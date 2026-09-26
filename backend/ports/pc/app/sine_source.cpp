#include "sine_source.h"

#include <cmath>

namespace pc_player {
namespace {
constexpr float kTwoPi = 6.28318530717958647692f;
}

SineSource::SineSource(float frequency_hz, uint32_t sample_rate_hz, float amplitude)
    : phase_increment_(kTwoPi * frequency_hz / static_cast<float>(sample_rate_hz))
    , amplitude_(amplitude) {
    iface_.self = this;
    iface_.render_add = &SineSource::render_add;
}

void SineSource::render_add(void* self_ptr, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames) {
    auto* self = static_cast<SineSource*>(self_ptr);
    for (uint32_t i = 0; i < n_frames; ++i) {
        // Шина в Q24.8 (soundsinth::mixbus::kMixFracBits).
        const int32_t sample = static_cast<int32_t>(std::sin(self->phase_) * self->amplitude_ * 256.0f);
        mix_l[i] += sample;
        mix_r[i] += sample;
        self->phase_ += self->phase_increment_;
        if (self->phase_ > kTwoPi) {
            self->phase_ -= kTwoPi;
        }
    }
}

} // namespace pc_player
