// SPDX-License-Identifier: MIT
#include "player/config/config_service.h"

#include <cstring>
#include <new>

#include "ff.h"

#include "platform/boot_mode.h"
#include "platform/compiler.h"
#include "platform/config_flash.h"
#include "platform/log.h"
#include "platform/mono_time.h"

#include "core/config/config_ini.h"
#include "core/config/config_store.h"

namespace player::config {
namespace {

using soundsinth::config::Settings;

constexpr const char* kSetPath = "set_config.txt";
constexpr const char* kSetDone = "set_config.done";
constexpr const char* kGetPath = "get_config.txt";
constexpr const char* kGetDone = "get_config.done";

// Приглашение в конфигуратор. Лежит в корне, содержимое не смотрится -
// важно само наличие. Снимается при осознанном выходе из конфигуратора, а
// не при входе: машина, которая до меню не дошла, обязана попасть в него и
// со второй попытки.
constexpr const char* kGuiPath = "open.gui";

// Короче этого файл считается недоделанным: пустой или обрезанный
// set_config.txt не должен сходить за настройки.
constexpr uint32_t kMinSetBytes = 10;

constexpr uint32_t kPageBytes = soundsinth::config::kPageBytes;
constexpr uint32_t kTextBytes = soundsinth::config::kIniMaxBytes;
static_assert(kPageBytes + kTextBytes + sizeof(FATFS) <= soundsinth::memory::kConfigBytes, "the scratch holds the block, the text and the volume state");

Settings s_settings;

// Переименовать, снеся прежний результат: второй запуск с той же картой
// иначе упёрся бы в существующее имя.
bool rename_over(const char* from, const char* to) {
    f_unlink(to);
    return f_rename(from, to) == FR_OK;
}

// Прочитать файл целиком. Возвращает длину, ноль - не годится.
uint32_t read_text(const char* path, char* text, uint32_t cap) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    const FSIZE_t size = f_size(&f);
    if (size < kMinSetBytes || size >= cap) {
        f_close(&f);
        return 0;
    }
    UINT got        = 0;
    const FRESULT r = f_read(&f, text, static_cast<UINT>(size), &got);
    f_close(&f);
    if (r != FR_OK || got != size) return 0;
    return got;
}

bool write_text(const char* path, const char* text, uint32_t len) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT put        = 0;
    const FRESULT r = f_write(&f, text, len, &put);
    // Закрытие обновляет каталог: без проверки файл оставался бы нулевой
    // длины, а мы считали бы его записанным.
    const bool closed = f_close(&f) == FR_OK;
    return r == FR_OK && put == len && closed;
}

// Свои ПЗУ: файл из корня карты против области флеша, гранула за гранулой;
// переписываются только разошедшиеся. Чужого размера файл не пишется:
// половина образа в области хуже прежнего целого.
void rom_slots_sync(const soundsinth::config::Settings& s, uint8_t* chunk) {
    for (uint32_t slot = 0; slot < soundsinth::config::kRomSlots; ++slot) {
        const char* name = s.rom_file[slot];
        if (name[0] == '\0') continue;
        const uint8_t* flash = platform::rom_slot_view(slot);
        if (flash == nullptr) return;
        FIL f;
        if (f_open(&f, name, FA_READ) != FR_OK) {
            platform::debug_logf("boot: ROM slot %u: %s not on the card\n", static_cast<unsigned>(slot + 1u), name);
            continue;
        }
        if (f_size(&f) != platform::kRomSlotBytes) {
            platform::debug_logf("boot: ROM slot %u: %s is %u B, not 65536\n", static_cast<unsigned>(slot + 1u), name, static_cast<unsigned>(f_size(&f)));
            f_close(&f);
            continue;
        }
        uint32_t written = 0;
        bool ok          = true;
        for (uint32_t sector = 0; sector < platform::kRomSlotBytes / platform::kRomSectorBytes && ok; ++sector) {
            UINT got = 0;
            ok       = f_read(&f, chunk, platform::kRomSectorBytes, &got) == FR_OK && got == platform::kRomSectorBytes;
            if (!ok || std::memcmp(chunk, flash + sector * platform::kRomSectorBytes, platform::kRomSectorBytes) == 0) continue;
            ok = platform::rom_slot_write(slot, sector, chunk);
            ++written;
        }
        f_close(&f);
        platform::debug_logf("boot: ROM slot %u: %s, sectors rewritten %u%s\n", static_cast<unsigned>(slot + 1u), name, static_cast<unsigned>(written),
                             ok ? "" : ", FAILED");
    }
}

// Засечка этапа загрузки, мс.
struct StopwatchMs {
    uint32_t t0 = platform::mono_us();
    uint32_t ms() const { return (platform::mono_us() - t0) / 1000u; }
};

// Шаги config_boot встроены: стек Core0 до планировщика 4 КБ, лишние
// кадры над f_* нежелательны.

// Приглашение конфигуратора и разбор set_config.txt. true - настройки
// изменились.
SOUNDSINTH_ALWAYS_INLINE bool apply_set_file(char* text, BootReport& rep, bool drop_invite) {
    // Приглашение снимается по запросу конфигуратора, до чтения
    // set_config.txt: порядок не важен, важно что в этом же окне.
    if (drop_invite) f_unlink(kGuiPath);
    FILINFO gui;
    rep.gui_invite = f_stat(kGuiPath, &gui) == FR_OK;

    const StopwatchMs t;
    const uint32_t len = read_text(kSetPath, text, kTextBytes);
    rep.set_ms         = t.ms();
    if (len == 0) return false;
    const soundsinth::config::IniResult r = soundsinth::config::ini_parse(text, len, s_settings);
    rep.applied                           = r.applied;
    rep.unknown                           = r.unknown;
    rep.bad                               = r.bad;
    rep.file_applied                      = true;
    rep.file_kept                         = !rename_over(kSetPath, kSetDone);
    return r.applied != 0;
}

