#pragma once

// Ревербератор Шрёдера (схема Freeverb): восемь гребёнок параллельно, затем
// всепропускающие; моно вход, стерео выход. Один на движок: в SF2 посыл
// (CC91, reverbEffectsSend) идёт в общий эффект, голоса подмешиваются со
// своим весом. Половинная частота (kReverbRateDiv): потеря выше 11 кГц в
// хвосте не слышна. Линии int16 в SRAM: в PSRAM двенадцать разбросанных
// чтений и записей на отсчёт промахиваются мимо кэша XIP.

#include <cstdint>

namespace soundsinth::engine {

// Внутренняя частота = частота выхода / kReverbRateDiv. 1 - полная
// (25 КБ), 2 - половинная (12.6 КБ). Длины линий делятся на неё же.
inline constexpr uint32_t kReverbRateDiv = 2;

inline constexpr uint32_t kReverbCombCount = 8;
inline constexpr uint32_t kReverbAllpassCount = 4;

// Длины Freeverb для 44100 Гц, у всех линий разные.
inline constexpr uint16_t kReverbCombLen44k[kReverbCombCount] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
inline constexpr uint16_t kReverbAllpassLen44k[kReverbAllpassCount] = {556, 441, 341, 225};

inline constexpr uint32_t reverb_line_total() {
    uint32_t n = 0;
    for (uint16_t v : kReverbCombLen44k) {
        n += v / kReverbRateDiv;
    }
    for (uint16_t v : kReverbAllpassLen44k) {
        n += v / kReverbRateDiv;
    }
    return n;
}

inline constexpr uint32_t kReverbSampleCount = reverb_line_total();

// Нулевое состояние (Reverb{}) - тишина в линиях.
struct Reverb {
    int16_t lines[kReverbSampleCount] = {};
    uint16_t comb_pos[kReverbCombCount] = {};
    uint16_t allpass_pos[kReverbAllpassCount] = {};
    // Последний выданный возврат - для линейной интерполяции обратно на
    // выходную частоту. Живёт между блоками, иначе на границе блока ступенька.
    int32_t prev_l = 0;
    int32_t prev_r = 0;
};

// Обрабатывает блок: bus - моно вход (масштаб int16, как у mix_l/mix_r),
// результат прибавляется к mix_l/mix_r. Вход не портится.
//
// Общая громкость возврата - SOUNDSINTH_REVERB_WET_Q15; посыл задаёт каждый
// голос при накоплении шины.
void reverb_process(Reverb& rv, const int32_t* bus, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames);

} // namespace soundsinth::engine
