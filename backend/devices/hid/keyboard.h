#pragma once

// Клавиатура ZX на порту 0xFE.
//
// Ответ зависит от старшей половины адреса: каждый её нулевой разряд
// выбирает полустроку, и процессор читает И по всем выбранным. Поэтому
// ответы лежат страницей на 256 байт - по одному на каждое сочетание, - а
// плата в цикле шины только берёт байт по адресу. Страница
// пересчитывается при смене состояния клавиш.
//
// Полустроки и разряды - как у машины:
//
//   A8   CAPS SHIFT  Z  X  C  V
//   A9   A  S  D  F  G
//   A10  Q  W  E  R  T
//   A11  1  2  3  4  5
//   A12  0  9  8  7  6
//   A13  P  O  I  U  Y
//   A14  ENTER  L  K  J  H
//   A15  SPACE  SYM SHIFT  M  N  B
//
// Разряд 0 - первая клавиша строки. Ноль значит нажата.

#include <cstdint>

namespace devices::hid {

inline constexpr uint8_t kKeyboardPort = 0xFE;
inline constexpr uint8_t kKeyboardRows = 8;
inline constexpr uint8_t kKeyboardKeysPerRow = 5;

// Разряды 5..7 ответа: 5 не используется, 6 - вход магнитофона, 7 не
// используется. Магнитофона нет, поэтому все три единицы.
inline constexpr uint8_t kKeyboardIdleBits = 0xE0;

// Состояние матрицы: разряды 0..4 каждой строки, ноль - нажата.
void keyboard_reset();
void keyboard_set_key(uint8_t row, uint8_t key, bool pressed);
void keyboard_set_rows(const uint8_t rows[kKeyboardRows]);

// Страница ответов, 256 байт, выровнена на 256. Пересчитывается при каждой
// смене состояния, читать её можно в любой момент.
const uint8_t* keyboard_page();

// Отвечать на порт клавиатуры или перестать.
//
// По умолчанию плата на него не отвечает: на машине с собственной
// клавиатурой порт ведёт ULA, и два источника в одном цикле - это
// столкновение на шине.
void keyboard_attach();
void keyboard_detach();

} // namespace devices::hid
