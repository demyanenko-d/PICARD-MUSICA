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
// ЛЕВЫЙ Shift не настраивается никогда: он и есть Shift, то есть выбор
// верхнего знака клавиши. Если верхней записи нет (буквы, стрелки), он
// становится CAPS SHIFT.
//
// Остальные модификаторы машине отдают по-разному, единого обычая нет:
// роль настраивается.
enum class ModRole : uint8_t { None, CapsShift, SymbolShift };

struct ModifierRoles {
    ModRole right_shift = ModRole::CapsShift;
    ModRole left_ctrl = ModRole::SymbolShift;
    ModRole right_ctrl = ModRole::SymbolShift;
    ModRole left_alt = ModRole::None;
    ModRole right_alt = ModRole::None;
};

// Умолчание нашей раскладки.
ModifierRoles usb_map_default_modifiers();

void usb_map_set_modifiers(const ModifierRoles& roles);
ModifierRoles usb_map_modifiers();

// Клавиатура: загрузочная раскладка - байт модификаторов, байт запаса,
// шесть кодов. От карты нужен только номер отчёта, если устройство шлёт
// его первым байтом. Матрица ставится целиком: отпускания приходят тем же
// отчётом, без кода.
void usb_map_keyboard(const ReportMap& map, const uint8_t* report, uint16_t len);

// Мышь: приращения X и Y и кнопки по карте.
void usb_map_mouse(const ReportMap& map, const uint8_t* report, uint16_t len);

// Джойстик: направления от осей X и Y, а если их нет - от шляпки. Огонь -
// любая кнопка. Мёртвая зона берётся долей логического диапазона, поэтому
// не зависит от разрядности осей.
void usb_map_gamepad(const ReportMap& map, const uint8_t* report, uint16_t len);

// Нет устройства - отпустить всё, иначе зажатое останется висеть.
void usb_map_keyboard_release_all();
void usb_map_gamepad_release_all();

} // namespace devices::hid
