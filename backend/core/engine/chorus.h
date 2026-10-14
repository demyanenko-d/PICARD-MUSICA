// SPDX-License-Identifier: MIT
#pragma once

// Хорус: одна линия задержки, два отвода с задержкой, которую качает
// треугольный LFO, - левый и правый в противофазе. Моно вход, стерео
// выход, без обратной связи. Один на движок, как ревербератор: в SF2 посыл
// (CC93, chorusEffectsSend) идёт в общий эффект, голоса подмешиваются со
// своим весом. Полная частота: задержка в миллисекундах, прореживание
// снимало бы верх с самого сигнала, а не с хвоста.

#include <cstdint>

#include "core/config.h"

namespace soundsinth::engine {

// Линия - степень двойки: позиция заворачивается маской. 1024 отсчёта -
// 23.2 мс при 44100 Гц, задержка отвода не выходит за 17.6 мс.
inline constexpr uint32_t kChorusLineLen = 1024;

// Уровень возврата по уровню хоруса SysEx 0..127: 64 - умолчание GS и XG,
// это SOUNDSINTH_CHORUS_WET_Q15.
inline constexpr int32_t chorus_wet_for_level(uint8_t level) {
    return SOUNDSINTH_CHORUS_WET_Q15 * static_cast<int32_t>(level) / 64;
}
static_assert(chorus_wet_for_level(64) == SOUNDSINTH_CHORUS_WET_Q15, "level 64 is the default return level");

// Нулевое состояние (Chorus{}) - тишина в линии, LFO в начале.
struct Chorus {
    int16_t line[kChorusLineLen] = {};
    uint32_t pos                 = 0;                         // куда пишется следующий отсчёт
    uint32_t lfo_phase           = 0;                         // фаза LFO, полный круг - 2^32
    int32_t wet_q15              = SOUNDSINTH_CHORUS_WET_Q15; // меняется SysEx по ходу трека
};

// Обрабатывает блок: bus - моно вход (масштаб int16, как у шины
// ревербератора), результат прибавляется к mix_l/mix_r в масштабе шины.
// Вход не портится. Громкость возврата - Chorus::wet_q15.
void chorus_process(Chorus& ch, const int32_t* bus, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames);

} // namespace soundsinth::engine
