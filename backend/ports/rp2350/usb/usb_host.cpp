// SPDX-License-Identifier: MIT
// USB-хост: подъём стека, разбор подключений и раздача отчётов HID.
//
// Порт здесь тонкий: что означает отчёт, решают устройства -
// они платформы не касаются и проверяются на ПК. Отсюда уходят только
// принятые байты.

#include "usb_host.h"

#include <cinttypes>
#include <iterator>

#include "hardware/irq.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/watchdog.h"
#include "pico.h"

#include "tusb.h"

#include "platform/boot_mode.h"
#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"
#include "platform/usb_host.h"

#include "devices/config/config_service.h"
#include "devices/hid/joystick.h"
#include "devices/hid/keyboard.h"
#include "devices/hid/mouse.h"
#include "devices/hid/report_map.h"
#include "devices/hid/usb_map.h"
#include "devices/zcontroller/zcontroller.h"

#include "hw_config.h" // IRQ_PRIO_USB

#include "bus.h" // port_answer_state
#include "hal/host_signals.h"
#include "usb_msc.h"

// Счётчик драйвера хоста TinyUSB: управляющие обмены, оборванные сроком.
extern "C" volatile uint32_t soundsinth_hcd_ctrl_timeouts;
extern "C" volatile uint32_t soundsinth_hcd_ctrl_cut_req;
extern "C" volatile uint32_t soundsinth_hcd_ctrl_cut_value;

