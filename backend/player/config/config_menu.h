// SPDX-License-Identifier: MIT
#pragma once

// Дерево меню ПЗУ-конфигуратора: какие поля страницы на каком экране, под
// какой надписью и с какой подсказкой.
//
// Поля называются именами ключей файла настроек: таблица ключей остаётся
// одной, меню только раскладывает её по экранам. Ключ без пункта меню
// конфигуратор не покажет - это проверяет тест.

#include <cstdint>
#include <iterator>

#include "player/config/config_tools.h"

namespace soundsinth::config {

// Вид пункта. Числа уходят в страницу как есть.
enum class MenuItemType : uint8_t {
    Blank   = 0, // пустая строка
    Field   = 1, // поле страницы
    Submenu = 2, // переход на экран; при заданном поле справа его значение
    KeyRow  = 3, // строка таблицы: два поля подряд, клавиши 1 и 2
    Save    = 4,
    Exit    = 5, // выход без сохранения
    Back    = 6, // назад на экран выше, как ESC
    Header  = 7, // шапка таблицы клавиш, не выбирается
    Tool    = 8, // экран инструмента; номер ToolId - в menu
};

// Вид экрана: список или таблица клавиш.
enum class MenuKind : uint8_t { List = 0, KeyTable = 1 };

struct MenuItemDesc {
    MenuItemType type;
    const char* key; // имя поля; у KeyRow - первое из двух; nullptr - нет
    uint8_t menu;    // экран перехода у Submenu, инструмент у Tool
    const char* label;
    const char* help; // до kMenuHelpMax знаков: три строки по 30
};

struct MenuDesc {
    const char* title;
    MenuKind kind;
    const MenuItemDesc* items;
    uint8_t count;
};

inline constexpr uint32_t kMenuHelpMax  = 90;
inline constexpr uint32_t kMenuLabelMax = 22;
// Строк списка на экране.
inline constexpr uint32_t kMenuRowsMax = 16;
// Строк над пунктами: на главном экране пустая, на подменю - имя, черта и
// пустая.
inline constexpr uint32_t kMenuMainTop = 1;
inline constexpr uint32_t kMenuSubTop  = 3;

// Номера экранов.
inline constexpr uint8_t kMenuMain     = 0;
inline constexpr uint8_t kMenuDisks    = 1;
inline constexpr uint8_t kMenuAudio    = 2;
inline constexpr uint8_t kMenuKeyboard = 3;
inline constexpr uint8_t kMenuMouse    = 4;
inline constexpr uint8_t kMenuJoystick = 5;
inline constexpr uint8_t kMenuJoyKeys  = 6;
inline constexpr uint8_t kMenuWifi     = 7;
inline constexpr uint8_t kMenuRomSlots = 8;
inline constexpr uint8_t kMenuTools    = 9;
inline constexpr uint8_t kMenuDigital  = 10;

// Таблицы экранов. Отдельное пространство имён - ради короткого T.
namespace menu_tables {

using T = MenuItemType;

inline constexpr uint8_t tool(ToolId t) {
    return static_cast<uint8_t>(t);
}

inline constexpr const char* kBackHelp = "BACK TO THE PREVIOUS SCREEN. ESC DOES THE SAME.";
// Первые строки каждого подменю: назад и пустая.
// clang-format off
#define SOUNDSINTH_BACK {T::Back, nullptr, 0, "BACK [ESC]", kBackHelp}, {T::Blank, nullptr, 0, nullptr, nullptr}
// clang-format on

inline constexpr MenuItemDesc kMainItems[] = {
    {T::Field, "DISKSYS", 0, "DISK SYSTEM", "DIVMMC: BOARD ROM AND CARD. TRDOS: BETA DISK ON THE BOARD WITH DISK IMAGES."},
    {T::Field, "ROM", 0, "MACHINE ROM", "BOARD: MACHINE ROM. SLOT: PENTAGON ROM ON THE BOARD. NOT WITH DIVMMC."},
    {T::Field, "RESET_SIGNAL", 0, "RESET SIGNAL", "THE BOARD MAY RESET THE MACHINE. ALWAYS ON WITH DIVMMC."},
    {T::Submenu, "WIFI", kMenuWifi, "WI-FI", "WI-FI MODULE: NETWORK AND PASSWORD. NOT WITH LIVE MIDI WIRE."},
    {T::Submenu, "KEYBOARD_LAYOUT", kMenuKeyboard, "KEYBOARD", "USB KEYBOARD: LAYOUT, SHIFT KEYS, JOYSTICK KEYS FOR WIN+F2."},
    {T::Submenu, "USB_MOUSE", kMenuMouse, "MOUSE", "USB MOUSE AS KEMPSTON MOUSE."},
    {T::Submenu, "USB_GAMEPAD", kMenuJoystick, "JOYSTICK", "USB GAMEPAD AS KEMPSTON JOYSTICK."},
    {T::Submenu, nullptr, kMenuDisks, "DISKS AND DRIVES", "MEDIA FOR DIVMMC CARDS AND Z-CONTROLLER, SD CARD CLOCK."},
    {T::Submenu, nullptr, kMenuAudio, "AUDIO", "GENERAL SOUND, LIVE MIDI, MOD PANNING."},
    {T::Submenu, nullptr, kMenuRomSlots, "ROM SLOTS", "ROM FILES FROM THE CARD FOR SLOTS 1-3."},
    {T::Submenu, nullptr, kMenuTools, "TOOLS", "CARD, BOARD AND USB INFO, DISK CHECK, CARD SPEED, WI-FI TERMINAL."},
    {T::Field, "LOG", 0, "BOARD LOG", "BOARD LOG ON GPIO32, 115200 BAUD."},
    {T::Blank, nullptr, 0, nullptr, nullptr},
    {T::Save, nullptr, 0, "SAVE [S]", "SAVE THE SETTINGS AND RESTART THE MACHINE."},
    {T::Exit, nullptr, 0, "EXIT WITHOUT SAVE [Q]", "FORGET THE CHANGES AND RESTART THE MACHINE."},
};

inline constexpr MenuItemDesc kDisksItems[] = {
    SOUNDSINTH_BACK,
    {T::Field, "DIVMMC_CARD1", 0, "DIVMMC CARD 1", "MEDIA OF THE CARD ON PORTS E7H/EBH. NONE: NO CARD. DIVMMC NEEDS A CARD."},
    {T::Field, "DIVMMC_CARD2", 0, "DIVMMC CARD 2", "SECOND CARD ON THE SAME PORTS, BIT 1 OF E7H. NONE: NO CARD."},
    {T::Field, "ZCONTROLLER", 0, "Z-CONTROLLER", "Z-CONTROLLER ON PORTS 57H/77H. NONE IF YOUR OWN ANSWERS THERE."},
    {T::Field, "SD_SPEED", 0, "SD CARD MHZ", "ABOVE 25 THE CARD NEEDS HIGH SPEED. TEST IN TOOLS."},
};

inline constexpr MenuItemDesc kAudioItems[] = {
    SOUNDSINTH_BACK,
    {T::Field, "GS", 0, "GENERAL SOUND", "GENERAL SOUND ON PORTS BBH/B3H."},
    {T::Field, "GS_FX_SEPARATE", 0, "GS FX SEPARATE", "GS EFFECTS ON OWN VOICES. OFF: AS THE REAL CARD."},
    {T::Field, "LIVE_MIDI", 0, "LIVE MIDI", "HOOK: MIDI FROM THE AY PORT. WIRE: MIDI WIRE ON GPIO33."},
    {T::Field, "AY_EMULATION", 0, "AY EMULATION", "TS: TWO YM2149. TSFM: TWO YM2203 WITH FM."},
    {T::Field, "AY_STEREO", 0, "AY STEREO", "ACB AS PENTAGON."},
    {T::Field, "SAA1099", 0, "SAA1099", "MULTISOUND SAA1099 ON PORTS FFH/1FFH."},
    {T::Submenu, nullptr, kMenuDigital, "DIGITAL SOUND", "COVOX, SOUNDRIVE AND BEEPER OF THE MACHINE."},
    {T::Field, "STOP_MUSIC_ON_RESET", 0, "STOP MUSIC ON RESET", "GS, MODULES AND MIDI STOP ON A MACHINE RESET."},
    {T::Field, "AUTO_VOLUME", 0, "AUTO VOLUME", "LEVELS TRACKS TO ONE LOUDNESS, -12..+18 DB."},
    {T::Field, "MOD_PANNING", 0, "MOD PANNING", "MOD STEREO WIDTH, PER CENT: 100 AS AMIGA, 0 MONO."},
    {T::Field, "MOD_INTERPOLATION", 0, "MOD INTERPOLATION", "ON: SMOOTH AS PC. OFF: STEPS AS AMIGA PAULA, BRIGHTER."},
};

inline constexpr MenuItemDesc kDigitalItems[] = {
    SOUNDSINTH_BACK,
    {T::Field, "COVOX", 0, "COVOX", "COVOX ON PORT FBH, BOTH CHANNELS."},
    {T::Field, "SOUNDRIVE", 0, "SOUNDRIVE", "SOUNDRIVE 1.05: 0FH/1FH LEFT, 4FH/5FH RIGHT."},
    {T::Field, "BEEPER", 0, "BEEPER", "1BIT: BIT 4 OF FEH. 2BIT: BIT 4 AND BIT 3 (TAPE OUT), AS THE 48K."},
};

inline constexpr MenuItemDesc kKeyboardItems[] = {
    SOUNDSINTH_BACK,
    {T::Field, "KEYBOARD_LAYOUT", 0, "LAYOUT", "USB KEYBOARD LAYOUT. NONE TURNS THE KEYBOARD OFF."},
    {T::Field, "MOD_RIGHT_SHIFT", 0, "RIGHT SHIFT", "WHAT THE RIGHT SHIFT BECOMES."},
    {T::Field, "MOD_LEFT_CTRL", 0, "LEFT CTRL", "WHAT THE LEFT CTRL BECOMES."},
    {T::Field, "MOD_RIGHT_CTRL", 0, "RIGHT CTRL", "WHAT THE RIGHT CTRL BECOMES."},
    {T::Field, "MOD_LEFT_ALT", 0, "LEFT ALT", "WHAT THE LEFT ALT BECOMES."},
    {T::Field, "MOD_RIGHT_ALT", 0, "RIGHT ALT", "WHAT THE RIGHT ALT BECOMES."},
    {T::Submenu, nullptr, kMenuJoyKeys, "JOYSTICK KEYS", "KEYS THAT ACT AS KEMPSTON JOYSTICK IN THE WIN+F2 MODE."},
};

inline constexpr MenuItemDesc kMouseItems[] = {
    SOUNDSINTH_BACK,
    {T::Field, "USB_MOUSE", 0, "USB MOUSE", "USB MOUSE AS KEMPSTON MOUSE."},
    {T::Field, "MOUSE_SPEED", 0, "SPEED", "IN QUARTERS: 4 IS ONE TO ONE, 8 IS TWICE AS FAST."},
    {T::Field, "MOUSE_SWAP_BUTTONS", 0, "SWAP BUTTONS", "SWAP THE LEFT AND RIGHT BUTTONS."},
    {T::Field, "MOUSE_AUTOSWAP", 0, "AUTOSWAP", "BOTH BUTTONS HALF A SECOND SWAP LEFT AND RIGHT UNTIL POWER OFF."},
};

inline constexpr MenuItemDesc kJoystickItems[] = {
    SOUNDSINTH_BACK,
    {T::Field, "USB_GAMEPAD", 0, "USB GAMEPAD", "USB GAMEPAD AS KEMPSTON JOYSTICK."},
    {T::Field, "JOY_8BIT", 0, "8 BIT KEMPSTON", "BITS 5..7 ARE THREE MORE BUTTONS. OFF: ALWAYS ZERO."},
};

inline constexpr const char* kJoyKeyHelp = "ENTER: PRESS A NEW KEY. DEL: CLEAR. FIRE2..4 NEED 8 BIT KEMPSTON IN JOYSTICK.";

inline constexpr MenuItemDesc kJoyKeyItems[] = {
    SOUNDSINTH_BACK,
    {T::Header, nullptr, 0, nullptr, nullptr},
    {T::KeyRow, "JOY_RIGHT 1", 0, "RIGHT", kJoyKeyHelp},
    {T::KeyRow, "JOY_LEFT 1", 0, "LEFT", kJoyKeyHelp},
    {T::KeyRow, "JOY_DOWN 1", 0, "DOWN", kJoyKeyHelp},
    {T::KeyRow, "JOY_UP 1", 0, "UP", kJoyKeyHelp},
    {T::KeyRow, "JOY_FIRE 1", 0, "FIRE", kJoyKeyHelp},
    {T::KeyRow, "JOY_FIRE2 1", 0, "FIRE2", kJoyKeyHelp},
    {T::KeyRow, "JOY_FIRE3 1", 0, "FIRE3", kJoyKeyHelp},
    {T::KeyRow, "JOY_FIRE4 1", 0, "FIRE4", kJoyKeyHelp},
};

inline constexpr MenuItemDesc kWifiItems[] = {
    SOUNDSINTH_BACK,
    {T::Field, "WIFI", 0, "WI-FI", "WI-FI MODULE ON GPIO34/38/39. NOT WITH LIVE MIDI WIRE."},
    {T::Field, "WIFI_SSID", 0, "NETWORK", "WI-FI NETWORK NAME, UP TO 32 CHARACTERS."},
    {T::Field, "WIFI_PASSWORD", 0, "PASSWORD", "WI-FI PASSWORD, UP TO 63 CHARACTERS. EMPTY FOR AN OPEN NETWORK."},
};

inline constexpr MenuItemDesc kRomSlotItems[] = {
    SOUNDSINTH_BACK,
    {T::Field, "ROM1_FILE", 0, "SLOT 1", "64 KB PENTAGON ROM FILE FROM THE CARD ROOT. WRITTEN TO THE BOARD ON SAVE."},
    {T::Field, "ROM2_FILE", 0, "SLOT 2", "64 KB PENTAGON ROM FILE FROM THE CARD ROOT. WRITTEN TO THE BOARD ON SAVE."},
    {T::Field, "ROM3_FILE", 0, "SLOT 3", "64 KB PENTAGON ROM FILE FROM THE CARD ROOT. WRITTEN TO THE BOARD ON SAVE."},
};

inline constexpr MenuItemDesc kToolItems[] = {
    SOUNDSINTH_BACK,
    {T::Tool, nullptr, tool(ToolId::CardInfo), "CARD INFO", "CARD MAKER, SIZE, FILE SYSTEM AND FREE SPACE."},
    {T::Tool, nullptr, tool(ToolId::BoardInfo), "BOARD INFO", "FIRMWARE VERSION, CHIP, MEMORY."},
    {T::Tool, nullptr, tool(ToolId::UsbInfo), "USB DEVICES", "CONNECTED USB DEVICES AND WHAT THEY ARE USED FOR."},
    {T::Tool, nullptr, tool(ToolId::UsbHid), "USB HID TEST", "MICE, KEYBOARDS, GAMEPADS: WHAT THEY SEND. ENTER SAVES USB_HID.TXT TO THE CARD."},
    {T::Blank, nullptr, 0, nullptr, nullptr},
    {T::Tool, nullptr, tool(ToolId::CardCheck), "CHECK CARD", "COUNTS FREE SPACE ON THE CARD AND FIXES FSINFO. CHECKS BOTH FAT COPIES."},
    {T::Tool, nullptr, tool(ToolId::UsbCheck), "CHECK USB DISK", "THE SAME FOR THE USB DISK."},
    {T::Tool, nullptr, tool(ToolId::CardSpeed), "CARD SPEED TEST", "READS THE CARD AT EVERY CLOCK AND COUNTS ERRORS. WRITES NOTHING."},
    {T::Blank, nullptr, 0, nullptr, nullptr},
    {T::Tool, nullptr, tool(ToolId::AtTerminal), "WI-FI AT TERMINAL", "TYPE AT COMMANDS TO THE WI-FI MODULE. UP AND DOWN GIVE READY ONES."},
};

#undef SOUNDSINTH_BACK

} // namespace menu_tables

#define SOUNDSINTH_MENU(title, kind, items) {title, kind, items, static_cast<uint8_t>(std::size(items))}

inline constexpr MenuDesc kMenus[] = {
    SOUNDSINTH_MENU("SETUP", MenuKind::List, menu_tables::kMainItems),
    SOUNDSINTH_MENU("DISKS AND DRIVES", MenuKind::List, menu_tables::kDisksItems),
    SOUNDSINTH_MENU("AUDIO", MenuKind::List, menu_tables::kAudioItems),
    SOUNDSINTH_MENU("KEYBOARD", MenuKind::List, menu_tables::kKeyboardItems),
    SOUNDSINTH_MENU("MOUSE", MenuKind::List, menu_tables::kMouseItems),
    SOUNDSINTH_MENU("JOYSTICK", MenuKind::List, menu_tables::kJoystickItems),
    SOUNDSINTH_MENU("JOYSTICK KEYS (WIN+F2)", MenuKind::KeyTable, menu_tables::kJoyKeyItems),
    SOUNDSINTH_MENU("WI-FI", MenuKind::List, menu_tables::kWifiItems),
    SOUNDSINTH_MENU("ROM SLOTS", MenuKind::List, menu_tables::kRomSlotItems),
    SOUNDSINTH_MENU("TOOLS", MenuKind::List, menu_tables::kToolItems),
    SOUNDSINTH_MENU("DIGITAL SOUND", MenuKind::List, menu_tables::kDigitalItems),
};

#undef SOUNDSINTH_MENU

inline constexpr uint32_t kMenuCount = std::size(kMenus);

// Номер экрана - его место в kMenus.
inline constexpr bool menu_at(uint8_t n, const MenuItemDesc* items) {
    return kMenus[n].items == items;
}
static_assert(menu_at(kMenuMain, menu_tables::kMainItems) && menu_at(kMenuDisks, menu_tables::kDisksItems) && menu_at(kMenuAudio, menu_tables::kAudioItems) &&
                  menu_at(kMenuKeyboard, menu_tables::kKeyboardItems) && menu_at(kMenuMouse, menu_tables::kMouseItems) &&
                  menu_at(kMenuJoystick, menu_tables::kJoystickItems) && menu_at(kMenuJoyKeys, menu_tables::kJoyKeyItems) &&
                  menu_at(kMenuWifi, menu_tables::kWifiItems) && menu_at(kMenuRomSlots, menu_tables::kRomSlotItems) &&
                  menu_at(kMenuTools, menu_tables::kToolItems) && menu_at(kMenuDigital, menu_tables::kDigitalItems) && kMenuDigital + 1u == kMenuCount,
              "kMenus order differs from the kMenu* screen numbers");

} // namespace soundsinth::config
