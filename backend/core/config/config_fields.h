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

#include <cstdint>

#include "core/config/config.h"

namespace soundsinth::config {

enum class Kind : uint8_t { Flag, Number, Media, Layout, Mod, DiskSys, LiveMidi, Key };
inline constexpr uint32_t kKindCount = 8;

// Именованные значения: одна таблица на разбор и на запись, поэтому
// написанное платой она прочитает обратно.
struct Named {
    const char* name;
    uint8_t value;
};

inline constexpr Named kMediaNames[] = {
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
    {"NONE", 0x00},   {"A", 0x04},        {"B", 0x05},           {"C", 0x06},          {"D", 0x07},         {"E", 0x08},       {"F", 0x09},
    {"G", 0x0a},      {"H", 0x0b},        {"I", 0x0c},           {"J", 0x0d},          {"K", 0x0e},         {"L", 0x0f},       {"M", 0x10},
    {"N", 0x11},      {"O", 0x12},        {"P", 0x13},           {"Q", 0x14},          {"R", 0x15},         {"S", 0x16},       {"T", 0x17},
    {"U", 0x18},      {"V", 0x19},        {"W", 0x1a},           {"X", 0x1b},          {"Y", 0x1c},         {"Z", 0x1d},       {"1", 0x1e},
    {"2", 0x1f},      {"3", 0x20},        {"4", 0x21},           {"5", 0x22},          {"6", 0x23},         {"7", 0x24},       {"8", 0x25},
    {"9", 0x26},      {"0", 0x27},        {"ENTER", 0x28},       {"BACKSPACE", 0x2a},  {"TAB", 0x2b},       {"SPACE", 0x2c},   {"MINUS", 0x2d},
    {"EQUAL", 0x2e},  {"LBRACKET", 0x2f}, {"RBRACKET", 0x30},    {"BACKSLASH", 0x31},  {"SEMICOLON", 0x33}, {"QUOTE", 0x34},   {"GRAVE", 0x35},
    {"COMMA", 0x36},  {"PERIOD", 0x37},   {"SLASH", 0x38},       {"CAPSLOCK", 0x39},   {"F1", 0x3a},        {"F2", 0x3b},      {"F3", 0x3c},
    {"F4", 0x3d},     {"F5", 0x3e},       {"F6", 0x3f},          {"F7", 0x40},         {"F8", 0x41},        {"F9", 0x42},      {"F10", 0x43},
    {"F11", 0x44},    {"F12", 0x45},      {"PRINTSCREEN", 0x46}, {"SCROLLLOCK", 0x47}, {"PAUSE", 0x48},     {"INSERT", 0x49},  {"HOME", 0x4a},
    {"PAGEUP", 0x4b}, {"DELETE", 0x4c},   {"END", 0x4d},         {"PAGEDOWN", 0x4e},   {"RIGHT", 0x4f},     {"LEFT", 0x50},    {"DOWN", 0x51},
    {"UP", 0x52},     {"NUMLOCK", 0x53},  {"KP_SLASH", 0x54},    {"KP_STAR", 0x55},    {"KP_MINUS", 0x56},  {"KP_PLUS", 0x57}, {"KP_ENTER", 0x58},
    {"KP_1", 0x59},   {"KP_2", 0x5a},     {"KP_3", 0x5b},        {"KP_4", 0x5c},       {"KP_5", 0x5d},      {"KP_6", 0x5e},    {"KP_7", 0x5f},
    {"KP_8", 0x60},   {"KP_9", 0x61},     {"KP_0", 0x62},        {"KP_DOT", 0x63},
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
            return {kMediaNames, sizeof(kMediaNames) / sizeof(kMediaNames[0])};
        case Kind::Layout:
            return {kLayoutNames, sizeof(kLayoutNames) / sizeof(kLayoutNames[0])};
        case Kind::Mod:
            return {kModNames, sizeof(kModNames) / sizeof(kModNames[0])};
        case Kind::DiskSys:
            return {kDiskSysNames, sizeof(kDiskSysNames) / sizeof(kDiskSysNames[0])};
        case Kind::LiveMidi:
            return {kLiveMidiNames, sizeof(kLiveMidiNames) / sizeof(kLiveMidiNames[0])};
        case Kind::Key:
            return {kKeyNames, sizeof(kKeyNames) / sizeof(kKeyNames[0])};
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
     "TRDOS is the TR-DOS trigger on the DOS_N signal; allowed: NONE, DIVMMC or TRDOS",
     "Memory and disks"},
    {"DISKSYS_MEDIA", &Settings::disksys_media, Kind::Media, 0, 0, "where DivMMC takes its sectors from; allowed: SD or USB", nullptr},
    {"ZCONTROLLER", &Settings::zcontroller, Kind::Flag, 0, 1,
     "Z-Controller emulation on ports 57h/77h; turn it off if your own hardware answers there; allowed: 0/1", nullptr},
    {"ZCONTROLLER_MEDIA", &Settings::zcontroller_media, Kind::Media, 0, 0, "where Z-Controller takes its sectors from; allowed: SD or USB", nullptr},

    {"GS", &Settings::gs, Kind::Flag, 0, 1, "General Sound emulation on ports BBh/B3h; allowed: 0/1", "Audio"},
    {"GS_FX_SEPARATE", &Settings::gs_fx_separate, Kind::Flag, 0, 1,
     "General Sound effects play on their own voices instead of taking a module channel; 0 is what the real card does and what games expect; "
     "allowed: 0/1",
     nullptr},
    {"LIVE_MIDI", &Settings::live_midi, Kind::LiveMidi, 0, 0,
     "live MIDI plays through the instrument bank: HOOK is the stream from the AY port, "
     "WIRE is a wire on the GPIO33 input; allowed: NONE, HOOK or WIRE",
     nullptr},
    {"AUTO_VOLUME", &Settings::auto_volume, Kind::Flag, 0, 1, "automatic volume control; does not work yet; allowed: 0/1", nullptr},
    {"MOD_PANNING", &Settings::mod_panning, Kind::Number, 0, kModPanningMax,
     "how wide MOD channels are spread, per cent of the Amiga hardware layout: 100 is hard left/right as on the real thing, 0 is all centred; allowed: "
     "0..100",
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
};

inline constexpr uint32_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

// Поля привязок в странице конфигуратора идут за ключами таблицы: по
// клавише действия на поле, действие за действием.
inline constexpr uint32_t kJoyKeyFields                       = kJoyActions * kJoyKeysPerAction;
inline constexpr const char* kJoyKeyFieldNames[kJoyKeyFields] = {
    "JOY_RIGHT 1", "JOY_RIGHT 2", "JOY_LEFT 1",  "JOY_LEFT 2",  "JOY_DOWN 1",  "JOY_DOWN 2",  "JOY_UP 1",    "JOY_UP 2",
    "JOY_FIRE 1",  "JOY_FIRE 2",  "JOY_FIRE2 1", "JOY_FIRE2 2", "JOY_FIRE3 1", "JOY_FIRE3 2", "JOY_FIRE4 1", "JOY_FIRE4 2",
};
inline constexpr const char* kJoyKeySection = "Joystick keys for Win+F2";
inline constexpr const char* kJoyKeyComment = "ENTER, then press the key on the USB keyboard; ESC cancels, DEL clears; FIRE2..4 need JOY_8BIT=1";

inline constexpr uint32_t kPageFieldCount = kKeyCount + kJoyKeyFields;

} // namespace soundsinth::config
