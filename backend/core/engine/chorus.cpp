// SPDX-License-Identifier: MIT
#include "core/engine/chorus.h"

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "core/config.h"
#include "core/audio/sound_source.h"
#include "core/audio/saturate.h"

namespace soundsinth::engine {
namespace {

constexpr uint32_t kLineMask = kChorusLineLen - 1u;
static_assert((kChorusLineLen & kLineMask) == 0, "the chorus line is a power of two");

// Задержка отвода: центр 11.5 мс, размах +-5 мс (6.5..16.5 мс), в
// отсчётах 44100 Гц. Частота LFO 0.5 Гц: шаг фазы 2^32 * 0.5 / 44100.
constexpr int32_t kCenterSamples = 507;
constexpr int32_t kDepthSamples  = 220;
constexpr uint32_t kLfoStep      = 48696;
static_assert(kCenterSamples + kDepthSamples + 2 < static_cast<int32_t>(kChorusLineLen), "the tap runs past the line");
static_assert(kCenterSamples - kDepthSamples > 0, "the tap reaches the write position");

// Вход в линию - вдвое тише, на выходе обратно: сумма посылов многих
// голосов выходит за int16, а насыщение в линии слышно как хрип.
constexpr int32_t kInputShift = 1;
static_assert(static_cast<int64_t>(chorus_wet_for_level(127)) * 32767 <= INT32_MAX, "the return product leaves int32");

// Отклонение задержки от центра в Q16 отсчётов при фазе LFO phase:
// треугольник -32767..32767 (Q15) на размах. На полкруга дальше треугольник
// тот же с обратным знаком - правый отвод берёт его со знаком минус.
SOUNDSINTH_ALWAYS_INLINE int32_t tap_swing_q16(uint32_t phase) {
    const int32_t t   = static_cast<int32_t>(phase >> 16);
    const int32_t tri = (t < 32768 ? t : 65535 - t) * 2 - 32767;
    return (kDepthSamples << 1) * tri;
}

// Отсчёт линии на задержке d_q16 от последнего записанного, линейной
// интерполяцией между соседними.
SOUNDSINTH_ALWAYS_INLINE int32_t tap_read(const Chorus& ch, uint32_t newest, int32_t d_q16) {
    const uint32_t di  = static_cast<uint32_t>(d_q16 >> 16);
    const int32_t frac = d_q16 & 0xffff;
    const int32_t a    = ch.line[(newest - di) & kLineMask];
    const int32_t b    = ch.line[(newest - di - 1u) & kLineMask];
    return a + (((b - a) * frac) >> 16);
}

} // namespace

// Раз на блок рендера, в SRAM: из флеша на плате стоит 6..14% времени
// рендера - кэш XIP общий с PSRAM, и сэмплы голосов вытесняют код цикла.
void SOUNDSINTH_HOT_PATH(chorus_process)(Chorus& ch, const int32_t* bus, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames) {
    constexpr int32_t kOne = 1 << mixbus::kMixFracBits;
    uint32_t pos           = ch.pos;
    uint32_t phase         = ch.lfo_phase;
    const int32_t wet      = ch.wet_q15;
    for (uint32_t i = 0; i < n_frames; ++i) {
        ch.line[pos & kLineMask] = static_cast<int16_t>(sat_s16(bus[i] >> kInputShift));
        // Правый отвод - на полкруга LFO дальше: задержки расходятся, это и
        // даёт ширину.
        constexpr int32_t kCenterQ16 = kCenterSamples << 16;
        const int32_t swing          = tap_swing_q16(phase);
        const int32_t l              = tap_read(ch, pos, kCenterQ16 + swing);
        const int32_t r              = tap_read(ch, pos, kCenterQ16 - swing);
        // (отвод << kInputShift) * wet / 32768 без сдвига: отвод не больше
        // 32767, уровень не больше 32512 - произведение в int32.
        mix_l[i] += (l * wet) / (32768 >> kInputShift) * kOne;
        mix_r[i] += (r * wet) / (32768 >> kInputShift) * kOne;
        ++pos;
        phase += kLfoStep;
    }
    ch.pos       = pos & kLineMask;
    ch.lfo_phase = phase;
}

} // namespace soundsinth::engine
