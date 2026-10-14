// SPDX-License-Identifier: MIT
#pragma once

// Джойстик Kempston на порту 0x1F.
//
// Ответ от старшей половины адреса не зависит, поэтому страница не нужна -
// хватает обычной ячейки. Разряды здесь подняты нажатием, а не опущены:
// это не матрица клавиатуры, и покой - нули.

#include <cstdint>

namespace devices::hid {

inline constexpr uint8_t kJoystickPort = 0x1f;

inline constexpr uint8_t kJoyRight = 0x01;
inline constexpr uint8_t kJoyLeft  = 0x02;
inline constexpr uint8_t kJoyDown  = 0x04;
inline constexpr uint8_t kJoyUp    = 0x08;
inline constexpr uint8_t kJoyFire  = 0x10;
// Три кнопки восьмиразрядного варианта.
inline constexpr uint8_t kJoyFire2 = 0x20;
inline constexpr uint8_t kJoyFire3 = 0x40;
inline constexpr uint8_t kJoyFire4 = 0x80;

void joystick_reset();

// Источников два, и они складываются: геймпад и клавиатура в режиме
// "стрелки - это Kempston". Каждый ставит своё состояние целиком - у
// джойстика нет состояния между отчётами, - а на порт уходит сумма.
// Врозь они нужны, чтобы отпускание на одном не гасило другой.
void joystick_set_pad(uint8_t bits);
void joystick_set_keys(uint8_t bits);

uint8_t joystick_state();

// Порт держится, пока его держит хоть один источник: геймпад подключён
// или включён режим клавиш. Иначе выход из режима снимал бы порт
// из-под играющего геймпада.
void joystick_attach();
void joystick_detach();
void joystick_keys_attach();
void joystick_keys_detach();

// Держит ли плата порт джойстика вообще. Решает настройка USB_GAMEPAD: у
// машины порт бывает свой, и отвечать на одно чтение вдвоём нельзя. Пока
// настройка не пришла - не держит. Спрашивает подключение пульта; режим
// стрелок клавиатуры (Win+F2) берёт порт сам.
void joystick_set_allowed(bool on);

// Кто держит порт сейчас - для журнала.
bool joystick_keys_held();
bool joystick_pad_held();
bool joystick_allowed();

// Восьмиразрядный вариант: разряды 5..7 доходят до порта. В
// пятиразрядном они нули при любых источниках.
void joystick_set_wide(bool on);

} // namespace devices::hid
