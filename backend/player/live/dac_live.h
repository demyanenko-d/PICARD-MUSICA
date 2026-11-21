// SPDX-License-Identifier: MIT
#pragma once

// Цифровой звук машины: Covox (#FB, в оба канала), Soundrive 1.05 режима 1
// (#0F и #1F слева, #4F и #5F справа) и бипер (#FE). Записи - из своего
// кольца отвода со штампом шины; уровень каждого ЦАП меняется ступенькой
// BLEP в своём отсчёте 44100 и фазе 1/32 отсчёта.
//
// Своё кольцо, а не общее: Soundrive у MOD-плееров пишет до 60 тысяч раз в
// секунду, бипер - до 35 тысяч; в общем кольце такой поток съел бы ёмкость
// живого MIDI.

#include <cstdint>

#include "core/audio/sound_source.h"
#include "player/live/live_tap.h"

namespace player::live {

// Источник записи в кольце: номер ЦАП.
enum class DacPort : uint8_t {
    Covox  = 0, // #FB
    SdA    = 1, // #0F, слева
    SdB    = 2, // #1F, слева
    SdC    = 3, // #4F, справа
    SdD    = 4, // #5F, справа
    Beeper = 5, // #FE, байт - разряды 4 (EAR) и 3 (MIC)
};

// 4096 записей - 68 мс при 60 тысячах в секунду: задержка звука живых
// чипов 50 мс плюс буфер рендера.
inline constexpr uint32_t kDacRing = 4096;
extern TapRing<kDacRing> g_dac_ring;

// Что разбирает обработчик шины: разряды kDacCovox, kDacSoundrive и маска
// разрядов порта #FE, которые слышны (0x10 - EAR, 0x18 - с MIC). Ноль -
// всё выключено, обработчику хватает одного чтения.
inline constexpr uint8_t kDacCovox     = 0x01;
inline constexpr uint8_t kDacSoundrive = 0x02;
inline constexpr uint8_t kDacBeeperEar = 0x10;
inline constexpr uint8_t kDacBeeperMic = 0x08;
extern volatile uint8_t g_dac_on;

// beeper - config::Beeper: 0 нет, 1 EAR, 2 с MIC.
void dac_live_configure(bool covox, bool soundrive, uint8_t beeper);
inline bool dac_live_enabled() {
    return g_dac_on != 0u;
}

// Источник микшера; пока всё выключено или тихо - молчит и не считает.
soundsinth::mixbus::SoundSource* dac_live_source();

// Строка журнала: принято, потеряно на полном кольце, опоздавших.
void dac_live_log_stats();

} // namespace player::live
