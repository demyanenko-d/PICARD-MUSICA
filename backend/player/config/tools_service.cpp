// SPDX-License-Identifier: MIT
#include "player/config/tools_service.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "platform/compiler.h"
#include "platform/board_info.h"
#include "platform/mono_time.h"
#include "platform/usb_host.h"

#include "core/util/fnv1a.h"

#include "devices/hal/media.h"
#include "devices/hid/report_map.h"
#include "devices/sd/card_protocol.h"
#include "devices/storage/fat_check.h"
#include "devices/storage/storage.h"
#include "devices/storage/volumes.h"

#include "player/config/config_tools.h"
#include "player/config/config_service.h"
#include "player/config/tools_view.h"

namespace player::config {

using namespace devices;
namespace {

using hal::Medium;
using soundsinth::config::BoardFacts;
using soundsinth::config::CardFacts;
using soundsinth::config::kSdSpeedCount;
using soundsinth::config::kSdSpeedHz;
using soundsinth::config::kToolAlert;
using soundsinth::config::kToolKeyClose;
using soundsinth::config::kToolKeyDel;
using soundsinth::config::kToolKeyEnter;
using soundsinth::config::kToolKeyEsc;
using soundsinth::config::kToolKeyOpen;
using soundsinth::config::Settings;
using soundsinth::config::ToolId;
using soundsinth::config::ToolScreen;
using soundsinth::config::ToolState;
using soundsinth::config::VolumeFacts;

constexpr uint32_t kCheckRun    = 4;   // секторов FAT за оборот цикла
constexpr uint32_t kRedrawMs    = 200; // ход проверки на экран не чаще
constexpr uint32_t kRefreshMs   = 1000;
constexpr uint32_t kUsbShownMax = 7; // по две строки на устройство

constexpr uint32_t kSpeedRun      = 8; // секторов в пачке теста частоты
constexpr uint32_t kSpeedRunBytes = kSpeedRun * hal::kSectorBytes;

// Состояние экранов - тоже в памяти инструментов: в рабочей прошивке
// утилиты не стоят ни байта ОЗУ.
struct Check {
    CheckView view;
    Medium medium;
    uint32_t t0;
    uint32_t drawn_ms;
};

struct Speed {
    SpeedView view;
    uint32_t row;   // частота в работе
    uint32_t step;  // чтение в ней
    uint32_t rnd;   // место следующего чтения
    uint32_t hash;  // свёртка прочитанного
    uint32_t ref;   // она же на первой чистой строке
    bool has_ref;   // чистая строка уже была
    uint32_t fails; // неудачных чтений
    uint32_t faults0;
    uint32_t run_us; // время пачек
    uint32_t drawn_ms;
};

struct Hid {
    HidView view;
    uint32_t seen; // сумма отчётов прошлой отрисовки
    uint32_t drawn_ms;
};

// Память инструментов - в TrackScratch: трека в режиме конфигуратора нет.
struct ToolMem {
    uint8_t buf[1024]; // две копии сектора FAT
    uint8_t sector[hal::kSectorBytes];
    uint8_t run[kSpeedRunBytes];
    FATFS fs; // том карты для записи журнала HID, если его ещё никто не держит
    FIL file;
    char line[160];
    Check check;
    Speed speed;
    Hid hid;
};

// Занята при первом экране и дальше не сходит: всё ниже tools_serve зовёт
// только при s_mem != nullptr.
ToolMem* s_mem = nullptr;

bool tool_mem_take(soundsinth::memory::TrackScratch& scratch) {
    if (s_mem == nullptr) {
        uint8_t* p = soundsinth::memory::scratch_take(scratch, soundsinth::memory::Scratch::Tools, sizeof(ToolMem));
        if (p != nullptr) s_mem = new (p) ToolMem();
    }
    return s_mem != nullptr;
}

bool io_read(void* user, uint32_t lba, uint8_t* dst) {
    return storage::storage_tool_read(*static_cast<Medium*>(user), lba, dst);
}

bool io_write(void* user, uint32_t lba, const uint8_t* src) {
    return storage::storage_tool_write(*static_cast<Medium*>(user), lba, src);
}

// --- Проверка тома ---

ToolId s_open    = ToolId::None; // экран, который ведёт Core1
uint32_t s_drawn = 0;

hal::SectorIo io_of(Medium& m) {
    return hal::SectorIo{&io_read, &io_write, &m};
}

void render_check(ToolScreen& scr) {
    s_mem->check.view.present = hal::media_state(s_mem->check.medium).present;
    tool_render_check(scr, s_mem->check.view);
}

void check_start(ToolScreen& scr, Medium m) {
    s_mem->check           = Check{};
    s_mem->check.medium    = m;
    s_mem->check.view.usb  = m == Medium::Usb;
    s_mem->check.t0        = platform::mono_ms();
    CheckView& v           = s_mem->check.view;
    const hal::SectorIo io = io_of(s_mem->check.medium);
    if (!hal::media_state(m).present || !storage::fat_volume_open(io, s_mem->sector, v.vol)) {
        v.phase = CheckPhase::Failed;
    } else {
        v.phase = CheckPhase::Scan;
        storage::fat_check_begin(v.c);
    }
    render_check(scr);
}

void check_step(ToolScreen& scr) {
    CheckView& v = s_mem->check.view;
    if (v.phase != CheckPhase::Scan) return;
    const hal::SectorIo io = io_of(s_mem->check.medium);
    const bool more        = v.vol.fat_bits != 12 && storage::fat_check_step(io, v.vol, v.c, s_mem->buf, kCheckRun);
    const uint32_t now     = platform::mono_ms();
    if (!more) {
        v.phase   = CheckPhase::Done;
        v.took_ms = now - s_mem->check.t0;
    } else if (now - s_mem->check.drawn_ms < kRedrawMs) {
        return;
    }
    s_mem->check.drawn_ms = now;
    render_check(scr);
    scr.publish();
}

void check_fix(ToolScreen& scr) {
    CheckView& v = s_mem->check.view;
    if (v.phase != CheckPhase::Done || v.c.io_error || !storage::fat_fsinfo_wrong(v.vol, v.c)) return;
    const hal::SectorIo io = io_of(s_mem->check.medium);
    v.fixed                = storage::fat_fix_fsinfo(io, v.vol, v.c, s_mem->sector);
    v.fix_failed           = !v.fixed;
    render_check(scr);
    scr.publish();
}

// --- Тест частоты карты ---
//
// Частоты по возрастанию; на каждой - пачки CMD18 и одиночные чтения по
// одним и тем же местам карты, мимо кэша носителя. Сверка - свёрткой
// прочитанного с младшей частотой. Потом частота из настроек.

constexpr uint32_t kSpeedRuns    = 256; // пачек на частоту
constexpr uint32_t kSpeedSingles = 512; // одиночных чтений на частоту
constexpr uint32_t kSpeedSteps   = kSpeedRuns + kSpeedSingles;
constexpr uint32_t kSpeedPerTurn = 4; // чтений за оборот цикла
constexpr uint32_t kSpeedSeed    = 12345;
// ГПСЧ места чтения: линейный конгруэнтный, множитель и шаг из ANSI C.
constexpr uint32_t kLcgMul = 1103515245u;
constexpr uint32_t kLcgInc = 12345u;

// Экран проверки носителя: карта и флешка ведутся одним check_step.
bool is_check(ToolId t) {
    return t == ToolId::CardCheck || t == ToolId::UsbCheck;
}

void speed_restore() {
    (void)sd::sd_card_set_speed(kSdSpeedHz[settings().sd_speed]);
}

// Частоты с row вверх, которые карта не приняла (High Speed нет), - строка
// без чтений. Возврат - первая принятая или kSdSpeedCount.
uint32_t skip_unsupported(uint32_t row) {
    Speed& s = s_mem->speed;
    for (; row < kSdSpeedCount; ++row) {
        if (sd::sd_card_set_speed(kSdSpeedHz[row]) >= kSdSpeedHz[row]) break;
        s.view.rows[row].state = SpeedState::NoHighSpeed;
    }
    return row;
}

// Следующая строка теста - с частоты row, счётчики заново. Строк больше
// нет - конец теста, частота из настроек обратно.
void start_next_row(uint32_t row) {
    Speed& s = s_mem->speed;
    s.row    = skip_unsupported(row);
    if (s.row >= kSdSpeedCount) {
        s.view.running = false;
        speed_restore();
        return;
    }
    s.view.rows[s.row].state = SpeedState::Run;
    s.step                   = 0;
    s.rnd                    = kSpeedSeed;
    s.hash                   = soundsinth::util::kFnvSeed;
    s.fails                  = 0;
    s.run_us                 = 0;
    s.faults0                = sd::sd_card_faults();
}

void speed_start(ToolScreen& scr) {
    s_mem->speed = Speed{};
    SpeedView& v = s_mem->speed.view;
    v.setting    = settings().sd_speed;
    v.present    = hal::media_state(Medium::Card).present && sd::sd_card_info().sector_count > kSpeedRun;
    v.running    = v.present;
    v.total      = kSdSpeedCount * kSpeedSteps;
    if (v.present) start_next_row(0);
    scr.state(v.running ? ToolState::Busy : ToolState::Ready);
    tool_render_speed(scr, v);
}

void speed_stop(ToolScreen& scr) {
    if (!s_mem->speed.view.running) return;
    s_mem->speed.view.running = false;
    s_mem->speed.view.stopped = true;
    speed_restore();
    scr.state(ToolState::Ready);
    tool_render_speed(scr, s_mem->speed.view);
    scr.publish();
}

void speed_step(ToolScreen& scr) {
    Speed& s = s_mem->speed;
    if (!s.view.running) return;
    const uint32_t sectors = sd::sd_card_info().sector_count;
    // Карту переподняли с нечитаемым CSD: читать нечего, делить не на что.
    if (sectors <= kSpeedRun) {
        speed_stop(scr);
        return;
    }
    for (uint32_t n = 0; n < kSpeedPerTurn && s.step < kSpeedSteps; ++n, ++s.step) {
        s.rnd = s.rnd * kLcgMul + kLcgInc;
        if (s.step < kSpeedRuns) {
            const uint32_t lba  = s.rnd % (sectors - kSpeedRun);
            const uint32_t t0   = platform::mono_us();
            const bool ok       = sd::sd_card_read_run(lba, kSpeedRun, s_mem->run);
            s.run_us           += platform::mono_us() - t0;
            if (ok) s.hash = soundsinth::util::fnv1a(s.hash, s_mem->run, sizeof(s_mem->run));
            s.fails += ok ? 0u : 1u;
        } else {
            const bool ok = sd::sd_card_read_sector(s.rnd % sectors, s_mem->sector);
            if (ok) s.hash = soundsinth::util::fnv1a(s.hash, s_mem->sector, sizeof(s_mem->sector));
            s.fails += ok ? 0u : 1u;
        }
    }
    s.view.done = s.row * kSpeedSteps + s.step;
    if (s.step == kSpeedSteps) {
        SpeedRow& r = s.view.rows[s.row];
        r.state     = SpeedState::Done;
        r.errors    = s.fails + (sd::sd_card_faults() - s.faults0);
        // КиБ/с: прочитанные пачками байты за секунду, делённые на 1024.
        const uint64_t kib = uint64_t{kSpeedRuns} * kSpeedRunBytes * 1000000u / 1024u;
        r.kbps             = s.run_us != 0 ? static_cast<uint32_t>(kib / s.run_us) : 0u;
        // Сверка данных - только у строк без отказов чтения: свёртка строки
        // с пропущенным чтением другая и без порчи.
        if (s.fails == 0 && !s.has_ref) {
            s.ref     = s.hash;
            s.has_ref = true;
        } else if (s.fails == 0) {
            r.differs = s.hash != s.ref;
        }
        start_next_row(s.row + 1u);
        if (!s.view.running) scr.state(ToolState::Ready);
    } else if (platform::mono_ms() - s.drawn_ms < kRedrawMs) {
        return;
    }
    s.drawn_ms = platform::mono_ms();
    tool_render_speed(scr, s.view);
    scr.publish();
}

// --- Проверка USB HID ---
//
// Что прислал каждый интерфейс и как он разобран: роль, протокол,
// расшифровка последнего отчёта по той же карте, что у устройства. По
// Enter - файл на карту: дескрипторы байтами, карта полей, последний
// отчёт. Журнала с UART у пользователя может не быть, а файл переносится
// на карте.

constexpr uint32_t kHidRedrawMs = 100;
constexpr const char* kHidFile  = "0:/USB_HID.TXT";

// Карта интерфейса - как у хоста: загрузочный протокол и отчёт короче
// дескрипторного - загрузочная раскладка мыши.
bool hid_map(const platform::UsbHidItf& d, hid::ReportMap& map) {
    const bool built = d.desc_len <= platform::kUsbHidDescKeep && hid::report_map_build(d.desc, d.desc_len, map);
    if (d.role == platform::kUsbRoleMouse && (!map.field(hid::Axis::X).present() || (d.boot && d.last_len < d.need))) {
        map = hid::kBootMouseMap;
        return true;
    }
    return built;
}

SOUNDSINTH_NOINLINE void hid_decode(const platform::UsbHidItf& d, HidRow& r) {
    r                  = HidRow{};
    r.addr             = d.addr;
    r.idx              = d.idx;
    r.role             = d.role;
    r.boot             = d.boot;
    r.vid              = d.vid;
    r.pid              = d.pid;
    r.top_page         = d.top_page;
    r.top_usage        = d.top_usage;
    r.last_len         = d.last_len;
    const uint16_t len = d.last_len < platform::kUsbHidReportKeep ? d.last_len : platform::kUsbHidReportKeep;
    if (d.role == 0 || len == 0) return;
    if (d.role == platform::kUsbRoleKeyboard) {
        // Модификаторы и первый код: у загрузочной раскладки - байты 0 и 2,
        // с номером отчёта всё сдвинуто на байт.
        hid::ReportMap map;
        const uint16_t at = hid_map(d, map) && map.uses_report_ids && !(d.boot && d.last_len < d.need) ? 1u : 0u;
        if (len < at + 3u) return;
        r.keys[0] = d.last[at];
        r.keys[1] = d.last[at + 2u];
        r.decoded = true;
        return;
    }
    hid::ReportMap map;
    if (!hid_map(d, map)) return;
    if (!hid::report_id_matches(hid::pointer_report_id(map), d.last, len)) return;
    (void)hid::report_field_read(map.field(hid::Axis::X), d.last, len, r.x);
    (void)hid::report_field_read(map.field(hid::Axis::Y), d.last, len, r.y);
    for (uint8_t i = 0; i < map.button_count; ++i) {
        if (hid::report_button_read(map.button[i], d.last, len)) r.buttons = static_cast<uint16_t>(r.buttons | (1u << i));
    }
    r.decoded = true;
}

// Экран USB HID заново. only_new - только если с прошлого кадра пришли
// отчёты или сменилось число интерфейсов; false - экран прежний.
SOUNDSINTH_NOINLINE bool hid_render(ToolScreen& scr, bool only_new) {
    platform::UsbHidItf itf[kHidRows];
    const uint32_t n = platform::usb_host_hid(itf, kHidRows);
    uint32_t seen    = 0;
    for (uint32_t i = 0; i < n; ++i) {
        seen += itf[i].reports;
    }
    HidView& v = s_mem->hid.view;
    if (only_new && seen == s_mem->hid.seen && n == v.count) return false;
    v.count         = static_cast<uint8_t>(n);
    s_mem->hid.seen = seen;
    for (uint32_t i = 0; i < n; ++i) {
        hid_decode(itf[i], v.rows[i]);
    }
    tool_render_hid(scr, v);
    return true;
}

// Строка файла журнала; f_write отказал - ok гаснет, дальше не пишется.
void hid_line(FIL& f, bool& ok, const char* fmt, ...) {
    if (!ok) return;
    va_list ap;
    va_start(ap, fmt);
    char* const line = s_mem->line;
    const int n      = std::vsnprintf(line, sizeof(s_mem->line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    const UINT len = static_cast<UINT>(static_cast<size_t>(n) < sizeof(s_mem->line) ? n : sizeof(s_mem->line) - 1u);
    UINT put       = 0;
    ok             = f_write(&f, line, len, &put) == FR_OK && put == len;
}

void hid_hex(FIL& f, bool& ok, const char* head, const uint8_t* p, uint32_t n) {
    for (uint32_t off = 0; off < n; off += 16u) {
        char hex[16 * 3 + 1] = {};
        const uint32_t k     = n - off < 16u ? n - off : 16u;
        for (uint32_t i = 0; i < k; ++i) {
            std::snprintf(hex + i * 3u, 4, "%02X ", p[off + i]);
        }
        hid_line(f, ok, "%s %03u: %s\r\n", head, static_cast<unsigned>(off), hex);
    }
}

void hid_map_lines(FIL& f, bool& ok, const hid::ReportMap& map) {
    static const char* const kAxisName[] = {"X", "Y", "Z", "Rx", "Ry", "Rz", "Slider", "Dial", "Wheel"};
    static_assert(sizeof(kAxisName) / sizeof(kAxisName[0]) == hid::kAxisCount, "a name for every axis");
    for (uint8_t i = 0; i < hid::kAxisCount; ++i) {
        const hid::Field& a = map.axis[i];
        if (!a.present()) continue;
        hid_line(f, ok, "  axis %s: report %u, bit %u, width %u, range %ld..%ld\r\n", kAxisName[i], a.report_id, a.bit_offset, a.bit_size,
                 static_cast<long>(a.logical_min), static_cast<long>(a.logical_max));
    }
    if (map.hat.present()) {
        hid_line(f, ok, "  hat: report %u, bit %u, width %u, range %ld..%ld\r\n", map.hat.report_id, map.hat.bit_offset, map.hat.bit_size,
                 static_cast<long>(map.hat.logical_min), static_cast<long>(map.hat.logical_max));
    }
    for (uint8_t i = 0; i < map.button_count; ++i) {
        hid_line(f, ok, "  button %u: report %u, bit %u\r\n", i + 1u, map.button[i].report_id, map.button[i].bit_offset);
    }
    const hid::KeyboardMap& k = map.keyboard;
    if (k.present() || k.modifier_count != 0) {
        hid_line(f, ok, "  keyboard: report %u, modifiers %u, slots %u x %u bits from bit %u\r\n", k.report_id, k.modifier_count, k.slot_count, k.slot_bit_size,
                 k.slot_bit_offset);
    }
}

SOUNDSINTH_NOINLINE bool hid_save() {
    if (!storage::volume_attach(storage::kVolumeCard, &s_mem->fs)) return false;
    FIL& f = s_mem->file;
    if (f_open(&f, kHidFile, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    bool ok = true;
    hid_line(f, ok, "PI-CARD MUSICA %s, USB HID check\r\n\r\n", SOUNDSINTH_VERSION_STR);
    platform::UsbDevice dev[kUsbShownMax];
    const uint32_t nd = platform::usb_host_devices(dev, kUsbShownMax);
    for (uint32_t i = 0; i < nd; ++i) {
        hid_line(f, ok, "device %u: %04X:%04X, speed %s, roles 0x%02X\r\n", dev[i].addr, dev[i].vid, dev[i].pid,
                 dev[i].speed == 1   ? "low"
                 : dev[i].speed == 2 ? "high"
                                     : "full",
                 dev[i].roles);
    }
    platform::UsbHidItf itf[kHidRows];
    const uint32_t n = platform::usb_host_hid(itf, kHidRows);
    for (uint32_t i = 0; i < n; ++i) {
        const platform::UsbHidItf& d = itf[i];
        const char* role             = d.role == platform::kUsbRoleMouse      ? "mouse"
                                       : d.role == platform::kUsbRoleKeyboard ? "keyboard"
                                       : d.role == platform::kUsbRoleGamepad  ? "gamepad"
                                                                              : "not taken";
        hid_line(f, ok, "\r\ninterface %u of device %u (%04X:%04X): %s, interface protocol %u, works in %s protocol\r\n", d.idx, d.addr, d.vid, d.pid, role,
                 d.itf_protocol, d.boot ? "boot" : "report");
        hid_line(f, ok, "top usage %04X:%04X, report by descriptor %u B, reports received %lu\r\n", d.top_page, d.top_usage, d.need,
                 static_cast<unsigned long>(d.reports));
        const uint16_t kept = d.desc_len < platform::kUsbHidDescKeep ? d.desc_len : platform::kUsbHidDescKeep;
        hid_line(f, ok, "descriptor %u B%s:\r\n", d.desc_len, kept < d.desc_len ? " (start only)" : "");
        hid_hex(f, ok, "  dsc", d.desc, kept);
        hid::ReportMap map;
        if (hid_map(d, map)) {
            hid_line(f, ok, "map%s:\r\n", d.boot && d.last_len < d.need ? " (boot layout, the report is short)" : "");
            hid_map_lines(f, ok, map);
        } else {
            hid_line(f, ok, "map: not built\r\n");
        }
        const uint16_t len = d.last_len < platform::kUsbHidReportKeep ? d.last_len : platform::kUsbHidReportKeep;
        hid_line(f, ok, "last report %u B:\r\n", d.last_len);
        hid_hex(f, ok, "  rep", d.last, len);
        HidRow r;
        hid_decode(d, r);
        if (r.decoded) hid_line(f, ok, "decoded: x %ld, y %ld, buttons 0x%04X\r\n", static_cast<long>(r.x), static_cast<long>(r.y), r.buttons);
    }
    const bool closed = f_close(&f) == FR_OK;
    return ok && closed;
}

// --- Сведения ---

void card_info(ToolScreen& scr) {
    const sd::SdInfo& sd = sd::sd_card_info();
    CardFacts f{};
    f.present   = hal::media_state(Medium::Card).present;
    f.sdhc      = sd.kind == sd::SdKind::Sdhc;
    f.sectors   = sd.sector_count;
    f.cid_valid = sd.cid_valid;
    std::memcpy(f.cid, sd.cid, sizeof(f.cid));
    VolumeFacts v{};
    Medium m = Medium::Card;
    storage::FatVolume fv;
    if (f.present && storage::fat_volume_open(io_of(m), s_mem->sector, fv)) {
        v.valid         = true;
        v.fat_bits      = fv.fat_bits;
        v.cluster_bytes = fv.cluster_bytes;
        v.clusters      = fv.clusters;
        v.fsinfo_free   = fv.fsinfo_free;
        std::memcpy(v.label, fv.label, sizeof(v.label));
    }
    tool_render_card(scr, f, v);
}

void board_info(ToolScreen& scr) {
    const platform::BoardInfo b = platform::board_info();
    BoardFacts f{};
    f.version         = SOUNDSINTH_VERSION_STR;
    f.chip_revision   = b.chip_revision;
    f.sys_hz          = b.sys_hz;
    f.flash_bytes     = b.flash_bytes;
    f.psram_bytes     = b.psram_bytes;
    f.uptime_s        = platform::mono_ms() / 1000u;
    const Settings& s = settings();
    for (const auto& name : s.rom_file) {
        if (name[0] != 0) ++f.rom_slots_used;
    }
    tool_render_board(scr, f);
}

void usb_info(ToolScreen& scr) {
    platform::UsbDevice dev[kUsbShownMax];
    const uint32_t n = platform::usb_host_devices(dev, kUsbShownMax);
    tool_render_usb(scr, dev, n);
}

void render_open(ToolScreen& scr) {
    switch (s_open) {
        case ToolId::CardInfo:
            card_info(scr);
            break;
        case ToolId::BoardInfo:
            board_info(scr);
            break;
        case ToolId::UsbInfo:
            usb_info(scr);
            break;
        case ToolId::CardCheck:
            check_start(scr, Medium::Card);
            break;
        case ToolId::UsbCheck:
            check_start(scr, Medium::Usb);
            break;
        case ToolId::CardSpeed:
            speed_start(scr);
            break;
        case ToolId::UsbHid:
            s_mem->hid.view.save = HidSave::None;
            hid_render(scr, false);
            break;
        default:
            scr.clear();
            break;
    }
}

} // namespace

void tools_serve(uint8_t* page, soundsinth::memory::TrackScratch& scratch) {
    ToolScreen scr(page);
    const ToolId tool = scr.tool();
    if (tool == ToolId::None || tool_on_wifi_core(tool)) {
        s_open = ToolId::None;
        return;
    }
    // Без памяти инструментов экранов нет: состояние живёт в ней.
    if (!tool_mem_take(scratch)) {
        uint8_t key = 0;
        while (scr.key(key, tool)) {
            if (key != kToolKeyOpen) continue;
            scr.clear();
            scr.put(0, "NO MEMORY FOR THE TOOLS", kToolAlert);
            scr.state(ToolState::Ready);
            scr.publish();
        }
        s_open = ToolId::None;
        return;
    }
    uint8_t k = 0;
    while (scr.key(k, tool)) {
        if (k == kToolKeyOpen) {
            s_open = tool;
            scr.state(ToolState::Ready);
            render_open(scr);
            s_drawn = platform::mono_ms();
            scr.publish();
            continue;
        }
        if (k == kToolKeyClose) {
            speed_stop(scr);
            s_open = ToolId::None;
            continue;
        }
        if (s_open == ToolId::None) continue;
        switch (k) {
            case kToolKeyEsc:
                speed_stop(scr); // тест идёт: ESC его останавливает, экран остаётся
                break;
            case kToolKeyDel:
                // Забой - назад, как ESC: ввода на этих экранах нет.
                speed_stop(scr);
                s_open = ToolId::None;
                scr.state(ToolState::Closed);
                scr.publish();
                break;
            case kToolKeyEnter:
                if (is_check(tool)) check_fix(scr);
                if (tool == ToolId::UsbHid) {
                    s_mem->hid.view.save = hid_save() ? HidSave::Saved : HidSave::Failed;
                    hid_render(scr, false);
                    scr.publish();
                }
                break;
            default:
                break;
        }
    }
    if (s_open == ToolId::None) return;
    if (is_check(tool)) {
        check_step(scr);
        return;
    }
    if (tool == ToolId::CardSpeed) {
        speed_step(scr);
        return;
    }
    if (tool == ToolId::UsbHid) {
        // Отчёты идут сотнями в секунду: перерисовка не чаще kHidRedrawMs и
        // только когда пришло новое.
        const uint32_t now = platform::mono_ms();
        if (now - s_mem->hid.drawn_ms < kHidRedrawMs) return;
        s_mem->hid.drawn_ms = now;
        if (hid_render(scr, true)) scr.publish();
        return;
    }
    // Время работы и список USB меняются сами: раз в секунду заново.
    const uint32_t now = platform::mono_ms();
    if ((tool == ToolId::UsbInfo || tool == ToolId::BoardInfo) && now - s_drawn >= kRefreshMs) {
        s_drawn = now;
        render_open(scr);
        scr.publish();
    }
}

} // namespace player::config
