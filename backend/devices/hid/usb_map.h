#pragma once

// Отчёты USB HID -> состояние эмулируемых устройств ZX.
//
// Разбор здесь, а не в порту: он платформы не касается и проверяется на
// ПК. Порт отдаёт сюда карту полей, снятую с дескриптора при подключении,
// и принятый отчёт.
//
// Благодаря карте незнакомое устройство работает невиданным: поле ищется
// по назначению, а не по смещению в байтах. Жёстких раскладок здесь нет
// ни одной - даже загрузочная мышь приходит картой (report_map_boot_mouse).

#include <cstdint>

#include "devices/hid/report_map.h"

namespace devices::hid {

// --- Модификаторы ---
//
// ЛЕВЫЙ Shift не настраивается никогда: он и есть Shift, то есть выбор
// верхнего знака клавиши. Если для клавиши верхней записи нет (буквы,
// стрелки), он становится CAPS SHIFT - выходит заглавная буква, как и
// ждёшь.
//
// Остальные машине можно отдать по-разному, и единого обычая нет: правый
// Shift держат за SYMBOL SHIFT в трёх известных раскладках из шести, за
// CAPS SHIFT в остальных. Поэтому роль настраивается, а умолчание берётся
// из раскладки.
enum class ModRole : uint8_t { None, CapsShift, SymbolShift };

struct ModifierRoles {
    ModRole right_shift = ModRole::CapsShift;
    ModRole left_ctrl = ModRole::SymbolShift;
    ModRole right_ctrl = ModRole::SymbolShift;
    ModRole left_alt = ModRole::None;
    ModRole right_alt = ModRole::None;
};

// Умолчание нашей раскладки. Отсюда же берут начальное состояние
// настройки.
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