SOUNDSINTH_ALWAYS_INLINE void save_to_flash(const uint8_t* page0, const uint8_t* page1, uint8_t* page, BootReport& rep) {
    const StopwatchMs t;
    const soundsinth::config::NextWrite next = soundsinth::config::store_next(page0, page1);
    soundsinth::config::page_build(page, s_settings, next.counter);
    rep.slot          = static_cast<uint8_t>(next.slot);
    rep.flash_written = platform::config_flash_write(static_cast<uint32_t>(next.slot), page, kPageBytes);
    rep.flash_ms      = t.ms();
}

// get_config.txt заполняется, только если пользователь его положил.
SOUNDSINTH_ALWAYS_INLINE void write_get_file(char* text, BootReport& rep) {
    const StopwatchMs t_write;
    FILINFO info;
    if (f_stat(kGetPath, &info) != FR_OK) return;
    const uint32_t len = soundsinth::config::ini_render(s_settings, text, kTextBytes);
    const bool written = len != 0 && len < kTextBytes && write_text(kGetPath, text, len);
    rep.save_write_ms  = t_write.ms();
    if (!written) return;
    const StopwatchMs t_rename;
    rep.file_saved     = rename_over(kGetPath, kGetDone);
    rep.save_rename_ms = t_rename.ms();
}

} // namespace

const Settings& settings() {
    return s_settings;
}

void settings_set(const Settings& s) {
    s_settings = s;
}

BootReport config_boot(soundsinth::memory::TrackScratch& scratch) {
    BootReport rep;

    const uint8_t* const page0 = platform::config_flash_view(0);
    const uint8_t* const page1 = platform::config_flash_view(1);
    const int cur              = soundsinth::config::store_pick(page0, page1);
    rep.flash_valid            = cur >= 0;
    if (cur >= 0) soundsinth::config::page_read(cur == 0 ? page0 : page1, s_settings);

    // Запрос прошлой загрузки. Флеш конфигуратор записал сам, перед
    // перезагрузкой; карту правит эта загрузка.
    platform::BootRequest req;
    const bool have_request = platform::boot_request_take(req);
    rep.from_configurator   = have_request && req.saved;
    rep.configurator        = have_request && req.configurator;

    // Блок для записи и текст файла - на вершине резидентного пула: своей
    // памяти им не надо, живут они одну загрузку.
    uint8_t* const buf = soundsinth::memory::scratch_take(scratch, soundsinth::memory::Scratch::Config, soundsinth::memory::kConfigBytes);
    if (buf == nullptr) return rep; // места нет: остаётся то, что пришло из флеша
    uint8_t* const page = buf;
    char* const text    = reinterpret_cast<char*>(buf + kPageBytes);
    // Состояние тома тоже не на стеке: у Core0 до планировщика 4 КБ, а
    // FATFS занимает больше полукилобайта.
    FATFS& fs = *new (buf + kPageBytes + kTextBytes) FATFS();

    // Карта своя: банк монтирует том позже и для себя.
    const StopwatchMs t_mount;
    const bool mounted = f_mount(&fs, "", 1) == FR_OK;
    rep.mount_ms       = t_mount.ms();

    const bool changed = mounted && apply_set_file(text, rep, have_request && req.drop_open_gui);

    // Блока не было вовсе или файл что-то поменял - сохраняем.
    if (!rep.flash_valid || changed) save_to_flash(page0, page1, page, rep);

    if (mounted) {
        // Блок уже записан: буфер блока свободен под гранулу образа.
        rom_slots_sync(s_settings, page);
        write_get_file(text, rep);
    }
    // Снять регистрацию тома и при отказе монтирования: f_mount регистрирует
    // FATFS до попытки, а память под ним сейчас уйдёт арене.
    const StopwatchMs t_umount;
    f_mount(nullptr, "", 0);
    if (mounted) rep.unmount_ms = t_umount.ms();

    soundsinth::memory::scratch_leave(scratch, soundsinth::memory::Scratch::Config);
    return rep;
}

namespace {

// Окончание .rom без учёта регистра.
bool rom_name(const char* name) {
    const size_t n = std::strlen(name);
    if (n < 5 || n > soundsinth::config::kRomFileMax) return false;
    const char* e = name + n - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'r' && (e[2] | 0x20) == 'o' && (e[3] | 0x20) == 'm';
}

} // namespace

uint32_t rom_files_scan(soundsinth::memory::TrackScratch& scratch, soundsinth::config::ConfigPageFiles& out) {
    out.count          = 0;
    uint8_t* const buf = soundsinth::memory::scratch_take(scratch, soundsinth::memory::Scratch::Config, soundsinth::memory::kConfigBytes);
    if (buf == nullptr) return 0;
    FATFS& fs = *new (buf) FATFS();
    if (f_mount(&fs, "", 1) == FR_OK) {
        DIR dir;
        FILINFO info;
        if (f_opendir(&dir, "/") == FR_OK) {
            while (out.count < soundsinth::config::kRomFilesMax && f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
                if ((info.fattrib & (AM_DIR | AM_HID | AM_SYS)) != 0) continue;
                if (info.fsize != platform::kRomSlotBytes || !rom_name(info.fname)) continue;
                std::memcpy(out.name[out.count], info.fname, std::strlen(info.fname) + 1u);
                ++out.count;
            }
            f_closedir(&dir);
        }
    }
    f_mount(nullptr, "", 0);
    soundsinth::memory::scratch_leave(scratch, soundsinth::memory::Scratch::Config);
    return out.count;
}

} // namespace player::config
