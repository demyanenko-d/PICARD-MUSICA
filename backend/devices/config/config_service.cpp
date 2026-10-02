// SPDX-License-Identifier: MIT
#include "devices/config/config_service.h"

#include <new>

#include "ff.h"

#include "core/config/config_ini.h"
#include "core/config/config_store.h"
#include "platform/boot_mode.h"
#include "platform/config_flash.h"
#include "platform/mono_time.h"

namespace devices::config {
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

    // Запрос прошлой загрузки: конфигуратор не пишет ни флеш, ни карту -
    // то и другое отнимает шину у живой машины. Он складывает сюда, а
    // делаем мы, в окне, где это уже безопасно.
    platform::BootRequest req;
    const bool have_request = platform::boot_request_take(req);
    bool changed            = false;
    if (have_request && req.save) {
        // Приведение к допустимым - и здесь тоже: запрос едет через
        // регистры, а записать во флеш непроверенное значит потерять
        // настройки у того, кто конфигуратор и не открывал.
        soundsinth::config::settings_clamp(req.settings);
        s_settings            = req.settings;
        rep.from_configurator = true;
        changed               = true;
    }
    rep.configurator = have_request && req.configurator;

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
    const uint32_t t_mount = platform::mono_us();
    const bool mounted     = f_mount(&fs, "", 1) == FR_OK;
    rep.mount_ms           = (platform::mono_us() - t_mount) / 1000u;

    if (mounted) {
        // Приглашение снимается по запросу конфигуратора, до чтения
        // set_config.txt: порядок не важен, важно что в этом же окне.
        if (have_request && req.drop_open_gui) f_unlink(kGuiPath);
        FILINFO gui;
        rep.gui_invite = f_stat(kGuiPath, &gui) == FR_OK;

        const uint32_t t_set = platform::mono_us();
        const uint32_t len   = read_text(kSetPath, text, kTextBytes);
        rep.set_ms           = (platform::mono_us() - t_set) / 1000u;
        if (len != 0) {
            const soundsinth::config::IniResult r = soundsinth::config::ini_parse(text, len, s_settings);
            rep.applied                           = r.applied;
            rep.unknown                           = r.unknown;
            rep.bad                               = r.bad;
            rep.file_applied                      = true;
            changed                               = changed || r.applied != 0;
            rename_over(kSetPath, kSetDone);
        }
    }

    // Блока не было вовсе или файл что-то поменял - сохраняем.
    if (!rep.flash_valid || changed) {
        const uint32_t t_flash                   = platform::mono_us();
        const soundsinth::config::NextWrite next = soundsinth::config::store_next(page0, page1);
        soundsinth::config::page_build(page, s_settings, next.counter);
        rep.slot          = static_cast<uint8_t>(next.slot);
        rep.flash_written = platform::config_flash_write(static_cast<uint32_t>(next.slot), page, kPageBytes);
        rep.flash_ms      = (platform::mono_us() - t_flash) / 1000u;
    }

    if (mounted) {
        FILINFO info;
        const uint32_t t0 = platform::mono_us();
        if (f_stat(kGetPath, &info) == FR_OK) {
            const uint32_t len = soundsinth::config::ini_render(s_settings, text, kTextBytes);
            const bool written = len != 0 && len < kTextBytes && write_text(kGetPath, text, len);
            rep.save_write_ms  = (platform::mono_us() - t0) / 1000u;
            if (written) {
                const uint32_t t1  = platform::mono_us();
                rep.file_saved     = rename_over(kGetPath, kGetDone);
                rep.save_rename_ms = (platform::mono_us() - t1) / 1000u;
            }
        }
        const uint32_t t_umount = platform::mono_us();
        f_mount(nullptr, "", 0);
        rep.unmount_ms = (platform::mono_us() - t_umount) / 1000u;
    }

    soundsinth::memory::scratch_leave(scratch, soundsinth::memory::Scratch::Config);
    return rep;
}

} // namespace devices::config
