// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "core/engine/engine_defs.h"
#include "core/engine/resonant_filter.h"
#include "core/engine/reverb.h"

// Ревербератор и резонансный фильтр отдельно от движка: затухание до нуля
// после конца сигнала и независимость ревербератора от разбиения на блоки.

namespace {

using namespace soundsinth;

// Синус 440 Гц полной шкалы 1 с, затем тишина: вход шины ревербератора.
std::vector<int32_t> reverb_input(uint32_t tone_frames, uint32_t total_frames) {
    std::vector<int32_t> in(total_frames, 0);
    for (uint32_t i = 0; i < tone_frames; ++i) {
        in[i] = static_cast<int32_t>(32767.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / engine::kSampleRateHz));
    }
    return in;
}

// Возврат ревербератора на весь вход блоками по block кадров.
void run_reverb(const std::vector<int32_t>& in, uint32_t block, std::vector<int32_t>& out_l, std::vector<int32_t>& out_r, engine::Reverb& rv) {
    const uint32_t n = static_cast<uint32_t>(in.size());
    out_l.assign(n, 0);
    out_r.assign(n, 0);
    for (uint32_t i = 0; i < n; i += block) {
        const uint32_t k = n - i < block ? n - i : block;
        engine::reverb_process(rv, in.data() + i, out_l.data() + i, out_r.data() + i, k);
    }
}

// Хвост гаснет до точного нуля: округление Q15 не держит постоянную
// составляющую в линиях.
void test_reverb_decays_to_zero() {
    std::printf("test_reverb_decays_to_zero\n");
    const uint32_t tone           = engine::kSampleRateHz;
    const std::vector<int32_t> in = reverb_input(tone, 11 * engine::kSampleRateHz);
    auto rv                       = std::make_unique<engine::Reverb>();
    std::vector<int32_t> l, r;
    run_reverb(in, 256, l, r, *rv);
    uint32_t last_nonzero = 0;
    for (uint32_t i = 0; i < l.size(); ++i) {
        if (l[i] != 0 || r[i] != 0) last_nonzero = i;
    }
    std::printf("  last non-zero return at %.3f s\n", static_cast<double>(last_nonzero) / engine::kSampleRateHz);
    CHECK(last_nonzero > tone);
    CHECK(last_nonzero < 5 * engine::kSampleRateHz);
    uint32_t nonzero_lines = 0;
    for (const int16_t v : rv->lines)
        nonzero_lines += v != 0;
    CHECK_EQ(nonzero_lines, 0u);
}

// Блоки чётной длины (кратные kReverbRateDiv) дают один и тот же выход.
void test_reverb_independent_of_even_blocks() {
    std::printf("test_reverb_independent_of_even_blocks\n");
    const std::vector<int32_t> in = reverb_input(engine::kSampleRateHz / 2, 3 * engine::kSampleRateHz);
    std::vector<int32_t> ref_l, ref_r;
    {
        auto rv = std::make_unique<engine::Reverb>();
        run_reverb(in, 256, ref_l, ref_r, *rv);
    }
    for (const uint32_t block : {2u, 512u}) {
        auto rv = std::make_unique<engine::Reverb>();
        std::vector<int32_t> l, r;
        run_reverb(in, block, l, r, *rv);
        CHECK(l == ref_l);
        CHECK(r == ref_r);
    }
}

// После импульса полной шкалы и 2 с нулей выход фильтра не больше 5 LSB:
// неподвижная точка округления есть, но граница у неё малая.
void test_filter_tail_after_impulse() {
    std::printf("test_filter_tail_after_impulse\n");
    constexpr uint32_t kBlock  = 256;
    const uint32_t tail_frames = 2 * engine::kSampleRateHz;
    int max_tail               = 0;
    uint32_t cases             = 0;
    for (const bool sf2 : {false, true}) {
        for (uint8_t co = 0; co < 128; co = static_cast<uint8_t>(co + 14)) {
            for (const uint8_t res : {0, 32, 64, 96, 127}) {
                const engine::FilterCoeffs c = engine::filter_compute(co, res, 256, sf2 ? 16 : 24, sf2);
                if (!c.active) continue;
                ++cases;
                engine::FilterState s;
                int16_t buf[kBlock] = {};
                buf[0]              = 32767;
                int tail            = 0;
                for (uint32_t done = 0; done < tail_frames; done += kBlock) {
                    engine::filter_apply(buf, kBlock, c, s);
                    if (done + kBlock >= tail_frames - kBlock) {
                        for (const int16_t v : buf)
                            tail = std::abs(v) > tail ? std::abs(v) : tail;
                    }
                    for (int16_t& v : buf)
                        v = 0;
                }
                max_tail = tail > max_tail ? tail : max_tail;
            }
        }
    }
    std::printf("  combinations %u, largest tail %d LSB\n", cases, max_tail);
    CHECK(cases > 0u);
    CHECK(max_tail <= 5);
}

} // namespace

void run_reverb_filter_tests() {
    test_reverb_decays_to_zero();
    test_reverb_independent_of_even_blocks();
    test_filter_tail_after_impulse();
}
