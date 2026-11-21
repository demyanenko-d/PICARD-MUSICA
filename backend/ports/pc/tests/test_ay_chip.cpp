// SPDX-License-Identifier: MIT
// Ядро AY/YM: тишина, точный возврат уровня после ступенек, частота тона,
// шаги огибающей по таблице ЦАП, средний уровень ультразвукового тона и
// независимость выхода от нарезки батчей.

#include "testing.h"

#include <cstdio>
#include <vector>

#include "core/chips/ay_chip.h"

namespace {

using soundsinth::chips::AyChip;

constexpr uint32_t kRate     = 44100;
constexpr uint32_t kPentagon = 1750000;

struct Write {
    uint64_t units;
    uint8_t reg;
    uint8_t value;
};

// Каналы подряд: out[c][i].
struct Render {
    std::vector<int16_t> ch[AyChip::kChannels];
};

Render render(AyChip& ay, const std::vector<Write>& writes, uint32_t samples, uint32_t batch) {
    Render r;
    size_t w = 0;
    AyChip::Row rows[AyChip::kChannels];
    for (uint32_t done = 0; done < samples;) {
        const uint32_t n = samples - done < batch ? samples - done : batch;
        ay.begin(n, rows);
        while (w < writes.size() && writes[w].units <= ay.last_units()) {
            ay.run_to(writes[w].units);
            ay.write(writes[w].reg, writes[w].value);
            ++w;
        }
        int16_t buf[AyChip::kChannels][AyChip::kMaxBatch];
        int16_t* const out[AyChip::kChannels] = {buf[0], buf[1], buf[2]};
        ay.end(out);
        for (uint32_t c = 0; c < AyChip::kChannels; ++c)
            r.ch[c].insert(r.ch[c].end(), buf[c], buf[c] + n);
        done += n;
    }
    return r;
}

void idle_chip_is_silent() {
    AyChip ay;
    ay.init(kPentagon, false, kRate);
    const Render r = render(ay, {}, 4410, 64);
    uint32_t off   = 0;
    for (uint32_t c = 0; c < AyChip::kChannels; ++c)
        for (int16_t v : r.ch[c])
            if (v != 0) ++off;
    CHECK_EQ(off, 0u);
}

// Громкость туда-обратно в случайные моменты: после последнего шага в ноль
// выход ровно на прежнем уровне - ступеньки суммируются без ухода.
void steps_return_exactly() {
    AyChip ay;
    ay.init(kPentagon, false, kRate);
    std::vector<Write> w = {{10, 7, 0x3f}};
    uint32_t seed        = 1;
    uint64_t t           = 20;
    for (int i = 0; i < 2000; ++i) {
        seed  = seed * 1103515245u + 12345u;
        t    += 1 + (seed >> 16) % 300;
        w.push_back({t, 8, static_cast<uint8_t>((seed >> 8) & 0x0f)});
    }
    w.push_back({t + 5, 8, 0});
    const uint32_t samples = static_cast<uint32_t>((t + 5) * kRate / (kPentagon / 8)) + 200;
    const Render r         = render(ay, w, samples, 64);
    CHECK_EQ(static_cast<int>(r.ch[0].back()), 0);
    // Громкость 15 без тона и шума держит полный уровень.
    AyChip full;
    full.init(kPentagon, false, kRate);
    const Render f = render(full, {{10, 7, 0x3f}, {10, 8, 15}}, 400, 64);
    CHECK_EQ(static_cast<int>(f.ch[0].back()), 16384);
}

// Тон TP = 100 на 1.75 МГц: 1093.75 Гц. Пересечения середины размаха вверх за
// секунду.
void tone_has_the_datasheet_pitch() {
    AyChip ay;
    ay.init(kPentagon, false, kRate);
    const Render r = render(ay, {{0, 0, 100}, {0, 7, 0x3e}, {0, 8, 15}}, kRate + 100, 64);
    uint32_t up    = 0;
    for (uint32_t i = 101; i < kRate + 100; ++i)
        if (r.ch[0][i - 1] < 8192 && r.ch[0][i] >= 8192) ++up;
    std::printf("  tone TP=100: %u rising crossings in 1 s (want 1093-1094)\n", up);
    CHECK(up >= 1093 && up <= 1094);
}

// Огибающая формы 13 (вверх и держать) с длинным шагом: посреди каждого шага
// выход ровно уровень ЦАП этого шага.
void envelope_walks_the_dac(bool ym, const uint16_t* want_q15) {
    AyChip ay;
    ay.init(kPentagon, ym, kRate);
    constexpr uint32_t kEp = 2000; // такт clock/8; шаг ~403 отсчёта
    const Render r = render(ay, {{0, 7, 0x3f}, {0, 8, 0x10}, {0, 11, kEp & 0xff}, {0, 12, kEp >> 8}, {100, 13, 0x0d}}, 34u * kEp * kRate / (kPentagon / 8), 64);
    uint32_t bad   = 0;
    for (uint32_t k = 0; k < 32; ++k) {
        const uint64_t mid = 100 + static_cast<uint64_t>(k) * kEp + kEp / 2;
        const uint32_t s   = static_cast<uint32_t>(mid * kRate / (kPentagon / 8));
        if (r.ch[0][s] != static_cast<int16_t>(want_q15[k] >> 1)) ++bad;
    }
    std::printf("  envelope %s: %u of 32 steps off the DAC table\n", ym ? "YM" : "AY", bad);
    CHECK_EQ(bad, 0u);
}

void envelope_steps() {
    static const uint16_t kAy[32] = {0,    0,    328,  328,  474,  474,  690,   690,   1006,  1006,  1493,  1493,  2114,  2114,  3518,  3518,
                                     4148, 4148, 6717, 6717, 9575, 9575, 12217, 12217, 16139, 16139, 20818, 20818, 26397, 26397, 32768, 32768};
    static const uint16_t kYm[32] = {0,    0,    153,  253,  359,  458,  557,  656,  799,   973,   1149,  1324,  1591,  1912,  2230,  2549,
                                     3032, 3640, 4252, 4866, 5789, 6932, 8074, 9211, 10936, 13121, 15315, 17512, 20813, 24838, 28833, 32768};
    envelope_walks_the_dac(false, kAy);
    envelope_walks_the_dac(true, kYm);
}

// Тон выше половины выходной частоты звучит средним: половина уровня.
void ultrasonic_tone_is_its_mean() {
    AyChip ay;
    ay.init(kPentagon, false, kRate);
    const Render r = render(ay, {{0, 0, 1}, {0, 7, 0x3e}, {0, 8, 15}}, 400, 64);
    CHECK_EQ(static_cast<int>(r.ch[0].back()), 8192);
}

// Выход не зависит от того, как время нарезано на батчи.
void batches_do_not_change_the_output() {
    std::vector<Write> w = {{0, 7, 0x30}, {0, 8, 15}, {0, 9, 0x10}, {0, 10, 12}, {0, 6, 3}, {0, 11, 40}, {0, 13, 0x0e}};
    uint32_t seed        = 7;
    uint64_t t           = 0;
    for (int i = 0; i < 400; ++i) {
        seed  = seed * 1103515245u + 12345u;
        t    += 1 + (seed >> 16) % 900;
        w.push_back({t, static_cast<uint8_t>((seed >> 8) % 6), static_cast<uint8_t>(seed >> 3)});
    }
    AyChip a, b;
    a.init(kPentagon, true, kRate);
    b.init(kPentagon, true, kRate);
    const uint32_t samples = 20000;
    const Render ra        = render(a, w, samples, 64);
    const Render rb        = render(b, w, samples, 17);
    uint32_t diff          = 0;
    for (uint32_t c = 0; c < AyChip::kChannels; ++c)
        for (uint32_t i = 0; i < samples; ++i)
            if (ra.ch[c][i] != rb.ch[c][i]) ++diff;
    CHECK_EQ(diff, 0u);
}

// Множитель периодов 2^2 на сетке вчетверо быстрее - тот же чип, что вход
// вчетверо ниже: тон, шум и огибающая совпадают отсчёт в отсчёт.
void period_shift_is_a_clock_division() {
    AyChip a, b;
    a.init(1996800, true, kRate);
    b.init(7987200, true, kRate);
    b.set_period_shift(2);
    const std::vector<Write> w = {{0, 0, 0x80},   {0, 2, 0x03},  {0, 6, 0x05},     {0, 11, 0x40},    {0, 13, 0x0e},    {0, 7, 0x18},
                                  {0, 8, 0x0f},   {0, 9, 0x0b},  {0, 10, 0x10},    {9000, 0, 0x31},  {20000, 6, 0x1f}, {31000, 11, 0x07},
                                  {31000, 13, 8}, {47000, 7, 0}, {52001, 4, 0x11}, {60000, 10, 0x0c}};
    std::vector<Write> w4      = w;
    for (Write& x : w4) {
        x.units *= 4;
    }
    const uint32_t samples = 16000;
    const Render ra        = render(a, w, samples, 64);
    const Render rb        = render(b, w4, samples, 64);
    uint32_t diff = 0, loud = 0;
    for (uint32_t c = 0; c < AyChip::kChannels; ++c) {
        for (uint32_t i = 0; i < samples; ++i) {
            if (ra.ch[c][i] != rb.ch[c][i]) ++diff;
            if (ra.ch[c][i] != 0) ++loud;
        }
    }
    CHECK(loud > samples);
    CHECK_EQ(diff, 0u);
}

} // namespace

void run_ay_chip_tests() {
    std::printf("chips: AY/YM core\n");
    idle_chip_is_silent();
    steps_return_exactly();
    tone_has_the_datasheet_pitch();
    envelope_steps();
    ultrasonic_tone_is_its_mean();
    batches_do_not_change_the_output();
    period_shift_is_a_clock_division();
}
