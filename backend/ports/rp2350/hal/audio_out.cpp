// SPDX-License-Identifier: MIT
// Вывод звуковой цепочки - синк I2S.
//
// Переходник без своего состояния: ReadyStats у синка и у контракта - один
// и тот же тип, копировать нечего.

#include "player/hal/audio_out.h"

#include "audio/i2s_sink.h"

void player::hal::audio_out_start(player::audio::BufferPool& pool, uint32_t sample_rate_hz) {
    rp2350::audio::i2s_sink_start(pool, sample_rate_hz);
}

void player::hal::audio_out_set_silent(bool silent) {
    rp2350::audio::i2s_sink_set_silent(silent);
}

uint32_t player::hal::audio_out_underruns() {
    return rp2350::audio::i2s_sink_underrun_count();
}

uint32_t player::hal::audio_out_underruns_bg() {
    return rp2350::audio::i2s_sink_underrun_bg_count();
}

uint32_t player::hal::audio_out_late() {
    return rp2350::audio::i2s_sink_late_count();
}

player::hal::ReadyStats player::hal::audio_out_ready_stats() {
    return rp2350::audio::i2s_sink_ready_stats();
}
