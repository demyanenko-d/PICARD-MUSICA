// SPDX-License-Identifier: MIT
#pragma once

// Отчёты USB HID -> состояние эмулируемых устройств ZX.
//
// Порт отдаёт сюда карту полей, снятую с дескриптора при подключении, и
// принятый отчёт. Поле ищется по назначению, а не по смещению в байтах.

#include <cstdint>

#include "devices/hid/report_map.h"

namespace devices::hid {

// --- Модификаторы ---
//
// Левый Shift не настраивается никогда: он и есть Shift, то есть выбор
// верхнего знака клавиши. Если верхней записи нет (буквы, стрелки), он
// становится CAPS SHIFT.
//
// Остальные модификаторы машине отдают по-разному, единого обычая нет:
// роль настраивается.
enum class ModRole : uint8_t { None, CapsShift, SymbolShift };

struct ModifierRoles {
    ModRole right_shift = ModRole::CapsShift;
    ModRole left_ctrl   = ModRole::SymbolShift;
    ModRole right_ctrl  = ModRole::SymbolShift;
    ModRole left_alt    = ModRole::None;
    ModRole right_alt   = ModRole::None;
};

// Умолчание нашей раскладки.
ModifierRoles usb_map_default_modifiers();

void usb_map_set_modifiers(const ModifierRoles& roles);

// Клавиши действий джойстика в режиме Win+F2: строка на разряд порта
// Kempston, коды HID, ноль - клавиши нет.
inline constexpr uint32_t kJoyKeyActions = 8;
inline constexpr uint32_t kJoyKeySlots   = 2;
// Захват клавиши для привязки в конфигураторе: после взвода ждётся отчёт
// без клавиш, затем берётся первый код следующего. Клавиша, которой
// взводили (Enter), так в захват не попадает.
void usb_map_capture_arm();
void usb_map_capture_cancel();
// true - клавиша поймана, code - её код HID; захват снят.
bool usb_map_capture_take(uint8_t& code);

void usb_map_set_joy_keys(const uint8_t (&keys)[kJoyKeyActions][kJoyKeySlots]);
ModifierRoles usb_map_modifiers();

// Нажатий по кодам, которых нет в раскладке (F1..F10, Pause, PrintScreen,
// Menu, часть цифровой клавиатуры). Для пользователя это "клавиша не
// работает"; число отличает это от промаха дескриптора и потери отчёта.
uint32_t usb_map_unmapped_presses();

// Клавиатура: загрузочная раскладка - байт модификаторов, байт запаса,
// шесть кодов. От карты нужен только номер отчёта, если устройство шлёт
// его первым байтом. Матрица ставится целиком: отпускания приходят тем же
// отчётом, без кода.
void usb_map_keyboard(const ReportMap& map, const uint8_t* report, uint16_t len);

// Мышь: скорость в четвертях (4 - один к одному) и перестановка левой с
// правой. Приращение умножается до обрезки байтом: у Kempston счётчик
// восьмиразрядный, и быстрое движение при большой скорости упирается в
// него, а не заворачивается.
struct MouseTuning {
    uint8_t speed     = 4;
    bool swap_buttons = false;
    // Разрешение жеста: обе кнопки, зажатые полсекунды, меняют левую и
    // правую местами, без настроек и перезагрузки. Выключен: обе кнопки
    // разом нажимают и в играх.
    bool autoswap = false;
};
void usb_map_set_mouse(const MouseTuning& t);

// Переставлены ли кнопки сейчас: жест переключает это на ходу, в настройках
// остаётся то, с чего машина поднимается.
bool usb_map_mouse_swapped();

// Мышь: приращения X и Y и кнопки по карте.
void usb_map_mouse(const ReportMap& map, const uint8_t* report, uint16_t len);

// Джойстик: направления от осей X и Y и от шляпки, огонь - любая кнопка.
// Мёртвая зона - доля логического диапазона, от разрядности осей не зависит.
void usb_map_gamepad(const ReportMap& map, const uint8_t* report, uint16_t len);

// --- Быстрый режим управления ---
//
// Win+F1 - обычный: стрелки, Enter и пробел идут в матрицу, как всегда.
// Win+F2 - стрелки становятся направлениями Kempston, Enter и пробел -
// огнём; в матрицу они при этом не идут вовсе.
//
// Переключается с клавиатуры и живёт до выключения: в настройках поля нет,
// машина поднимается в обычном режиме.
enum class KeyMode : uint8_t { Normal, KempstonArrows };

KeyMode usb_map_key_mode();
void usb_map_set_key_mode(KeyMode mode);

// Нет устройства - отпустить всё, иначе зажатое останется висеть.
void usb_map_keyboard_release_all();
void usb_map_gamepad_release_all();

} // namespace devices::hid
