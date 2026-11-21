// SPDX-License-Identifier: MIT
#pragma once

// Таблица ключей настроек: одна на всех потребителей.
//
// Её читают разбор и запись файла (`config_ini`) и построение страницы для
// ПЗУ-конфигуратора (`config_page`). Разойдись они - плата писала бы файл
// одним набором ключей, а показывала на экране другой.
//
// Порядок записей - это порядок строк в файле и порядок полей в странице,
// поэтому поля только дописываются в конец.

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "core/config/config.h"

namespace soundsinth::config {

enum class Kind : uint8_t { Flag, Number, Media, Layout, Mod, DiskSys, LiveMidi, Key, Rom, SdSpeed, AyStereo, AyEmulation, Beeper };
inline constexpr uint32_t kKindCount = 13;

// Именованные значения: одна таблица на разбор и на запись, поэтому
// написанное платой она прочитает обратно.
struct Named {
    const char* name;
    uint8_t value;
};

inline constexpr Named kMediaNames[] = {
    {"NONE", static_cast<uint8_t>(Media::None)},
    {"SD", static_cast<uint8_t>(Media::Sd)},
    {"USB", static_cast<uint8_t>(Media::Usb)},
};

inline constexpr Named kLayoutNames[] = {
    {"NONE", static_cast<uint8_t>(config::Layout::None)},
    {"PICARD", static_cast<uint8_t>(config::Layout::Picard)},
};

inline constexpr Named kDiskSysNames[] = {
    {"NONE", static_cast<uint8_t>(config::DiskSys::None)},
    {"DIVMMC", static_cast<uint8_t>(config::DiskSys::DivMmc)},
    {"TRDOS", static_cast<uint8_t>(config::DiskSys::TrDos)},
};

// Имена в файле. В конфигураторе вместо SLOT1..3 - имена зашитых файлов.
inline constexpr Named kRomNames[] = {
    {"BOARD", static_cast<uint8_t>(config::RomSource::Board)},
    {"SLOT1", static_cast<uint8_t>(config::RomSource::Slot1)},
    {"SLOT2", static_cast<uint8_t>(config::RomSource::Slot2)},
    {"SLOT3", static_cast<uint8_t>(config::RomSource::Slot3)},
};

// Частота карты в МГц, номера - в kSdSpeedHz.
inline constexpr Named kSdSpeedNames[] = {
    {"12.5", 0}, {"15", 1}, {"18.75", 2}, {"25", 3}, {"37.5", 4},
};
static_assert(std::size(kSdSpeedNames) == kSdSpeedCount, "a name per card speed");

inline constexpr Named kAyStereoNames[] = {
    {"ACB", static_cast<uint8_t>(config::AyStereo::Acb)},
    {"ABC", static_cast<uint8_t>(config::AyStereo::Abc)},
    {"MONO", static_cast<uint8_t>(config::AyStereo::Mono)},
};

// Пробелы в имени при разборе файла не значат: значение читается без них.
inline constexpr Named kAyEmulationNames[] = {
    {"NONE", static_cast<uint8_t>(config::AyEmulation::None)},
    {"AY8910", static_cast<uint8_t>(config::AyEmulation::Ay8910)},
    {"YM2149", static_cast<uint8_t>(config::AyEmulation::Ym2149)},
    {"TS 2xYM2149", static_cast<uint8_t>(config::AyEmulation::TsYm2149)},
    {"TSFM 2xYM2203", static_cast<uint8_t>(config::AyEmulation::TsfmYm2203)},
};

inline constexpr Named kBeeperNames[] = {
    {"NONE", static_cast<uint8_t>(config::Beeper::None)},
    {"1BIT", static_cast<uint8_t>(config::Beeper::OneBit)},
    {"2BIT", static_cast<uint8_t>(config::Beeper::TwoBit)},
};

inline constexpr Named kLiveMidiNames[] = {
    {"NONE", static_cast<uint8_t>(config::LiveMidi::None)},
    {"HOOK", static_cast<uint8_t>(config::LiveMidi::Hook)},
    {"WIRE", static_cast<uint8_t>(config::LiveMidi::Wire)},
};

inline constexpr Named kModNames[] = {
    {"NONE", static_cast<uint8_t>(ModRole::None)},
    {"CAPS", static_cast<uint8_t>(ModRole::CapsShift)},
    {"SYMBOL", static_cast<uint8_t>(ModRole::SymbolShift)},
};

// Клавиши, которые привязываются к действиям джойстика: коды HID от
// kKeyCodeMin до kKeyCodeMax без Esc. NONE - клавиши нет.
inline constexpr Named kKeyNames[] = {
    {"NONE", 0x00},     {"A", 0x04},           {"B", 0x05},          {"C", 0x06},         {"D", 0x07},       {"E", 0x08},        {"F", 0x09},
    {"G", 0x0a},        {"H", 0x0b},           {"I", 0x0c},          {"J", 0x0d},         {"K", 0x0e},       {"L", 0x0f},        {"M", 0x10},
    {"N", 0x11},        {"O", 0x12},           {"P", 0x13},          {"Q", 0x14},         {"R", 0x15},       {"S", 0x16},        {"T", 0x17},
    {"U", 0x18},        {"V", 0x19},           {"W", 0x1a},          {"X", 0x1b},         {"Y", 0x1c},       {"Z", 0x1d},        {"1", 0x1e},
    {"2", 0x1f},        {"3", 0x20},           {"4", 0x21},          {"5", 0x22},         {"6", 0x23},       {"7", 0x24},        {"8", 0x25},
    {"9", 0x26},        {"0", 0x27},           {"ENTER", 0x28},      {"TAB", 0x2b},       {"SPACE", 0x2c},   {"MINUS", 0x2d},    {"EQUAL", 0x2e},
    {"LBRACKET", 0x2f}, {"RBRACKET", 0x30},    {"BACKSLASH", 0x31},  {"SEMICOLON", 0x33}, {"QUOTE", 0x34},   {"GRAVE", 0x35},    {"COMMA", 0x36},
    {"PERIOD", 0x37},   {"SLASH", 0x38},       {"CAPSLOCK", 0x39},   {"F1", 0x3a},        {"F2", 0x3b},      {"F3", 0x3c},       {"F4", 0x3d},
    {"F5", 0x3e},       {"F6", 0x3f},          {"F7", 0x40},         {"F8", 0x41},        {"F9", 0x42},      {"F10", 0x43},      {"F11", 0x44},
    {"F12", 0x45},      {"PRINTSCREEN", 0x46}, {"SCROLLLOCK", 0x47}, {"PAUSE", 0x48},     {"INSERT", 0x49},  {"HOME", 0x4a},     {"PAGEUP", 0x4b},
    {"DELETE", 0x4c},   {"END", 0x4d},         {"PAGEDOWN", 0x4e},   {"RIGHT", 0x4f},     {"LEFT", 0x50},    {"DOWN", 0x51},     {"UP", 0x52},
    {"NUMLOCK", 0x53},  {"KP_SLASH", 0x54},    {"KP_STAR", 0x55},    {"KP_MINUS", 0x56},  {"KP_PLUS", 0x57}, {"KP_ENTER", 0x58}, {"KP_1", 0x59},
    {"KP_2", 0x5a},     {"KP_3", 0x5b},        {"KP_4", 0x5c},       {"KP_5", 0x5d},      {"KP_6", 0x5e},    {"KP_7", 0x5f},     {"KP_8", 0x60},
    {"KP_9", 0x61},     {"KP_0", 0x62},        {"KP_DOT", 0x63},
};

// Строки привязок в файле: по ключу на действие, в порядке разрядов порта.
inline constexpr const char* kJoyActionNames[kJoyActions] = {
    "JOY_RIGHT", "JOY_LEFT", "JOY_DOWN", "JOY_UP", "JOY_FIRE", "JOY_FIRE2", "JOY_FIRE3", "JOY_FIRE4",
};

struct NameTable {
    const Named* items;
    uint32_t count;
};

inline NameTable names_of(Kind k) {
    switch (k) {
        case Kind::Media:
            return {kMediaNames, std::size(kMediaNames)};
        case Kind::Layout:
            return {kLayoutNames, std::size(kLayoutNames)};
        case Kind::Mod:
            return {kModNames, std::size(kModNames)};
        case Kind::DiskSys:
            return {kDiskSysNames, std::size(kDiskSysNames)};
        case Kind::LiveMidi:
            return {kLiveMidiNames, std::size(kLiveMidiNames)};
        case Kind::Key:
            return {kKeyNames, std::size(kKeyNames)};
        case Kind::Rom:
            return {kRomNames, std::size(kRomNames)};
        case Kind::SdSpeed:
            return {kSdSpeedNames, std::size(kSdSpeedNames)};
        case Kind::AyStereo:
            return {kAyStereoNames, std::size(kAyStereoNames)};
        case Kind::AyEmulation:
            return {kAyEmulationNames, std::size(kAyEmulationNames)};
        case Kind::Beeper:
            return {kBeeperNames, std::size(kBeeperNames)};
        default:
            return {nullptr, 0};
    }
}

struct KeyDesc {
    const char* name;
    uint8_t Settings::*field;
    Kind kind;
    uint8_t min;
    uint8_t max;
    const char* comment;
    // Заголовок раздела перед ключом; nullptr - ключ идёт следом за
    // предыдущим.
    const char* section;
};

// Порядок - он же порядок строк в записанном файле.
inline constexpr KeyDesc kKeys[] = {
    {"DISKSYS", &Settings::disksys, Kind::DiskSys, 0, 0,
     "disk system: DIVMMC emulates the ROM and the card, the machine boots from the board card; "
     "TRDOS is the Beta Disk on the board: four drives with TRD and SCL images from the card or the USB flash drive and the TR-DOS "
     "trigger on the DOS_N signal; allowed: NONE, DIVMMC or TRDOS",
     "Memory and disks"},
    {"DIVMMC_CARD1", &Settings::divmmc_card1, Kind::Media, 0, 0,
     "DivMMC card 1 on ports E7h/EBh and where it takes its sectors from, with any DISKSYS; NONE turns it off; with DISKSYS=DIVMMC "
     "one of the cards is always on, card 1 on SD if none is chosen; allowed: NONE, SD or USB",
     nullptr},
    {"DIVMMC_CARD2", &Settings::divmmc_card2, Kind::Media, 0, 0, "DivMMC card 2 on the same ports, selected by bit 1 of E7h; allowed: NONE, SD or USB",
     nullptr},
    {"ZCONTROLLER", &Settings::zcontroller, Kind::Media, 0, 0,
     "Z-Controller on ports 57h/77h and where it takes its sectors from; NONE if your own hardware answers there; allowed: NONE, SD or USB", nullptr},
    {"SD_SPEED", &Settings::sd_speed, Kind::SdSpeed, 0, 0,
     "SD card clock in MHz; above 25 the card must take High Speed, otherwise it runs at 25; check the card with TOOLS - CARD SPEED TEST; "
     "allowed: 12.5, 15, 18.75, 25 or 37.5",
     nullptr},

    {"GS", &Settings::gs, Kind::Flag, 0, 1, "General Sound emulation on ports BBh/B3h; allowed: 0/1", "Audio"},
    {"GS_FX_SEPARATE", &Settings::gs_fx_separate, Kind::Flag, 0, 1,
     "General Sound effects play on their own voices instead of taking a module channel; 0 is what the real card does and what games expect; "
     "allowed: 0/1",
     nullptr},
    {"LIVE_MIDI", &Settings::live_midi, Kind::LiveMidi, 0, 0,
     "live MIDI plays through the instrument bank: HOOK is the stream from the AY port, "
     "WIRE is a wire on the GPIO33 input; allowed: NONE, HOOK or WIRE",
     nullptr},
    {"AY_EMULATION", &Settings::ay_emulation, Kind::AyEmulation, 0, 0,
     "the board plays the AY music itself, TS is TurboSound, TSFM is TurboSound FM; "
     "allowed: NONE, AY8910, YM2149, TS 2xYM2149 or TSFM 2xYM2203",
     nullptr},
    {"AY_STEREO", &Settings::ay_stereo, Kind::AyStereo, 0, 0, "AY channels from left to right; allowed: ACB, ABC or MONO", nullptr},
    {"SAA1099", &Settings::saa1099, Kind::Flag, 0, 1, "the board plays the SAA1099 of MultiSound on ports #FF and #1FF; allowed: 0/1", nullptr},
    {"COVOX", &Settings::covox, Kind::Flag, 0, 1, "the board plays the Covox on port #FB, to both channels; allowed: 0/1", nullptr},
    {"SOUNDRIVE", &Settings::soundrive, Kind::Flag, 0, 1,
     "the board plays the Soundrive 1.05: ports #0F and #1F to the left channel, #4F and #5F to the right; allowed: 0/1", nullptr},
    {"BEEPER", &Settings::beeper, Kind::Beeper, 0, 0,
     "the board plays the beeper of port #FE: 1BIT is bit 4 only, 2BIT adds bit 3 (tape out) ten times quieter, as the 48K ULA output; allowed: NONE, 1BIT or "
     "2BIT",
     nullptr},
    {"STOP_MUSIC_ON_RESET", &Settings::stop_music_on_reset, Kind::Flag, 0, 1, "a machine reset stops the board music: GS, modules, MIDI; allowed: 0/1",
     nullptr},
    {"AUTO_VOLUME", &Settings::auto_volume, Kind::Flag, 0, 1,
     "automatic volume control: tracks from files are levelled to one loudness, within -12..+18 dB; allowed: 0/1", nullptr},
    {"MOD_PANNING", &Settings::mod_panning, Kind::Number, 0, kModPanningMax,
     "how wide MOD channels are spread, per cent of the Amiga hardware layout: 100 is hard left/right as on the real thing, 0 is all centred; allowed: "
     "0..100",
     nullptr},
    {"MOD_INTERPOLATION", &Settings::mod_interpolation, Kind::Flag, 0, 1,
     "MOD samples between their points: 1 is smooth, as PC trackers play; 0 holds each point, the staircase of the Amiga Paula chip, "
     "brighter; other formats are always smooth; allowed: 0/1",
     nullptr},

    {"RESET_SIGNAL", &Settings::reset_signal, Kind::Flag, 0, 1, "the board may reset the machine; allowed: 0/1", "Host machine"},

    {"KEYBOARD_LAYOUT", &Settings::keyboard_layout, Kind::Layout, 0, 0, "USB keyboard layout; allowed: PICARD or NONE, which turns the keyboard off",
     "USB keyboard"},
    {"MOD_RIGHT_SHIFT", &Settings::mod_right_shift, Kind::Mod, 0, 0, "what the right Shift becomes; allowed: CAPS, SYMBOL or NONE", nullptr},
    {"MOD_LEFT_CTRL", &Settings::mod_left_ctrl, Kind::Mod, 0, 0, "what the left Ctrl becomes; allowed: CAPS, SYMBOL or NONE", nullptr},
    {"MOD_RIGHT_CTRL", &Settings::mod_right_ctrl, Kind::Mod, 0, 0, "what the right Ctrl becomes; allowed: CAPS, SYMBOL or NONE", nullptr},
    {"MOD_LEFT_ALT", &Settings::mod_left_alt, Kind::Mod, 0, 0, "what the left Alt becomes; allowed: CAPS, SYMBOL or NONE", nullptr},
    {"MOD_RIGHT_ALT", &Settings::mod_right_alt, Kind::Mod, 0, 0, "what the right Alt becomes; allowed: CAPS, SYMBOL or NONE", nullptr},

    {"USB_MOUSE", &Settings::usb_mouse, Kind::Flag, 0, 1, "USB mouse as Kempston; allowed: 0/1", "USB mouse"},
    {"MOUSE_SPEED", &Settings::mouse_speed, Kind::Number, kMouseSpeedMin, kMouseSpeedMax,
     "mouse speed in quarters: 4 is one to one, 8 is twice as fast; allowed: 1..32", nullptr},
    {"MOUSE_SWAP_BUTTONS", &Settings::mouse_swap_buttons, Kind::Flag, 0, 1, "swap the left and right buttons; allowed: 0/1", nullptr},
    {"MOUSE_AUTOSWAP", &Settings::mouse_autoswap, Kind::Flag, 0, 1,
     "holding both buttons together for half a second swaps left and right until the board is powered off; allowed: 0/1", nullptr},

    {"USB_GAMEPAD", &Settings::usb_gamepad, Kind::Flag, 0, 1, "USB gamepad as a Kempston joystick; allowed: 0/1", "USB joystick"},
    {"JOY_8BIT", &Settings::joy_8bit, Kind::Flag, 0, 1,
     "Kempston with 8 bits: bits 5..7 are three more buttons; with 0 they always read as zero, as on the plain interface; allowed: 0/1", nullptr},

    {"LOG", &Settings::log, Kind::Flag, 0, 1, "board log on GPIO32, 115200 baud; allowed: 0/1", "Board"},
    {"WIFI", &Settings::wifi, Kind::Flag, 0, 1, "Wi-Fi: takes GPIO34, 38 and 39, so it does not work together with LIVE_MIDI=WIRE; allowed: 0/1", nullptr},

    {"ROM", &Settings::rom, Kind::Rom, 0, 0,
     "machine ROM: BOARD is the ROM of the machine itself, SLOT1..3 is a 64 KB Pentagon ROM image from that board flash slot; "
     "works with any DISKSYS but DIVMMC; allowed: BOARD, SLOT1, SLOT2 or SLOT3",
     "Machine ROM"},
};

inline constexpr uint32_t kKeyCount = std::size(kKeys);

// Поля привязок в странице конфигуратора идут за ключами таблицы: по
// клавише действия на поле, действие за действием.
inline constexpr uint32_t kJoyKeyFields                       = kJoyActions * kJoyKeysPerAction;
inline constexpr const char* kJoyKeyFieldNames[kJoyKeyFields] = {
    "JOY_RIGHT 1", "JOY_RIGHT 2", "JOY_LEFT 1",  "JOY_LEFT 2",  "JOY_DOWN 1",  "JOY_DOWN 2",  "JOY_UP 1",    "JOY_UP 2",
    "JOY_FIRE 1",  "JOY_FIRE 2",  "JOY_FIRE2 1", "JOY_FIRE2 2", "JOY_FIRE3 1", "JOY_FIRE3 2", "JOY_FIRE4 1", "JOY_FIRE4 2",
};

// Строковые настройки: в файле - всё после "=", в странице - буфер текста.
// Секретная в выгрузке настроек не пишется; file - значение выбирается из
// списка файлов.
struct TextDesc {
    const char* name;
    uint16_t offset; // место строки в Settings
    uint8_t max;     // знаков без завершающего нуля
    bool secret;
    bool file;
    const char* comment;
    const char* section;
};

inline constexpr TextDesc kTextKeys[] = {
    {"WIFI_SSID", offsetof(Settings, wifi_ssid), kWifiSsidMax, false, false, "Wi-Fi network name, up to 32 characters; works with WIFI=1", "Wi-Fi network"},
    {"WIFI_PASSWORD", offsetof(Settings, wifi_pass), kWifiPassMax, true, false, "Wi-Fi password, up to 63 characters; empty for an open network", nullptr},
    {"ROM1_FILE", offsetof(Settings, rom_file[0]), kRomFileMax, false, true,
     "64 KB Pentagon ROM image in the card root for slot 1; the board flashes it at boot when the slot differs", "Machine ROM files"},
    {"ROM2_FILE", offsetof(Settings, rom_file[1]), kRomFileMax, false, true, "the same for slot 2", nullptr},
    {"ROM3_FILE", offsetof(Settings, rom_file[2]), kRomFileMax, false, true, "the same for slot 3", nullptr},
};
inline constexpr uint32_t kTextKeyCount = std::size(kTextKeys);

inline char* text_of(Settings& s, const TextDesc& t) {
    return reinterpret_cast<char*>(&s) + t.offset;
}
inline const char* text_of(const Settings& s, const TextDesc& t) {
    return reinterpret_cast<const char*>(&s) + t.offset;
}

// Поля страницы: ключи таблицы, привязки джойстика, строки.
inline constexpr uint32_t kTextFieldFirst = kKeyCount + kJoyKeyFields;
inline constexpr uint32_t kPageFieldCount = kTextFieldFirst + kTextKeyCount;

} // namespace soundsinth::config
