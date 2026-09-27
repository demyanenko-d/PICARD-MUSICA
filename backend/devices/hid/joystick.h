#pragma once

// Джойстик Kempston на порту 0x1F.
//
// Ответ от старшей половины адреса не зависит, поэтому страница не нужна -
// хватает обычной ячейки. Разряды здесь подняты нажатием, а не опущены:
// это не матрица клавиатуры, и покой - нули.

#include <cstdint>

namespace devices::hid {

inline constexpr uint8_t kJoystickPort = 0x1F;

inline constexpr uint8_t kJoyRight = 0x01;
inline constexpr uint8_t kJoyLeft = 0x02;
inline constexpr uint8_t kJoyDown = 0x04;
inline constexpr uint8_t kJoyUp = 0x08;
inline constexpr uint8_t kJoyFire = 0x10;

void joystick_reset();

// Разряды kJoy*, единица - нажато. Ставится целиком: у джойстика нет
// состояния между отчётами.
void joystick_set(uint8_t bits);

uint8_t joystick_state();

void joystick_attach();
void joystick_detach();

} // namespace devices::hid
