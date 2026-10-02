// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "core/audio/sound_source.h"

namespace pc_player {

// Тестовый источник - синус. Не часть soundsinth_core: диагностический
// генератор для проверки пайплайна audio_sink/mixbus/sdl_sink/wav_writer,
// появился раньше движка и остался как режим без файла.
class SineSource {
public:
    SineSource(float frequency_hz, uint32_t sample_rate_hz, float amplitude);

    soundsinth::mixbus::SoundSource* as_sound_source() { return &iface_; }

private:
    static void render_add(void* self, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames);

    soundsinth::mixbus::SoundSource iface_;
    float phase_ = 0.0f;
    float phase_increment_;
    float amplitude_;
};

} // namespace pc_player
