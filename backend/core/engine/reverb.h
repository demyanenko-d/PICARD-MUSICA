// SPDX-License-Identifier: MIT
#pragma once

// Ревербератор Шрёдера (схема Freeverb): восемь гребёнок параллельно, затем
// всепропускающие; моно вход, стерео выход. Один на движок: в SF2 посыл
// (CC91, reverbEffectsSend) идёт в общий эффект, голоса подмешиваются со
// своим весом. Половинная частота (kReverbRateDiv): потеря выше 11 кГц в
// хвосте не слышна. Линии int16 в SRAM: в PSRAM двенадцать разбросанных
// чтений и записей на отсчёт промахиваются мимо кэша XIP.

#include <cstdint>

#include "core/config.h"

namespace soundsinth::engine {

// Внутренняя частота = частота выхода / kReverbRateDiv. Длины линий
// делятся на неё же.
inline constexpr uint32_t kReverbRateDiv = 2;

// Множитель длин Freeverb: kReverbSizeNum / kReverbSizeDen. При 3/2 хвост
// длиннее, но гребёнки 38..55 мс дают редкие отражения: неровность огибающей
// хвоста 1.28 дБ против 0.92 при 1/1, на слух - отдельные повторы. Длину
// хвоста задаёт обратная связь.
inline constexpr uint32_t kReverbSizeNum = 1;
inline constexpr uint32_t kReverbSizeDen = 1;

inline constexpr uint32_t kReverbCombCount    = 8;
inline constexpr uint32_t kReverbAllpassCount = 4;

// Длины Freeverb для 44100 Гц, у всех линий разные.
inline constexpr uint16_t kReverbCombLen44k[kReverbCombCount]       = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
inline constexpr uint16_t kReverbAllpassLen44k[kReverbAllpassCount] = {556, 441, 341, 225};

// Длина линии во внутренних отсчётах по длине Freeverb для 44100 Гц.
inline constexpr uint32_t reverb_line_len(uint16_t len44k) {
    return static_cast<uint32_t>(len44k) * kReverbSizeNum / kReverbSizeDen / kReverbRateDiv;
}

inline constexpr uint32_t reverb_line_total() {
    uint32_t n = 0;
    for (uint16_t v : kReverbCombLen44k) {
        n += reverb_line_len(v);
    }
    for (uint16_t v : kReverbAllpassLen44k) {
        n += reverb_line_len(v);
    }
    return n;
}

inline constexpr uint32_t kReverbSampleCount = reverb_line_total();

// Обратная связь гребёнок по умолчанию, Q15: 0.95.
inline constexpr int32_t kReverbFeedbackQ15 = 31130;

// Уровень возврата по уровню реверберации SysEx 0..127: 64 - умолчание GS и
// XG, это SOUNDSINTH_REVERB_WET_Q15; 127 - почти вдвое громче.
inline constexpr int32_t reverb_wet_for_level(uint8_t level) {
    return SOUNDSINTH_REVERB_WET_Q15 * static_cast<int32_t>(level) / 64;
}

// Обратная связь по времени хвоста SysEx 0..127: 0 - 0.86, 64 - 0.95
// (умолчание), 127 - 0.96. Выше 0.96 гребёнки int16 рискуют насыщением.
inline constexpr int32_t reverb_feedback_for_time(uint8_t time) {
    constexpr int32_t kShort = 28180, kLong = 31457;
    return time <= 64 ? kShort + (kReverbFeedbackQ15 - kShort) * time / 64 : kReverbFeedbackQ15 + (kLong - kReverbFeedbackQ15) * (time - 64) / 63;
}
static_assert(reverb_wet_for_level(64) == SOUNDSINTH_REVERB_WET_Q15, "level 64 is the default return level");
static_assert(reverb_feedback_for_time(64) == kReverbFeedbackQ15, "time 64 is the default feedback");

// Нулевое состояние (Reverb{}) - тишина в линиях.
struct Reverb {
    int16_t lines[kReverbSampleCount]         = {};
    uint16_t comb_pos[kReverbCombCount]       = {};
    uint16_t allpass_pos[kReverbAllpassCount] = {};
    // Последний выданный возврат - для линейной интерполяции обратно на
    // выходную частоту. Живёт между блоками, иначе на границе блока ступенька.
    int32_t prev_l = 0;
    int32_t prev_r = 0;
    // Уровень возврата и обратная связь, Q15. Меняются SysEx по ходу трека.
    int32_t wet_q15      = SOUNDSINTH_REVERB_WET_Q15;
    int32_t feedback_q15 = kReverbFeedbackQ15;
};

// Обрабатывает блок: bus - моно вход (масштаб int16, как у mix_l/mix_r),
// результат прибавляется к mix_l/mix_r. Вход не портится.
//
// Общая громкость возврата - SOUNDSINTH_REVERB_WET_Q15; посыл задаёт каждый
// голос при накоплении шины.
void reverb_process(Reverb& rv, const int32_t* bus, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames);

} // namespace soundsinth::engine
