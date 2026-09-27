#pragma once

// Мышь Kempston: три порта с общим младшим байтом 0xDF, различает их
// старшая половина адреса.
//
//   0xFADF  кнопки
//   0xFBDF  X
//   0xFFDF  Y
//
// Поэтому ответы лежат страницей на 256 байт: у порта 0xDF байт выбирает
// старший байт адреса. Остальные его сочетания отвечают 0xFF - как
// неподведённая шина.
//
// Координаты - свободно бегущие восьмиразрядные счётчики, чтение их не
// меняет: программа сама следит за переполнением. Y растёт вверх.

#include <cstdint>

namespace devices::hid {

inline constexpr uint8_t kMousePort = 0xDF;
inline constexpr uint8_t kMouseHiButtons = 0xFA;
inline constexpr uint8_t kMouseHiX = 0xFB;
inline constexpr uint8_t kMouseHiY = 0xFF;

// Разряды кнопок, ноль - нажата; старшие не используются и подняты.
inline constexpr uint8_t kMouseButtonRight = 0x01;
inline constexpr uint8_t kMouseButtonLeft = 0x02;
inline constexpr uint8_t kMouseButtonMiddle = 0x04;
inline constexpr uint8_t kMouseButtonsIdle = 0xFF;

void mouse_reset();
void mouse_move(int8_t dx, int8_t dy);
void mouse_set_buttons(uint8_t mask); // разряды kMouseButton*, ноль - нажата

const uint8_t* mouse_page();

void mouse_attach();
void mouse_detach();

} // namespace devices::hid
