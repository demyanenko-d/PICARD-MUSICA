// SPDX-License-Identifier: MIT
// Голос стойки чипов (Stream): отсчёты своего канала стойки 1:1, через тот
// же тракт громкости, что у голоса сэмпла; разгон с тишины на запуске.

#include "testing.h"

#include <cstdio>

#include "core/engine/voice_mixer.h"
#include "core/memory/psram_store.h"

namespace {

using soundsinth::engine::VoiceMixer;

void stream_voice_plays_its_channel() {
    static VoiceMixer m;
    static soundsinth::memory::PsramStore psram;
    constexpr uint32_t kN = 64;
    static int16_t buf[3][kN];
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t i = 0; i < kN; ++i)
            buf[c][i] = static_cast<int16_t>((c + 1) * 100 + i);
    m.stream_base   = &buf[0][0];
    m.stream_stride = kN;
    m.ramp_samples  = 16;
    m.trigger_stream(5, 1);
    m.set_gains(5, 1 << soundsinth::engine::kGainQ24Bits, 1 << (soundsinth::engine::kGainQ24Bits - 1));
    m.ramp_to_gains(5);
    m.list_clear();
    m.list_push(5);
    int32_t l[kN] = {}, r[kN] = {};
    m.mix(psram, l, r, 0, kN, 0);
    // Разгон: первый отсчёт тише конечного уровня.
    CHECK(l[0] < buf[1][0] * 256);
    for (uint32_t i = 0; i < kN; ++i) {
        l[i] = 0;
        r[i] = 0;
    }
    m.mix(psram, l, r, 0, kN, 0);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < kN; ++i) {
        if (l[i] != buf[1][i] * 256 || r[i] != buf[1][i] * 128) ++bad;
    }
    std::printf("  stream voice: %u of %u samples off channel 1 at gains 1 and 1/2\n", bad, kN);
    CHECK_EQ(bad, 0u);
    // Цена - без прыжка по PSRAM.
    CHECK_EQ(m.list_cost_ns(), static_cast<uint64_t>(SOUNDSINTH_VOICE_COST_BASE_NS));
}

} // namespace

void run_stream_voice_tests() {
    std::printf("engine: stream voice of the chip rack\n");
    stream_voice_plays_its_channel();
}
