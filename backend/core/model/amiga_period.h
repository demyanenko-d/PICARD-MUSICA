// SPDX-License-Identifier: MIT
#pragma once

// Период ProTracker (finetune 0) <-> нота. Общее для загрузчика MOD
// (период -> нота при разборе паттерна) и Voice (нота -> период при
// триггере в модели Amiga).

#include <cstdint>
#include <iterator>

namespace soundsinth::model {

// 3 октавы, C-1..B-3 в терминах Amiga (совпадает с libxmp побайтово). Индекс 0..35 - нота 36+index в общей шкале
// 0..119; так средний диапазон совпадает с другими форматами (XM: C-4 =
// 48).
inline constexpr uint16_t kAmigaPeriodTable[36] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320,
    302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};
inline constexpr uint8_t kAmigaFirstNote  = 36;
inline constexpr uint8_t kAmigaLastNote   = static_cast<uint8_t>(kAmigaFirstNote + std::size(kAmigaPeriodTable) - 1);
inline constexpr uint16_t kAmigaPeriodMax = kAmigaPeriodTable[0];
inline constexpr uint16_t kAmigaPeriodMin = kAmigaPeriodTable[std::size(kAmigaPeriodTable) - 1];

// Период ячейки MOD -> нота, как у OpenMPT: таблица
// ProTracker на семь октав (3424..28, ноты 12..95 - расширенные октавы
// xCHN/xxCH и части M.K.), из двух соседних - ближайшая, при равенстве -
// более высокая. В файлах встречаются периоды не строго по таблице
// (портаменто, округление в оригинальном трекере). kNoteNone при period 0
// и 0xFFF.
uint8_t amiga_period_to_note(uint16_t period);

// Нота -> период: таблица трёх октав, за пределами нот 36..71 - удвоением
// по октавам (период вдвое меньше на октаву выше, вдвое больше ниже), с
// ограничением 16-битным периодом вместо переполнения. Периоды нот 0..120
// посчитаны при сборке; нота больше 120 - как 120.
uint16_t amiga_note_to_period(uint8_t note);

// Период ноты сетки полутонов (amiga_note_to_period, ноты 0..119) для
// glissando. nearest == false - первая нота не ниже звучащей высоты, то
// есть наибольший период ноты, не больший period; true - ближайшая нота,
// граница - середина полутона по высоте (FT2). 0 -> 0.
uint16_t amiga_snap_period(uint16_t period, bool nearest);

// Ограничение периода (возможно отрицательного после сложения со
// смещением вибрато). amiga_limits (kQuirkAmigaLimits) - границами
// kAmigaPeriodTable, 113..856, как у OpenMPT; иначе только 1..65535: без
// флага OpenMPT ноты вне трёх октав не режет (у S3M без флага это около 10%
// нот).
inline uint16_t clamp_amiga_period(int32_t period, bool amiga_limits) {
    const int32_t low  = amiga_limits ? kAmigaPeriodMin : 1;
    const int32_t high = amiga_limits ? kAmigaPeriodMax : 0xffff;
    if (period < low) return static_cast<uint16_t>(low);
    if (period > high) return static_cast<uint16_t>(high);
    return static_cast<uint16_t>(period);
}

} // namespace soundsinth::model