namespace rp2350::usb {
namespace {

// Приоритет прерывания - bus::IRQ_PRIO_USB: ниже всех шинных обработчиков
// и ниже звука, ответ машине на чтение порта ждать USB не должен ни такта.
// Своего срока у хоста нет - кадр шины 1 мс, а стек и так разбирается в
// цикле.

// Чем занять ожидание внутри стека. Ставит цикл того ядра, которое ведёт
// шину; до установки ожидания просто крутятся.
void (*s_wait_service)(void*) = nullptr;
void* s_wait_user             = nullptr;

// Роль интерфейса HID запоминается при подключении: в отчёте её нет, а
// спрашивать стек на каждом отчёте - лишние обращения в горячем месте.
enum class Role : uint8_t { None, Keyboard, Mouse, Gamepad };

// Состояние интерфейса HID - массивами по idx обратных вызовов. В TinyUSB
// 0.21.0 idx - номер в общем массиве интерфейсов HID стека (CFG_TUH_HID на
// все устройства), а не номер интерфейса внутри устройства. Сменилась
// версия TinyUSB - проверить заново.
constexpr uint8_t kItfCount = CFG_TUH_HID;

Role s_role[kItfCount];
uint8_t s_dev[kItfCount]; // адрес устройства: повтору заказа нужна пара
HostStats s_stats;

void count_role(Role r, int delta) {
    switch (r) {
        case Role::Keyboard:
            s_stats.hid_keyboards = static_cast<uint8_t>(s_stats.hid_keyboards + delta);
            break;
        case Role::Mouse:
            s_stats.hid_mice = static_cast<uint8_t>(s_stats.hid_mice + delta);
            break;
        case Role::Gamepad:
            s_stats.hid_gamepads = static_cast<uint8_t>(s_stats.hid_gamepads + delta);
            break;
        default:
            break;
    }
}

// Сколько интерфейсов держат роль. Порт устройства один на роль, и снимать
// его можно только с последним: вынули одну клавиатуру - пульт или вторая
// клавиатура обязаны работать дальше.
uint8_t role_count(Role r) {
    switch (r) {
        case Role::Keyboard:
            return s_stats.hid_keyboards;
        case Role::Mouse:
            return s_stats.hid_mice;
        case Role::Gamepad:
            return s_stats.hid_gamepads;
        default:
            return 0;
    }
}

// --- Что видно в журнале ---
//
// Подключение само по себе ничего не доказывает: устройство могло
// перечислиться и замолчать. Поэтому печатается первый отчёт с каждого
// интерфейса целиком и дальше каждое изменение состояния - нажатие,
// отпускание, поворот джойстика. Мышь придерживается: она шлёт сотню
// отчётов в секунду, и без задержки журнал состоял бы из неё одной.

// Карта полей интерфейса: что нашлось в его дескрипторе. Снимается один
// раз при подключении, дальше по ней разбирается каждый отчёт.
devices::hid::ReportMap s_map[kItfCount];
static_assert(std::size(s_map) == CFG_TUH_HID, "a map for every HID interface of the stack");

// Напечатан ли первый отчёт этого интерфейса, и прошлый отчёт
// клавиатуры: модификаторы и шесть кодов. Печатать каждый - значит
// печатать и автоповтор, а он идёт пока клавишу держат.
//
// При отпускании интерфейса чистится, иначе следующая клавиатура на том же
// idx получила бы чужой прошлый отчёт и первое нажатие не напечаталось бы.
bool s_first_report[kItfCount];
uint8_t s_last_kbd[kItfCount][8];

void itf_forget(uint8_t idx) {
    s_first_report[idx] = false;
    for (uint8_t& b : s_last_kbd[idx]) {
        b = 0;
    }
}

uint8_t s_last_joy = 0;

// Мышь: накопленное между строками и время прошлой.
int32_t s_mouse_dx                = 0;
int32_t s_mouse_dy                = 0;
uint32_t s_mouse_reports          = 0;
uint32_t s_mouse_foreign          = 0; // отчёты с чужим номером: осей в них нет
uint8_t s_mouse_buttons           = 0;
uint32_t s_mouse_prev_us          = 0;
uint32_t s_mouse_gap_max_us       = 0;
uint32_t s_mouse_logged_us        = 0;
constexpr uint32_t kMousePeriodUs = 500u * 1000u;

// Байты в шестнадцатеричный вид: две цифры и пробел на байт, в конце ноль.
// Не влезающие в cap байты отбрасываются. Возвращает, сколько байт выведено.
uint16_t hex_bytes(char* out, size_t cap, const uint8_t* p, uint16_t n) {
    static const char kHex[] = "0123456789ABCDEF";
    const size_t fit         = cap == 0 ? 0 : (cap - 1u) / 3u;
    if (n > fit) n = static_cast<uint16_t>(fit);
    for (uint16_t i = 0; i < n; ++i) {
        out[i * 3u]      = kHex[p[i] >> 4];
        out[i * 3u + 1u] = kHex[p[i] & 0x0fu];
        out[i * 3u + 2u] = ' ';
    }
    if (cap != 0) out[n * 3u] = '\0';
    return n;
}

const char* role_name(Role r) {
    switch (r) {
        case Role::Keyboard:
            return "keyboard";
        case Role::Mouse:
            return "mouse";
        case Role::Gamepad:
            return "gamepad";
        default:
            return "device";
    }
}

void log_raw(const char* what, uint8_t dev_addr, uint8_t idx, const uint8_t* report, uint16_t len) {
    // Буфер журнала 192 байта, поэтому не больше шестнадцати байт отчёта:
    // длиннее у загрузочных протоколов не бывает, а у джойстика хватает и
    // этого, чтобы увидеть раскладку.
    char hex[3 * 16 + 1];
    hex_bytes(hex, sizeof(hex), report, len);
    debug_logf("usb: first report %s %u/%u, %u B: %s\n", what, dev_addr, idx, len, hex);
}

void log_keyboard(uint8_t dev_addr, uint8_t idx, const uint8_t* report, uint16_t len) {
    if (len < 8) return;
    uint8_t* last = s_last_kbd[idx];
    bool same     = true;
    for (uint8_t i = 0; i < 8; ++i) {
        if (last[i] != report[i]) same = false;
        last[i] = report[i];
    }
    if (same) return; // автоповтор: состояние то же

    uint8_t codes[6];
    uint8_t n = 0;
    for (uint8_t i = 2; i < 8; ++i) {
        if (report[i] != 0) codes[n++] = report[i];
    }
    char keys[3 * 6 + 1];
    hex_bytes(keys, sizeof(keys), codes, n);
    if (n == 0 && report[0] == 0) {
        debug_logf("usb: keyboard %u/%u - all released\n", dev_addr, idx);
        return;
    }
    debug_logf("usb: keyboard %u/%u: modifiers 0x%02X, codes %s\n", dev_addr, idx, report[0], n ? keys : "none");
}

// Поля берутся по карте: у отчёта с номером первый байт - номер, и
// загрузочные смещения к нему неприменимы.
void log_mouse(const devices::hid::ReportMap& map, const uint8_t* report, uint16_t len) {
    int32_t dx       = 0;
    int32_t dy       = 0;
    const bool got_x = devices::hid::report_field_read(map.field(devices::hid::Axis::X), report, len, dx);
    const bool got_y = devices::hid::report_field_read(map.field(devices::hid::Axis::Y), report, len, dy);
    if (got_x) s_mouse_dx += dx;
    if (got_y) s_mouse_dy += dy;
    // Отчёт не того номера - он не про мышь.
    if (!got_x && !got_y) ++s_mouse_foreign;
    uint8_t b = 0;
    for (uint8_t i = 0; i < map.button_count && i < 8u; ++i) {
        if (devices::hid::report_button_read(map.button[i], report, len)) b |= static_cast<uint8_t>(1u << i);
    }
    s_mouse_buttons = b;
    ++s_mouse_reports;

    // Наибольший разрыв между отчётами. Медленное движение "залипает" либо
    // от округления, либо оттого, что отчёты не доходят; округление
    // ничего не теряет (остаток точный), так что отличает их только это
    // число. Мышь шлёт раз в 8 мс - всё, что заметно больше, и есть помеха.
    const uint32_t now = platform::mono_us();
    if (s_mouse_prev_us != 0) {
        const uint32_t gap = now - s_mouse_prev_us;
        if (gap > s_mouse_gap_max_us) s_mouse_gap_max_us = gap;
    }
    s_mouse_prev_us = now;

    if (now - s_mouse_logged_us < kMousePeriodUs) return;
    s_mouse_logged_us = now;
    debug_logf("usb: mouse - reports %" PRIu32 " (foreign %" PRIu32 "), delta %+" PRId32 " %+" PRId32 ", buttons 0x%02X, ZX x=%u y=%u b=0x%02X\n",
               s_mouse_reports, s_mouse_foreign, s_mouse_dx, s_mouse_dy, s_mouse_buttons, devices::hid::mouse_page()[devices::hid::kMouseHiX],
               devices::hid::mouse_page()[devices::hid::kMouseHiY], devices::hid::mouse_page()[devices::hid::kMouseHiButtons]);
    debug_logf("usb: mouse - longest gap between reports %" PRIu32 " us\n", s_mouse_gap_max_us);
    s_mouse_gap_max_us = 0;
    s_mouse_reports    = 0;
    s_mouse_foreign    = 0;
    s_mouse_dx         = 0;
    s_mouse_dy         = 0;
}

// Дескриптор байтами, один раз при подключении. Буфер строки 192 Б,
// отсюда 24 байта в строку.
void dump_descriptor(const uint8_t* desc, uint16_t len) {
    char line[80];
    for (uint16_t off = 0; off < len; off += 24u) {
        const uint16_t left = static_cast<uint16_t>(len - off);
        const uint16_t n    = left < 24u ? left : 24u;
        hex_bytes(line, sizeof(line), desc + off, n);
        debug_logf("usb:   dsc %03u: %s\n", off, line);
    }
}

// Разобранная карта: где у устройства оси и кнопки. Один раз, при
// подключении.
void dump_report_map(const devices::hid::ReportMap& map) {
    static const char* const kAxisName[] = {"X", "Y", "Z", "Rx", "Ry", "Rz", "Slider", "Dial", "Wheel"};
    // Имя на каждую ось: без этого лишняя ось читает за концом массива.
    static_assert(std::size(kAxisName) == devices::hid::kAxisCount, "a name for every axis");
    for (uint8_t i = 0; i < devices::hid::kAxisCount; ++i) {
        const devices::hid::Field& f = map.axis[i];
        if (!f.present()) continue;
        debug_logf("usb:   axis %s - report %u, bit %u, width %u, range %" PRId32 "..%" PRId32 "\n", kAxisName[i], f.report_id, f.bit_offset, f.bit_size,
                   f.logical_min, f.logical_max);
    }
    for (uint8_t i = 0; i < map.button_count && i < 4u; ++i) {
        debug_logf("usb:   button %u - report %u, bit %u\n", i, map.button[i].report_id, map.button[i].bit_offset);
    }
}

void log_joystick() {
    const uint8_t now = devices::hid::joystick_state();
    if (now == s_last_joy) return;
    s_last_joy = now;
    debug_logf("usb: gamepad 0x%02X%s%s%s%s%s\n", now, (now & devices::hid::kJoyUp) ? " up" : "", (now & devices::hid::kJoyDown) ? " down" : "",
               (now & devices::hid::kJoyLeft) ? " left" : "", (now & devices::hid::kJoyRight) ? " right" : "", (now & devices::hid::kJoyFire) ? " fire" : "");
}

// Сброс шины USB: SE0 дольше 10 мс возвращает устройство в Default с адресом 0.
// Питание на разъёме не снимается, и после перезапуска платы устройство
// остаётся с прежним адресом. hcd_port_reset порта RP2350 пуст, а
// SIE_CTRL.RESET_BUS держит SE0 1 мкс, поэтому SE0 ставится прямым управлением PHY.
constexpr uint32_t kBusResetHoldUs     = 15000; // больше 10 мс по спецификации
constexpr uint32_t kBusResetRecoveryUs = 20000; // TRSTRCY, с запасом

void bus_reset() {
    // Одиночный режим обязателен: при TX_DIFFMODE=1 линия TX_DM
    // игнорируется, и нуля на обеих не выйдет.
    constexpr uint32_t kDrive    = USB_USBPHY_DIRECT_TX_DP_OE_BITS | USB_USBPHY_DIRECT_TX_DM_OE_BITS;
    constexpr uint32_t kLow      = USB_USBPHY_DIRECT_TX_DP_BITS | USB_USBPHY_DIRECT_TX_DM_BITS | USB_USBPHY_DIRECT_TX_DIFFMODE_BITS;
    constexpr uint32_t kOverride = USB_USBPHY_DIRECT_OVERRIDE_TX_DP_OE_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_DM_OE_OVERRIDE_EN_BITS |
                                   USB_USBPHY_DIRECT_OVERRIDE_TX_DP_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_DM_OVERRIDE_EN_BITS |
                                   USB_USBPHY_DIRECT_OVERRIDE_TX_DIFFMODE_OVERRIDE_EN_BITS;

    const uint32_t direct_was   = usb_hw->phy_direct;
    const uint32_t override_was = usb_hw->phy_direct_override;

    // Выходы включены, уровни и режим - нули. Значения выставляются до
    // того, как override отдаёт линии нам: иначе PHY на миг получит чужое.
    hw_write_masked(&usb_hw->phy_direct, kDrive, kDrive | kLow);
    hw_set_bits(&usb_hw->phy_direct_override, kOverride);

    // Машина на этом месте ещё стоит в сбросе: release_host_reset() идёт
    // позже, из цикла сеанса. Ждать здесь можно.
    //
    // Повторять этот сброс из витка нельзя - замером: HCD падает
    // (buf_ctrl already available), потому что сброс выдёргивает шину из-под
    // обмена, который стек уже начал. Единственное безопасное место - здесь,
    // до первого витка.
    const uint32_t start = platform::mono_us();
    while (platform::mono_us() - start < kBusResetHoldUs) {
        tight_loop_contents();
    }
    const uint32_t held = platform::mono_us() - start;

    // Отпускаем в обратном порядке: сперва вернуть линии SIE, потом
    // прежние значения полей.
    usb_hw->phy_direct_override = override_was;
    usb_hw->phy_direct          = direct_was;

    const uint32_t settle = platform::mono_us();
    while (platform::mono_us() - settle < kBusResetRecoveryUs) {
        tight_loop_contents();
    }
    debug_logf("usb: bus reset, SE0 held %" PRIu32 " us\n", held);
}

// Состояние сторожа перечисления; сам сторож - ниже, рядом с витком.
constexpr uint32_t kRetryAfterUs = 2500000;
constexpr uint8_t kRetryLimit    = 2;

uint32_t s_up_us   = 0;
uint8_t s_retries  = 0;
bool s_retry_armed = false;
bool s_host_up     = false; // host_init звали: без него tuh_task не крутится

} // namespace

void host_init() {
    for (Role& r : s_role) {
        r = Role::None;
    }
    for (uint8_t& d : s_dev) {
        d = 0;
    }
    s_stats   = HostStats{};
    s_host_up = true;

    // tuh_init(0) с 0.21.0 объявлен устаревшим: роль корневого порта теперь
    // задаётся явно, и одним вызовом поднимается любая сторона стека.
    const tusb_rhport_init_t rh = {
        .role  = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_AUTO,
    };
    if (!tusb_init(0, &rh)) {
        debug_log("usb: host did not come up\n");
        return;
    }
    // Приоритет - после подъёма: обработчик регистрирует он.
    irq_set_priority(USBCTRL_IRQ, bus::IRQ_PRIO_USB);
    // После подъёма: до него контроллер не в режиме хоста и шину не ведёт.
    // До первого витка стека: событие подключения лежит в очереди и
    // разбирается позже, так что перечисление начнётся уже по сброшенной
    // шине.
    bus_reset();
    s_up_us = platform::mono_us();
    // Повтор сторожим только после тёплого перезапуска: по замеру после
    // снятия питания перечисление проходит всегда, и трогать работающее
    // незачем. Перезапуск сторожем (вход в конфигуратор, выход из него)
    // CHIP_RESET не обновляет, и HAD_POR от включения держится через все
    // такие перезапуски; их видно по причине в регистре сторожа.
    s_retry_armed = (powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_POR_BITS) == 0u || watchdog_hw->reason != 0u;
    // Метка времени: от сброса до готовности хоста - это наша загрузка, а
    // дальше идёт перечисление, которым правит устройство. Без двух меток
    // "долго до клавиатуры" не поделить между ними.
    debug_logf("usb: host up on the built-in controller, t=%" PRIu32 " ms\n", s_up_us / 1000u);
}

// --- Сторожевой повтор заказа отчётов ---
//
// Каждый следующий отчёт заказывается из обработчика принятого, и заказ
// может не пройти: конечная точка бывает занята. Один непрошедший заказ -
// и интерфейс молчит до переподключения, а матрица так и держит то, что
// пришло последним. Отсюда залипание клавиши, и лечится оно только
// повторным заказом.
//
// Признак - конечная точка свободна: значит отчёта никто не ждёт. Проверка
// раз в kRearmPeriodUs, поэтому пропажа живёт не дольше 2 мс, а не до
// переподключения.
namespace {
// По сроку, а не каждым витком: виток при работающем плагине просыпается на
// каждом прерывании шины, а сторож ловит единственный непрошедший заказ, и
// задержка в миллисекунды на нём не сказывается.
constexpr uint32_t kRearmPeriodUs = 2000;

void rearm_reports() {
    static uint32_t s_last_us = 0;
    const uint32_t now        = platform::mono_us();
    if (now - s_last_us < kRearmPeriodUs) return;
    s_last_us = now;
    // Циклом, а не развёрткой: проход раз в 2 мс, а развёртка - 40 байт SRAM.
#pragma GCC unroll 1
    for (uint8_t idx = 0; idx < kItfCount; ++idx) {
        if (s_role[idx] == Role::None) continue;
        if (!tuh_hid_receive_ready(s_dev[idx], idx)) continue;
        if (tuh_hid_receive_report(s_dev[idx], idx)) ++s_stats.rearms;
    }
}
// --- Повтор подъёма стека после тёплого перезапуска ---
//
// Устройства за концентратором после тёплого перезапуска сброса не получают,
// и пропустившее перечисление не возвращается. Поэтому стек поднимается заново:
// tuh_deinit останавливает все обмены; один сброс шины без него роняет HCD
// в панику. Попытка стоит 35 мс (SE0 15 мс и восстановление 20 мс), на это
// время цикл Core1 стоит; снятие и регистрация обработчика USB гасят
// прерывания Core1 (PRIMASK).

// Чего ждут настройки и чего не пришло. Мышь и джойстик сюда не входят:
// их отсутствие ничего не ломает, а повтор из-за неподключенной мыши
// стоил бы перечисления на каждой загрузке.
// Флешку ждут, только если её берёт включённый эмулятор, и не в
// конфигураторе: ему носитель не нужен.
bool expected_device_missing() {
    const soundsinth::config::Settings& cfg = devices::config::settings();
    if (soundsinth::config::keyboard_enabled(cfg) && s_stats.hid_keyboards == 0) return true;
    const bool usb_medium = !platform::config_rom_active() && soundsinth::config::usb_medium_needed(cfg);
    return usb_medium && msc_drive_count() == 0;
}

void retry_host_if_short() {
    if (!s_retry_armed || s_retries >= kRetryLimit) return;
    if (platform::mono_us() - s_up_us < kRetryAfterUs) return;
    if (!expected_device_missing()) {
        s_retry_armed = false; // всё пришло: больше не смотрим
        return;
    }
    // Перезапуск отключает все устройства, и исправные тоже. Если с флешки
    // уже читали, машина с неё грузится: без носителя она встанет, это
    // хуже, чем остаться без клавиатуры.
    if (msc_stats().read_cmds != 0) {
        s_retry_armed = false;
        debug_log("usb: a device is missing, but the drive is already in use - the host stack stays up\n");
        return;
    }
    ++s_retries;
    debug_logf("usb: expected device missing, restarting the host stack, attempt %u\n", s_retries);
    tuh_deinit(0);
    host_init();
}

// Тик платы, не стека USB: сроки сброса и NMI машины и перезапуск платы по
// F12 и Win+F12 (host_signals_tick), признак носителя Z-Controller. Идёт
// витком USB и только им: ожидание загрузки виток не крутит, и сигналы в
// это время не отпускаются.
SOUNDSINTH_ALWAYS_INLINE void board_tick() {
    rp2350::hal::host_signals_tick();
    devices::zcontroller::zcontroller_tick();
}

} // namespace

void SOUNDSINTH_HOT_PATH(host_task)() {
    if (!s_host_up) {
        // Стек не поднят: остаётся тик платы.
        board_tick();
        return;
    }
    const uint32_t start = platform::mono_us();
    tuh_task();
    msc_task();
    rearm_reports();
    // После витка, а не до: внутри стека обмен может идти, а повтор
    // разбирает контроллер целиком.
    retry_host_if_short();
    // После стека: сброс и NMI подаёт клавиатура этим же витком, а флешка
    // встаёт в msc_task выше, и машина узнаёт о ней тем же витком.
    board_tick();
    // Виток крутит ядро шины: сколько он занял, столько шина не получала
    // кооперативного обслуживания. Прерывания при этом идут - стек собран без
    // операционной системы и ничего не маскирует.
    const uint32_t spent = platform::mono_us() - start;
    if (spent > s_stats.task_max_us) s_stats.task_max_us = spent;
}

// Отдать время шине. Зовут ожидания внутри стека: обмен с флешкой и паузы
// перечисления. Виток стека отсюда не крутится - паузу стек берёт изнутри
// tuh_task(), и повторный вход был бы рекурсией.
void yield_to_bus() {
    if (s_wait_service != nullptr) s_wait_service(s_wait_user);
}

void set_wait_service(void (*fn)(void*), void* user) {
    s_wait_service = fn;
    s_wait_user    = user;
}

HostStats host_stats() {
    HostStats s  = s_stats;
    s.msc_drives = msc_drive_count();
    return s;
}

void host_log_health() {
    static uint32_t s_reported = 0;
    const HostStats s          = host_stats();
    // Прирост важнее суммы: по нему видно, идёт поток или устройство
    // подключено и молчит. Перезапуск стека обнуляет s_stats: сумма меньше
    // прошлой - прирост с нуля.
    const uint32_t delta = s.reports >= s_reported ? s.reports - s_reported : s.reports;
    s_reported           = s.reports;
    debug_logf("usb: keyboards %u, mice %u, gamepads %u, disks %u | reports %" PRIu32 " (+%" PRIu32 ") | task max %" PRIu32 " us\n", s.hid_keyboards,
               s.hid_mice, s.hid_gamepads, s.msc_drives, s.reports, delta, s.task_max_us);
    // Управляющие обмены, оборванные сроком драйвера хоста: устройство
    // отвечало NAK дольше срока. Строка - только когда такие были; последний
    // оборванный - устройство, запрос и этап.
    if (const uint32_t cut = soundsinth_hcd_ctrl_timeouts; cut != 0) {
        const uint32_t req = soundsinth_hcd_ctrl_cut_req;
        const uint32_t val = soundsinth_hcd_ctrl_cut_value;
        debug_logf("usb: control transfers cut by deadline %" PRIu32 ", last: device %u, request %02X/%02X, wValue %04X, wIndex %04X, stage ep %02X\n", cut,
                   static_cast<unsigned>(req >> 24), static_cast<unsigned>((req >> 16) & 0xFFu), static_cast<unsigned>((req >> 8) & 0xFFu),
                   static_cast<unsigned>(val & 0xFFFFu), static_cast<unsigned>(val >> 16), static_cast<unsigned>(req & 0xFFu));
    }
    // Нажатия по кодам без записи в раскладке - своей строкой и только когда
    // они есть: в строку выше не влезает, а при жалобе "клавиша не
    // нажимается" ненулевое число сразу указывает на раскладку, а не на
    // дескриптор и не на потерю отчёта.
    // Kempston: режим клавиатуры, кто держит порт и что порт 0x1F отвечает
    // на шине на самом деле (ячейка таблицы портов, а не состояние
    // устройства). Только когда порт кто-то держит или режим включён.
    {
        const bool arrows = devices::hid::usb_map_key_mode() == devices::hid::KeyMode::KempstonArrows;
        const bool keys   = devices::hid::joystick_keys_held();
        const bool pad    = devices::hid::joystick_pad_held();
        if (arrows || keys || pad) {
            const int32_t a = bus::port_answer_state(devices::hid::kJoystickPort);
            debug_logf("usb: kempston mode %s, held keys %u pad %u, bits %02X, port 1F answers %s%02X\n", arrows ? "arrows" : "normal", keys ? 1u : 0u,
                       pad ? 1u : 0u, devices::hid::joystick_state(), a == bus::kPortReleased ? "NOTHING " : (a == bus::kPortPaged ? "PAGE " : ""),
                       a < 0 ? 0u : static_cast<unsigned>(a));
        }
    }
    const uint32_t unmapped = devices::hid::usb_map_unmapped_presses();
    if (unmapped != 0) debug_logf("usb: keys without a keymap entry %" PRIu32 "\n", unmapped);
    // Печатается только когда есть что печатать: в норме сторож молчит.
    if (s.rearms != 0) {
        debug_logf("usb: watchdog re-armed transfers %" PRIu32 "\n", s.rearms);
    }
}

} // namespace rp2350::usb

