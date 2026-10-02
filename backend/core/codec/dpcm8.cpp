// SPDX-License-Identifier: MIT
#include "core/codec/dpcm8.h"

#include <iterator>

namespace soundsinth::dpcm8 {

uint8_t quantize_sample(int16_t sample, const Dpcm8State& state) {
    const int32_t residual = static_cast<int32_t>(sample) - static_cast<int32_t>(state.predictor);

    // Округление к ближайшему; den (kScaleTable[s]) всегда > 0.
    auto round_div = [](int32_t num, int32_t den) -> int32_t {
        if (num >= 0) return (num + den / 2) / den;
        return -((-num + den / 2) / den);
    };

    uint8_t best_scale = 0;
    int32_t best_value = 0;
    int32_t best_err   = -1;
    // Перебор 8 масштабов: в каждом округление к ближайшему кратному уже
    // оптимально (шаг регулярный), минимум по 8 масштабам равен минимуму
    // по всем 256 кодам.
    for (size_t s = 0; s < std::size(kScaleTable); ++s) {
        const int32_t v    = clamp_i32(round_div(residual, kScaleTable[s]), kCodeValueMin, kCodeValueMax);
        const int32_t diff = residual - v * kScaleTable[s];
        const int32_t err  = diff < 0 ? -diff : diff;
        if (best_err < 0 || err < best_err) {
            best_err   = err;
            best_scale = s;
            best_value = v;
        }
    }
    return static_cast<uint8_t>((best_scale << kCodeScaleLsb) | (static_cast<uint8_t>(best_value) & kCodeValueBits));
}

uint32_t encode_block(const int16_t* samples, uint32_t sample_count, uint32_t global_sample_offset, Dpcm8State& state, int8_t* dpcm_out,
                      Dpcm8Checkpoint* checkpoints_out, uint32_t checkpoints_capacity) {
    uint32_t checkpoint_count = 0;
    for (uint32_t i = 0; i < sample_count; ++i) {
        if ((global_sample_offset + i) % kCheckpointIntervalSamples == 0 && checkpoint_count < checkpoints_capacity) {
            checkpoints_out[checkpoint_count++] = Dpcm8Checkpoint{state.predictor};
        }

        const uint8_t code = quantize_sample(samples[i], state);
        decode_delta(code, state); // кодировщик повторяет путь декодера
        dpcm_out[i] = static_cast<int8_t>(code);
    }
    return checkpoint_count;
}

void decode_block(const int8_t* dpcm_in, uint32_t sample_count, Dpcm8State state, int16_t* samples_out) {
    for (uint32_t i = 0; i < sample_count; ++i) {
        samples_out[i] = decode_delta(static_cast<uint8_t>(dpcm_in[i]), state);
    }
}

} // namespace soundsinth::dpcm8
