// SPDX-License-Identifier: MIT
#include "testing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "core/engine/chorus.h"
#include "core/engine/engine_defs.h"
#include "core/engine/resonant_filter.h"
#include "core/engine/reverb.h"
#include "core/formats/midi_sysex.h"

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
    // Обратная связь 0.95 и уровень +12 дБ: точный ноль около 5 с.
    CHECK(last_nonzero < 8 * engine::kSampleRateHz);
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

// Хорус: тишина остаётся тишиной, импульс возвращается не раньше 6.5 мс и
// не позже 16.5 мс, полная шкала на входе не переполняет выход.
void test_chorus_delay_and_range() {
    std::printf("test_chorus_delay_and_range\n");
    constexpr uint32_t n = 2048;
    std::vector<int32_t> in(n, 0), l(n, 0), r(n, 0);
    auto ch = std::make_unique<engine::Chorus>();
    engine::chorus_process(*ch, in.data(), l.data(), r.data(), n);
    bool silent = true;
    for (uint32_t i = 0; i < n; ++i)
        silent = silent && l[i] == 0 && r[i] == 0;
    CHECK(silent);

    in[0] = 32767;
    engine::chorus_process(*ch, in.data(), l.data(), r.data(), n);
    uint32_t first = n, last = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (l[i] != 0 || r[i] != 0) {
            if (first == n) first = i;
            last = i;
        }
    }
    std::printf("  impulse returns at %u..%u samples\n", first, last);
    CHECK(first >= 6 * engine::kSampleRateHz / 1000);
    CHECK(last <= 17 * engine::kSampleRateHz / 1000);

    std::fill(in.begin(), in.end(), 32767 * 8);
    std::fill(l.begin(), l.end(), 0);
    engine::chorus_process(*ch, in.data(), l.data(), r.data(), n);
    int32_t peak = 0;
    for (uint32_t i = 0; i < n; ++i)
        peak = std::max(peak, std::abs(l[i]));
    std::printf("  full scale peak %d\n", peak);
    CHECK(peak > 0);
    CHECK(peak <= 32768 * 256);
}

// Параметры эффектов из SysEx GS и XG и их перевод в уровни и обратную связь.
void test_sysex_effect_params() {
    std::printf("test_sysex_effect_params\n");
    using soundsinth::formats::midi::sysex_effect_param;
    uint8_t p = 0xff, v = 0xff;
    const uint8_t gs_level[] = {0x41, 0x10, 0x42, 0x12, 0x40, 0x01, 0x33, 119, 0x00};
    CHECK(sysex_effect_param(gs_level, sizeof gs_level, p, v));
    CHECK_EQ(p, 0u);
    CHECK_EQ(v, 119u);
    const uint8_t gs_macro_hall2[] = {0x41, 0x10, 0x42, 0x12, 0x40, 0x01, 0x30, 4, 0x00};
    CHECK(sysex_effect_param(gs_macro_hall2, sizeof gs_macro_hall2, p, v));
    CHECK_EQ(p, 1u);
    CHECK_EQ(v, 72u);
    const uint8_t gs_chorus[] = {0x41, 0x10, 0x42, 0x12, 0x40, 0x01, 0x3A, 127, 0x00};
    CHECK(sysex_effect_param(gs_chorus, sizeof gs_chorus, p, v));
    CHECK_EQ(p, 2u);
    CHECK_EQ(v, 127u);
    const uint8_t xg_tunnel[] = {0x43, 0x11, 0x4c, 0x02, 0x01, 0x00, 0x11, 0x00};
    CHECK(sysex_effect_param(xg_tunnel, sizeof xg_tunnel, p, v));
    CHECK_EQ(p, 1u);
    CHECK_EQ(v, 96u);
    const uint8_t xg_reverb_off[] = {0x43, 0x10, 0x4c, 0x02, 0x01, 0x00, 0x00, 0x00};
    CHECK(sysex_effect_param(xg_reverb_off, sizeof xg_reverb_off, p, v));
    CHECK_EQ(p, 0u);
    CHECK_EQ(v, 0u);
    const uint8_t xg_chorus_on[] = {0x43, 0x10, 0x4c, 0x02, 0x01, 0x20, 0x42, 0x00};
    CHECK(!sysex_effect_param(xg_chorus_on, sizeof xg_chorus_on, p, v));
    const uint8_t gs_drum_part[] = {0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x15, 0x01, 0x00};
    CHECK(!sysex_effect_param(gs_drum_part, sizeof gs_drum_part, p, v));

    CHECK_EQ(engine::reverb_wet_for_level(0), 0);
    CHECK(engine::reverb_wet_for_level(127) > engine::reverb_wet_for_level(64));
    CHECK(engine::reverb_feedback_for_time(0) < engine::kReverbFeedbackQ15);
    CHECK(engine::reverb_feedback_for_time(127) <= 31457); // 0.96
    CHECK_EQ(engine::chorus_wet_for_level(0), 0);
}

} // namespace

void run_reverb_filter_tests() {
    test_sysex_effect_params();
    test_reverb_decays_to_zero();
    test_reverb_independent_of_even_blocks();
    test_filter_tail_after_impulse();
    test_chorus_delay_and_range();
}