// Часы стека: TinyUSB просит их у приложения, когда собран без своего
// слоя платы. Единственное место, где он смотрит на время, - паузы
// перечисления.
extern "C" uint32_t tusb_time_millis_api(void) {
    return platform::mono_ms();
}

// Пауза перечисления. Своя вместо готовой: та крутит пустой цикл, а пауза
// идёт миллисекундами на ядре, которое ведёт шину. Здесь то же ожидание
// занято обслуживанием шины - как ожидание обмена с флешкой.
//
// Считается по микросекундному счётчику, а не по миллисекундному: у того
// шаг в целую миллисекунду, и пауза в 1 мс выходила бы от нуля до двух.
extern "C" void tusb_time_delay_ms_api(uint32_t ms) {
    const uint32_t start = platform::mono_us();
    const uint32_t need  = ms * 1000u;
    while (platform::mono_us() - start < need) {
        rp2350::usb::yield_to_bus();
    }
}

void platform::usb_host_task() {
    rp2350::usb::host_task();
}

void platform::usb_host_set_wait_service(void (*fn)(void*), void* user) {
    rp2350::usb::set_wait_service(fn, user);
}

// --- Обратные вызовы TinyUSB ---
//
// Зовутся из tuh_task, то есть из цикла Core1, а не из прерывания.

