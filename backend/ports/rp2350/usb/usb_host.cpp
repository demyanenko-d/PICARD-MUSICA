// USB-хост: подъём стека, разбор подключений и раздача отчётов HID.
//
// Порт здесь тонкий: что означает отчёт, решает devices/hid/usb_map.cpp -
// он платформы не касается и проверяется на ПК. Отсюда уходят только
// принятые байты.

#include "usb_host.h"

#include <cinttypes>
#include <iterator>

#include "tusb.h"

#include "hardware/irq.h"
#include "hardware/structs/usb.h"
#include "pico/platform.h"

#include "devices/hid/joystick.h"
#include "devices/hid/keyboard.h"
#include "devices/hid/mouse.h"
#include "devices/hid/report_map.h"
#include "devices/hid/usb_map.h"
#include "hal/host_signals.h"
#include "platform/log.h"
#include "platform/mono_time.h"
#include "platform/usb_host.h"
#include "usb_msc.h"

namespace rp2350::usb {
namespace {

// Ниже всех шинных обработчиков и ниже звука: ответ машине на чтение
// порта ждать USB не должен ни такта. Своего срока у хоста нет - кадр
// шины 1 мс, а стек и так разбирается в цикле.
constexpr uint8_t kIrqPriority = 0xC0;

// Чем занять ожидание внутри стека. Ставит цикл того ядра, которое ведёт
// шину; до установки ожидания просто крутятся.
void (*s_wait_service)(void*) = nullptr;
void* s_wait_user = nullptr;

// Роль интерфейса HID запоминается при подключении: в отчёте её нет, а
// спрашивать стек на каждом отчёте - лишние обращения в горячем месте.
enum class Role : uint8_t { None, Keyboard, Mouse, Gamepad };

constexpr uint8_t kMaxDev = CFG_TUH_DEVICE_MAX + 1; // адреса с единицы
constexpr uint8_t kMaxItf = CFG_TUH_HID;

Role s_role[kMaxDev][kMaxItf];
HostStats s_stats;

Role& role_of(uint8_t dev_addr, uint8_t idx) {
    static Role dummy = Role::None;
    if (dev_addr >= kMaxDev || idx >= kMaxItf) return dummy;
    return s_role[dev_addr][idx];
}

void count_role(Role r, int delta) {
    switch (r) {
        case Role::Keyboard: s_stats.hid_keyboards = static_cast<uint8_t>(s_stats.hid_keyboards + delta); break;
        case Role::Mouse: s_stats.hid_mice = static_cast<uint8_t>(s_stats.hid_mice + delta); break;
        case Role::Gamepad: s_stats.hid_gamepads = static_cast<uint8_t>(s_stats.hid_gamepads + delta); break;
        default: break;
    }
}

// --- Что видно в журнале ---
//
// Подключение само по себе ничего не доказывает: устройство могло
// перечислиться и замолчать. Поэтому печатается первый отчёт с каждого
// интерфейса целиком и дальше каждое изменение состояния - нажатие,
// отпускание, поворот джойстика. Мышь придерживается: она шлёт сотню
// отчётов в секунду, и без задержки журнал состоял бы из неё одной.

bool s_first_report[kMaxDev][kMaxItf];

// Карта полей интерфейса: что нашлось в его дескрипторе. Снимается один
// раз при подключении, дальше по ней разбирается каждый отчёт.
//
// Карт ровно столько, сколько стек держит интерфейсов HID сразу, а не по
// одной на каждую пару "адрес, интерфейс": вторых двадцать четыре, и
// заняты из них хорошо если две. Владелец слота помнится рядом.
constexpr uint8_t kMapSlots = CFG_TUH_HID;
constexpr uint8_t kNoOwner = 0xFF;

devices::hid::ReportMap s_map[kMapSlots];
uint8_t s_map_owner[kMapSlots];
devices::hid::ReportMap s_map_none; // ответ, когда слота нет

uint8_t owner_key(uint8_t dev_addr, uint8_t idx) {
    return static_cast<uint8_t>((dev_addr << 4) | (idx & 0x0F));
}

// Слот под подключившийся интерфейс: свой, если он уже был, иначе первый
// свободный. Слотов не осталось - карты не будет, отчёты пойдут мимо.
devices::hid::ReportMap* map_claim(uint8_t dev_addr, uint8_t idx) {
    const uint8_t key = owner_key(dev_addr, idx);
    for (uint8_t i = 0; i < kMapSlots; ++i) {
        if (s_map_owner[i] == key) return &s_map[i];
    }
    for (uint8_t i = 0; i < kMapSlots; ++i) {
        if (s_map_owner[i] != kNoOwner) continue;
        s_map_owner[i] = key;
        return &s_map[i];
    }
    return nullptr;
}

void map_release(uint8_t dev_addr, uint8_t idx) {
    const uint8_t key = owner_key(dev_addr, idx);
    for (uint8_t i = 0; i < kMapSlots; ++i) {
        if (s_map_owner[i] == key) s_map_owner[i] = kNoOwner;
    }
}

const devices::hid::ReportMap& map_of(uint8_t dev_addr, uint8_t idx) {
    const uint8_t key = owner_key(dev_addr, idx);
    for (uint8_t i = 0; i < kMapSlots; ++i) {
        if (s_map_owner[i] == key) return s_map[i];
    }
    return s_map_none;
}

// Прошлый отчёт клавиатуры: модификаторы и шесть кодов. Печатать каждый -
// значит печатать и автоповтор, а он идёт пока клавишу держат.
uint8_t s_last_kbd[kMaxDev][kMaxItf][8];

uint8_t s_last_joy = 0;

// Мышь: накопленное между строками и время прошлой.
int32_t s_mouse_dx = 0;
int32_t s_mouse_dy = 0;
uint32_t s_mouse_reports = 0;
uint32_t s_mouse_foreign = 0; // отчёты с чужим номером: осей в них нет
uint8_t s_mouse_buttons = 0;
uint32_t s_mouse_logged_us = 0;
constexpr uint32_t kMousePeriodUs = 500u * 1000u;

void log_raw(const char* what, uint8_t dev_addr, uint8_t idx, const uint8_t* report, uint16_t len) {
    // Буфер журнала 192 байта, поэтому не больше шестнадцати байт отчёта:
    // длиннее у загрузочных протоколов не бывает, а у джойстика хватает и
    // этого, чтобы увидеть раскладку.
    char hex[3 * 16 + 1];
    uint16_t n = len > 16 ? 16 : len;
    for (uint16_t i = 0; i < n; ++i) {
        const uint8_t b = report[i];
        hex[i * 3] = "0123456789ABCDEF"[b >> 4];
        hex[i * 3 + 1] = "0123456789ABCDEF"[b & 0x0F];
        hex[i * 3 + 2] = ' ';
    }
    hex[n * 3] = '\0';
    debug_logf("usb: первый отчёт %s %u/%u, %u Б: %s\n", what, dev_addr, idx, len, hex);
}

void log_keyboard(uint8_t dev_addr, uint8_t idx, const uint8_t* report, uint16_t len) {
    if (len < 8 || dev_addr >= kMaxDev || idx >= kMaxItf) return;
    uint8_t* last = s_last_kbd[dev_addr][idx];
    bool same = true;
    for (uint8_t i = 0; i < 8; ++i) {
        if (last[i] != report[i]) same = false;
        last[i] = report[i];
    }
    if (same) return; // автоповтор: состояние то же

    char keys[3 * 6 + 1];
    uint8_t n = 0;
    for (uint8_t i = 2; i < 8; ++i) {
        if (report[i] == 0) continue;
        keys[n * 3] = "0123456789ABCDEF"[report[i] >> 4];
        keys[n * 3 + 1] = "0123456789ABCDEF"[report[i] & 0x0F];
        keys[n * 3 + 2] = ' ';
        ++n;
    }
    keys[n * 3] = '\0';
    if (n == 0 && report[0] == 0) {
        debug_logf("usb: клавиатура %u/%u - всё отпущено\n", dev_addr, idx);
        return;
    }
    debug_logf("usb: клавиатура %u/%u: модификаторы 0x%02X, коды %s\n", dev_addr, idx, report[0],
               n ? keys : "нет");
}

// Поля берутся по карте: у отчёта с номером первый байт - номер, и
// загрузочные смещения к нему неприменимы.
void log_mouse(const devices::hid::ReportMap& map, const uint8_t* report, uint16_t len) {
    int32_t dx = 0;
    int32_t dy = 0;
    const bool got_x = devices::hid::report_field_read(map.axis[static_cast<uint8_t>(devices::hid::Axis::X)], report,
                                                       len, dx);
    const bool got_y = devices::hid::report_field_read(map.axis[static_cast<uint8_t>(devices::hid::Axis::Y)], report,
                                                       len, dy);
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

    const uint32_t now = platform::mono_us();
    if (now - s_mouse_logged_us < kMousePeriodUs) return;
    s_mouse_logged_us = now;
    debug_logf("usb: мышь - отчётов %" PRIu32 " (чужих %" PRIu32 "), смещение %+" PRId32 " %+" PRId32
               ", кнопки 0x%02X, ZX x=%u y=%u к=0x%02X\n",
               s_mouse_reports, s_mouse_foreign, s_mouse_dx, s_mouse_dy, s_mouse_buttons,
               devices::hid::mouse_page()[devices::hid::kMouseHiX], devices::hid::mouse_page()[devices::hid::kMouseHiY],
               devices::hid::mouse_page()[devices::hid::kMouseHiButtons]);
    s_mouse_reports = 0;
    s_mouse_foreign = 0;
    s_mouse_dx = 0;
    s_mouse_dy = 0;
}

// Дескриптор байтами, один раз при подключении. Буфер строки 192 Б,
// отсюда 24 байта в строку.
void dump_descriptor(const uint8_t* desc, uint16_t len) {
    char line[80];
    for (uint16_t off = 0; off < len; off += 24u) {
        const uint16_t n = (len - off) < 24u ? static_cast<uint16_t>(len - off) : 24u;
        for (uint16_t i = 0; i < n; ++i) {
            static const char kHex[] = "0123456789ABCDEF";
            line[i * 3u] = kHex[desc[off + i] >> 4];
            line[i * 3u + 1u] = kHex[desc[off + i] & 0x0Fu];
            line[i * 3u + 2u] = ' ';
        }
        line[n * 3u] = '\0';
        debug_logf("usb:   дкр %03u: %s\n", off, line);
    }
}

// Разобранная карта: где у устройства оси и кнопки. Один раз, при
// подключении.
void dump_report_map(const devices::hid::ReportMap& map) {
    static const char* const kAxisName[] = {"X", "Y", "Z", "Rx", "Ry", "Rz", "Slider", "Dial", "Wheel"};
    // Имя на каждую ось: без этого лишняя ось читает за концом массива.
    static_assert(std::size(kAxisName) == devices::hid::kAxisCount, "имя на каждую ось");
    for (uint8_t i = 0; i < devices::hid::kAxisCount; ++i) {
        const devices::hid::Field& f = map.axis[i];
        if (!f.present()) continue;
        debug_logf("usb:   ось %s - отчёт %u, разряд %u, ширина %u, границы %" PRId32 "..%" PRId32 "\n", kAxisName[i],
                   f.report_id, f.bit_offset, f.bit_size, f.logical_min, f.logical_max);
    }
    for (uint8_t i = 0; i < map.button_count && i < 4u; ++i) {
        debug_logf("usb:   кнопка %u - отчёт %u, разряд %u\n", i, map.button[i].report_id, map.button[i].bit_offset);
    }
}

void log_joystick() {
    const uint8_t now = devices::hid::joystick_state();
    if (now == s_last_joy) return;
    s_last_joy = now;
    debug_logf("usb: джойстик 0x%02X%s%s%s%s%s\n", now, (now & devices::hid::kJoyUp) ? " вверх" : "",
               (now & devices::hid::kJoyDown) ? " вниз" : "", (now & devices::hid::kJoyLeft) ? " влево" : "",
               (now & devices::hid::kJoyRight) ? " вправо" : "", (now & devices::hid::kJoyFire) ? " огонь" : "");
}

// Сброс шины USB: SE0 на линиях. Дольше 10 мс он возвращает любое
// устройство в состояние Default с адресом 0 (спецификация USB 2.0,
// 7.1.7.5).
//
// Нужен потому, что питание на разъёме не пропадает никогда - отключать
// его плата не умеет. После перезапуска микроконтроллера (тройка с
// клавиатуры, сброс с отладчика, отказ) устройство остаётся с прежним
// адресом и на обращения к нулевому не отвечает: перечисление не идёт,
// клавиатура мертва до снятия питания со всей машины.
//
// Стек этого не делает: hcd_port_reset у порта RP2350 - пустая заглушка,
// шинный сброс он не выдаёт ни разу за всё перечисление.
//
// Разряд самоочищающийся, длительность выдерживает сама SIE. Сколько она
// держит - в паспорте не сказано, поэтому замеряем и печатаем: короче 10
// мс означало бы, что сброса не было.
constexpr uint32_t kBusResetWaitUs = 60000;  // предел ожидания самоочистки
constexpr uint32_t kBusResetRecoveryUs = 20000; // TRSTRCY, с запасом

void bus_reset() {
    const uint32_t start = platform::mono_us();
    hw_set_bits(&usb_hw->sie_ctrl, USB_SIE_CTRL_RESET_BUS_BITS);
    while ((usb_hw->sie_ctrl & USB_SIE_CTRL_RESET_BUS_BITS) != 0u) {
        if (platform::mono_us() - start > kBusResetWaitUs) break;
    }
    const uint32_t held = platform::mono_us() - start;
    // Машина на этом месте ещё стоит в сбросе: release_host_reset() идёт
    // позже, из цикла сеанса. Ждать здесь можно.
    const uint32_t settle = platform::mono_us();
    while (platform::mono_us() - settle < kBusResetRecoveryUs) {
        tight_loop_contents();
    }
    debug_logf("usb: сброс шины, SE0 держался %" PRIu32 " мкс\n", held);
}

} // namespace

void host_init() {
    for (auto& dev : s_role) {
        for (Role& r : dev) r = Role::None;
    }
    for (uint8_t& o : s_map_owner) o = kNoOwner;
    s_stats = HostStats{};

    if (!tuh_init(0)) {
        debug_log("usb: хост не поднялся\n");
        return;
    }
    // Приоритет - после tuh_init: обработчик регистрирует он.
    irq_set_priority(USBCTRL_IRQ, kIrqPriority);
    // После tuh_init: до него контроллер не в режиме хоста и шину не ведёт.
    // До первого витка стека: событие подключения лежит в очереди и
    // разбирается позже, так что перечисление начнётся уже по сброшенной
    // шине.
    bus_reset();
    debug_log("usb: хост поднят на встроенном контроллере\n");
}

// --- Отчёт не может пропасть насовсем ---
//
// Каждый следующий отчёт заказывается из обработчика принятого, и заказ
// может не пройти: конечная точка бывает занята. Один непрошедший заказ -
// и интерфейс молчит до переподключения, а матрица так и держит то, что
// пришло последним. Отсюда залипание клавиши, и лечится оно только
// повторным заказом.
//
// Признак - конечная точка свободна: значит отчёта никто не ждёт. Заказ
// стоит копейки и берётся каждым витком, поэтому пропажа живёт один
// оборот цикла, а не до конца сеанса.
static void rearm_reports() {
    for (uint8_t dev = 0; dev < kMaxDev; ++dev) {
        for (uint8_t idx = 0; idx < kMaxItf; ++idx) {
            if (s_role[dev][idx] == Role::None) continue;
            if (!tuh_hid_receive_ready(dev, idx)) continue;
            if (tuh_hid_receive_report(dev, idx)) ++s_stats.rearms;
        }
    }
}

void host_task() {
    const uint32_t start = platform::mono_us();
    tuh_task();
    msc_task();
    rearm_reports();
    // Сброс и NMI подаёт клавиатура отсюда же, а держатся они по сроку.
    rp2350::hal::host_signals_tick();
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
    s_wait_user = user;
}

HostStats host_stats() {
    HostStats s = s_stats;
    s.msc_drives = msc_drive_count();
    return s;
}

void host_log_health() {
    static uint32_t s_reported = 0;
    const HostStats s = host_stats();
    // Прирост важнее суммы: по нему видно, идёт поток или устройство
    // подключено и молчит.
    const uint32_t delta = s.reports - s_reported;
    s_reported = s.reports;
    debug_logf("usb: клавиатур %u, мышей %u, джойстиков %u, дисков %u | отчётов %" PRIu32 " (+%" PRIu32
               ") | виток макс %" PRIu32 " мкс\n",
               s.hid_keyboards, s.hid_mice, s.hid_gamepads, s.msc_drives, s.reports, delta, s.task_max_us);
    // Печатается только когда есть что печатать: в норме сторож молчит.
    if (s.rearms != 0) {
        debug_logf("usb: сторож восстановил заказов %" PRIu32 "\n", s.rearms);
    }
}

} // namespace rp2350::usb

