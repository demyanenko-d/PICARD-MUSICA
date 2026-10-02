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

enum class Kind : uint8_t { Flag, Number, Media, Layout, Mod, DiskSys, LiveMidi };

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
    {"LIVE_MIDI", &Settings::live_midi, Kind::LiveMidi, 0, 0,
     "live MIDI plays through the instrument bank: HOOK is the stream from the AY port, "
     "WIRE is a wire on the GPIO33 input; allowed: NONE, HOOK or WIRE",
     nullptr},
    {"AUTO_VOLUME", &Settings::auto_volume, Kind::Flag, 0, 1, "automatic volume control; does not work yet; allowed: 0/1", nullptr},

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

    {"USB_GAMEPAD", &Settings::usb_gamepad, Kind::Flag, 0, 1, "USB gamepad as a Kempston joystick; allowed: 0/1", "USB joystick"},

    {"LOG", &Settings::log, Kind::Flag, 0, 1, "board log on GPIO32, 115200 baud; allowed: 0/1", "Board"},
    {"WIFI", &Settings::wifi, Kind::Flag, 0, 1, "Wi-Fi: takes GPIO34, 38 and 39, so it does not work together with LIVE_MIDI=WIRE; allowed: 0/1", nullptr},
};

inline constexpr uint32_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

} // namespace soundsinth::config