namespace rp2350::usb {

extern "C" {

void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid = 0;
    uint16_t pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    debug_logf("usb: device %u attached, %04x:%04x, t=%" PRIu32 " ms\n", dev_addr, vid, pid, platform::mono_ms());
}

void tuh_umount_cb(uint8_t dev_addr) {
    debug_logf("usb: device %u detached\n", dev_addr);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t* desc, uint16_t desc_len) {
    if (idx >= kItfCount) return;
    const soundsinth::config::Settings& cfg = devices::config::settings();
    const uint8_t proto                     = tuh_hid_interface_protocol(dev_addr, idx);
    Role role                               = Role::Gamepad;
    if (proto == HID_ITF_PROTOCOL_KEYBOARD) {
        role = Role::Keyboard;
        // Интерфейс берётся всегда, а на шину клавиатура выходит только по
        // настройке: Win+F12 обязан работать и при выключенной, иначе
        // вернуть её будет нечем. Сигнальные клавиши идут мимо матрицы.
        if (soundsinth::config::keyboard_enabled(cfg) || platform::config_rom_active()) {
            devices::hid::keyboard_attach();
        } else {
            debug_logf("usb: keyboard off the bus by settings, interface %u of device %u taken for the hotkeys only\n", idx, dev_addr);
        }
    } else if (proto == HID_ITF_PROTOCOL_MOUSE) {
        if (cfg.usb_mouse == 0) {
            debug_logf("usb: mouse disabled by settings, interface %u of device %u not taken\n", idx, dev_addr);
            return;
        }
        role = Role::Mouse;
        devices::hid::mouse_attach();
    } else {
        if (!devices::hid::joystick_allowed()) {
            debug_logf("usb: gamepad disabled by settings, interface %u of device %u not taken\n", idx, dev_addr);
            return;
        }
        devices::hid::joystick_attach();
    }
    s_role[idx] = role;
    s_dev[idx]  = dev_addr;
    count_role(role, +1);
    // Первый отчёт печатается заново после каждого подключения: воткнули
    // то же устройство - должно быть видно, что оно ожило.
    itf_forget(idx);
    // Дескриптор разбирается здесь и только здесь: таблица предметов
    // разборщика велика, а нужна из неё выжимка в полсотни байт.
    devices::hid::ReportMap& map = s_map[idx];
    const bool parsed            = devices::hid::report_map_build(desc, desc_len, map);
    if (role == Role::Mouse && !map.field(devices::hid::Axis::X).present()) {
        // Дескриптор не разобрался или осей в нём нет: раскладка
        // загрузочной мыши задана спецификацией, ею и пользуемся.
        devices::hid::report_map_boot_mouse(map);
    }

    debug_logf("usb: %s on device %u, interface %u, t=%" PRIu32 " ms\n", role_name(role), dev_addr, idx, platform::mono_ms());
    // Код отказа разборщика рядом со словом: потолок предметов (32) и номеров
    // отчёта (8) лечится не так, как незнакомая страница назначений, а по
    // одному "НЕ РАЗОБРАН" их не различить.
    debug_logf("usb: descriptor %u B %s (code %u) | axes %u, hat %u, buttons %u, report ids %u\n", desc_len, parsed ? "parsed" : "NOT PARSED",
               devices::hid::report_map_last_error(), map.has_axes() ? 1u : 0u, map.hat.present() ? 1u : 0u, map.button_count, map.uses_report_ids ? 1u : 0u);
    dump_descriptor(desc, desc_len);
    dump_report_map(map);

    // Первый заказ отчёта: дальше каждый следующий заказывается из
    // обработчика принятого, иначе поток прекратится после первого.
    if (!tuh_hid_receive_report(dev_addr, idx)) {
        debug_logf("usb: report from device %u not armed\n", dev_addr);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
    if (idx >= kItfCount) return;
    Role& r = s_role[idx];
    if (r == Role::None) return;
    count_role(r, -1);
    // Отпустить всё: зажатое при отключении осталось бы висеть на шине.
    // Оставшиеся устройства той же роли пришлют своё состояние следующим
    // отчётом. Порт снимается только с последним держателем роли.
    const bool last = role_count(r) == 0;
    switch (r) {
        case Role::Keyboard:
            devices::hid::usb_map_keyboard_release_all();
            if (last) devices::hid::keyboard_detach();
            break;
        case Role::Mouse:
            if (last) devices::hid::mouse_detach();
            break;
        case Role::Gamepad:
            devices::hid::usb_map_gamepad_release_all();
            if (last) devices::hid::joystick_detach();
            break;
        default:
            break;
    }
    r          = Role::None;
    s_dev[idx] = 0;
    itf_forget(idx);
    debug_logf("usb: interface %u of device %u detached\n", idx, dev_addr);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t* report, uint16_t len) {
    if (idx >= kItfCount) return;
    const Role role = s_role[idx];

    // Первый отчёт печатается целиком: по нему видно, что отчёты доходят от
    // устройства, и сразу показывает раскладку - у джойстиков она
    // своя у каждого.
    if (role != Role::None && !s_first_report[idx]) {
        s_first_report[idx] = true;
        log_raw(role_name(role), dev_addr, idx, report, len);
    }

    const devices::hid::ReportMap& map = s_map[idx];
    switch (role) {
        case Role::Keyboard:
            devices::hid::usb_map_keyboard(map, report, len);
            log_keyboard(dev_addr, idx, report, len);
            break;
        case Role::Mouse:
            devices::hid::usb_map_mouse(map, report, len);
            log_mouse(map, report, len);
            break;
        case Role::Gamepad:
            devices::hid::usb_map_gamepad(map, report, len);
            log_joystick();
            break;
        default:
            break;
    }
    ++s_stats.reports;

    // Заказ следующего - здесь: без него устройство замолкает. Не прошёл -
    // подберёт сторожевой виток, поэтому возврат не проверяется.
    tuh_hid_receive_report(dev_addr, idx);
}

} // extern "C"

} // namespace rp2350::usb