// Часы стека: TinyUSB просит их у приложения, когда собран без своего
// слоя платы. Единственное место, где он смотрит на время, - паузы
// перечисления.
extern "C" uint32_t tusb_time_millis_api(void) {
    return platform::mono_us() / 1000u;
}

// Пауза перечисления. Своя вместо готовой: та крутит пустой цикл, а пауза
// идёт миллисекундами на ядре, которое ведёт шину. Здесь то же ожидание
// занято обслуживанием шины - как ожидание обмена с флешкой.
//
// Считается по микросекундному счётчику, а не по миллисекундному: у того
// шаг в целую миллисекунду, и пауза в 1 мс выходила бы от нуля до двух.
extern "C" void tusb_time_delay_ms_api(uint32_t ms) {
    const uint32_t start = platform::mono_us();
    const uint32_t need = ms * 1000u;
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

extern "C" {

void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid = 0;
    uint16_t pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    debug_logf("usb: подключено устройство %u, %04x:%04x\n", dev_addr, vid, pid);
}

void tuh_umount_cb(uint8_t dev_addr) {
    debug_logf("usb: отключено устройство %u\n", dev_addr);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t* desc, uint16_t desc_len) {
    using rp2350::usb::Role;
    const uint8_t proto = tuh_hid_interface_protocol(dev_addr, idx);
    Role role = Role::Gamepad;
    const char* name = "джойстик";
    if (proto == HID_ITF_PROTOCOL_KEYBOARD) {
        role = Role::Keyboard;
        name = "клавиатура";
        devices::hid::keyboard_attach();
    } else if (proto == HID_ITF_PROTOCOL_MOUSE) {
        role = Role::Mouse;
        name = "мышь";
        devices::hid::mouse_attach();
    } else {
        devices::hid::joystick_attach();
    }
    rp2350::usb::role_of(dev_addr, idx) = role;
    rp2350::usb::count_role(role, +1);
    // Первый отчёт печатается заново после каждого подключения: воткнули
    // то же устройство - должно быть видно, что оно ожило.
    if (dev_addr < rp2350::usb::kMaxDev && idx < rp2350::usb::kMaxItf) {
        rp2350::usb::s_first_report[dev_addr][idx] = false;
        for (uint8_t& b : rp2350::usb::s_last_kbd[dev_addr][idx]) b = 0;
    }
    // Дескриптор разбирается здесь и только здесь: таблица предметов
    // разборщика велика, а нужна из неё выжимка в полсотни байт.
    devices::hid::ReportMap* slot = rp2350::usb::map_claim(dev_addr, idx);
    if (slot == nullptr) {
        debug_logf("usb: свободных карт нет, интерфейс %u устройства %u без разбора\n", idx, dev_addr);
        return;
    }
    devices::hid::ReportMap& map = *slot;
    const bool parsed = devices::hid::report_map_build(desc, desc_len, map);
    if (role == Role::Mouse && !map.axis[static_cast<uint8_t>(devices::hid::Axis::X)].present()) {
        // Дескриптор не разобрался или осей в нём нет: раскладка
        // загрузочной мыши задана спецификацией, ею и пользуемся.
        devices::hid::report_map_boot_mouse(map);
    }

    debug_logf("usb: %s на устройстве %u, интерфейс %u\n", name, dev_addr, idx);
    debug_logf("usb: дескриптор %u Б %s | оси %u, шляпка %u, кнопок %u, номера отчётов %u\n", desc_len,
               parsed ? "разобран" : "НЕ РАЗОБРАН", map.has_axes() ? 1u : 0u, map.hat.present() ? 1u : 0u,
               map.button_count, map.uses_report_ids ? 1u : 0u);
    rp2350::usb::dump_descriptor(desc, desc_len);
    rp2350::usb::dump_report_map(map);

    // Первый заказ отчёта: дальше каждый следующий заказывается из
    // обработчика принятого, иначе поток прекратится после первого.
    if (!tuh_hid_receive_report(dev_addr, idx)) {
        debug_logf("usb: отчёт с устройства %u не заказан\n", dev_addr);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
    using rp2350::usb::Role;
    Role& r = rp2350::usb::role_of(dev_addr, idx);
    // Отпустить всё: зажатое при отключении осталось бы висеть на шине.
    switch (r) {
        case Role::Keyboard:
            devices::hid::usb_map_keyboard_release_all();
            devices::hid::keyboard_detach();
            break;
        case Role::Mouse:
            devices::hid::mouse_detach();
            break;
        case Role::Gamepad:
            devices::hid::usb_map_gamepad_release_all();
            devices::hid::joystick_detach();
            break;
        default:
            return;
    }
    rp2350::usb::count_role(r, -1);
    r = Role::None;
    rp2350::usb::map_release(dev_addr, idx);
    debug_logf("usb: интерфейс %u устройства %u отключён\n", idx, dev_addr);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t* report, uint16_t len) {
    using rp2350::usb::Role;
    const Role role = rp2350::usb::role_of(dev_addr, idx);

    // Первый отчёт печатается целиком: он доказывает, что труба работает от
    // устройства до нас, и сразу показывает раскладку - у джойстиков она
    // своя у каждого.
    if (dev_addr < rp2350::usb::kMaxDev && idx < rp2350::usb::kMaxItf &&
        !rp2350::usb::s_first_report[dev_addr][idx]) {
        rp2350::usb::s_first_report[dev_addr][idx] = true;
        const char* what = role == Role::Keyboard  ? "клавиатуры"
                           : role == Role::Mouse   ? "мыши"
                           : role == Role::Gamepad ? "джойстика"
                                                   : "устройства";
        rp2350::usb::log_raw(what, dev_addr, idx, report, len);
    }

    switch (role) {
        case Role::Keyboard:
            devices::hid::usb_map_keyboard(rp2350::usb::map_of(dev_addr, idx), report, len);
            rp2350::usb::log_keyboard(dev_addr, idx, report, len);
            break;
        case Role::Mouse:
            devices::hid::usb_map_mouse(rp2350::usb::map_of(dev_addr, idx), report, len);
            rp2350::usb::log_mouse(rp2350::usb::map_of(dev_addr, idx), report, len);
            break;
        case Role::Gamepad:
            devices::hid::usb_map_gamepad(rp2350::usb::map_of(dev_addr, idx), report, len);
            rp2350::usb::log_joystick();
            break;
        default: break;
    }
    ++rp2350::usb::s_stats.reports;

    // Заказ следующего - здесь: без него устройство замолкает. Не прошёл -
    // подберёт сторожевой виток, поэтому возврат не проверяется.
    tuh_hid_receive_report(dev_addr, idx);
}


} // extern "C"
