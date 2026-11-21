// SPDX-License-Identifier: MIT
// Ядро SN76489: тишина после сброса Sega, частота тона, средний уровень
// ультразвукового тона, шум звучит и независимость выхода от нарезки батчей.

#include "testing.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "core/chips/sn_chip.h"

namespace {

using soundsinth::chips::SnChip;
using soundsinth::chips::SnVariant;

constexpr uint32_t kRate  = 44100;
constexpr uint32_t kClock = 3579545;
// PSG Sega без обратной полярности: уровни положительные.
const SnVariant kSega{16, 0x0009, false, false, true, false};

struct Write {
    uint64_t units;
    uint8_t data;
};

struct Render {
    std::vector<int16_t> ch[SnChip::kChannels];
};

Render render(SnChip& sn, const std::vector<Write>& writes, uint32_t samples, uint32_t batch) {
    Render r;
    size_t w = 0;
    SnChip::Row rows[SnChip::kChannels];
    for (uint32_t done = 0; done < samples;) {
        const uint32_t n = samples - done < batch ? samples - done : batch;
        sn.begin(n, rows);
        while (w < writes.size() && writes[w].units <= sn.last_units()) {
            sn.run_to(writes[w].units);
            sn.write(writes[w].data);
            ++w;
        }
        int16_t buf[SnChip::kChannels][SnChip::kMaxBatch];
        int16_t* const out[SnChip::kChannels] = {buf[0], buf[1], buf[2], buf[3]};
        sn.end(out);
        for (uint32_t c = 0; c < SnChip::kChannels; ++c)
            r.ch[c].insert(r.ch[c].end(), buf[c], buf[c] + n);
        done += n;
    }
    return r;
}

void sega_reset_is_silent() {
    SnChip sn;
    sn.init(kClock, kSega, kRate);
    const Render r = render(sn, {}, 4410, 64);
    uint32_t off   = 0;
    for (uint32_t c = 0; c < SnChip::kChannels; ++c)
        for (int16_t v : r.ch[c])
            if (v != 0) ++off;
    CHECK_EQ(off, 0u);
}

// Период 254: 3579545 / 32 / 254 = 440.4 Гц.
void tone_has_the_datasheet_pitch() {
    SnChip sn;
    sn.init(kClock, kSega, kRate);
    const Render r = render(sn, {{0, 0x8e}, {0, 0x0f}, {0, 0x90}}, kRate + 100, 64);
    uint32_t up    = 0;
    for (uint32_t i = 101; i < kRate + 100; ++i)
        if (r.ch[0][i - 1] < 8192 && r.ch[0][i] >= 8192) ++up;
    std::printf("  tone period 254: %u rising crossings in 1 s (want 440)\n", up);
    CHECK(up >= 439 && up <= 441);
}

// Период 1 - тон 112 кГц: звучит средним, половиной уровня.
void ultrasonic_tone_is_its_mean() {
    SnChip sn;
    sn.init(kClock, kSega, kRate);
    const Render r = render(sn, {{0, 0x81}, {0, 0x00}, {0, 0x90}}, 400, 64);
    CHECK_EQ(static_cast<int>(r.ch[0].back()), 8192);
}

// Белый шум на полной громкости звучит: уровень меняется.
void noise_sounds() {
    SnChip sn;
    sn.init(kClock, kSega, kRate);
    const Render r   = render(sn, {{0, 0xe4}, {0, 0xf0}}, 4410, 64);
    uint32_t changes = 0;
    for (uint32_t i = 1; i < r.ch[3].size(); ++i)
        if (r.ch[3][i] != r.ch[3][i - 1]) ++changes;
    CHECK(changes > 1000);
}

void batches_do_not_change_the_output() {
    std::vector<Write> w = {{0, 0x90}, {0, 0xb4}, {0, 0xd8}, {0, 0xf2}, {0, 0xe5}};
    uint32_t seed        = 7;
    uint64_t t           = 0;
    for (int i = 0; i < 400; ++i) {
        seed  = seed * 1103515245u + 12345u;
        t    += 1 + (seed >> 16) % 500;
        w.push_back({t, static_cast<uint8_t>(seed >> 8)});
    }
    SnChip a, b;
    a.init(kClock, kSega, kRate);
    b.init(kClock, kSega, kRate);
    const uint32_t samples = 20000;
    const Render ra        = render(a, w, samples, 64);
    const Render rb        = render(b, w, samples, 17);
    uint32_t diff          = 0;
    for (uint32_t c = 0; c < SnChip::kChannels; ++c)
        for (uint32_t i = 0; i < samples; ++i)
            if (ra.ch[c][i] != rb.ch[c][i]) ++diff;
    CHECK_EQ(diff, 0u);
}

// Шум с нулевой громкостью идёт по регистру сдвига дальше: включённый позже,
// он звучит так же, как у чипа, где был слышен с начала.
void silent_noise_keeps_its_place() {
    SnChip a, b;
    a.init(kClock, kSega, kRate);
    b.init(kClock, kSega, kRate);
    const uint64_t later = kClock / 16 / 10; // 0.1 с в тактах чипа
    const Render ra      = render(a, {{0, 0xf0}}, 8820, 64);
    const Render rb      = render(b, {{later, 0xf0}}, 8820, 64);
    uint32_t diff = 0, moved = 0;
    for (uint32_t i = 4410 + 64; i < 8820; ++i) {
        if (ra.ch[3][i] != rb.ch[3][i]) ++diff;
        if (ra.ch[3][i] != ra.ch[3][i - 1]) ++moved;
    }
    CHECK_EQ(diff, 0u);
    CHECK(moved > 100);
    CHECK_EQ(a.lfsr(), b.lfsr());
}

// Белый шум быстрее выхода (период тона 3 - ноль, шум идёт от него) - среднее
// по отсчёту: уровень в среднем половинный, размах - как у пяти шагов на
// отсчёт, от нарезки батчей выход не зависит.
void fast_noise_is_averaged() {
    const std::vector<Write> w = {{0, 0xe7}, {0, 0xf0}, {30000, 0xf4}, {60000, 0xe4}, {90000, 0xe7}, {120000, 0xf0}};
    SnChip a, b;
    a.init(kClock, kSega, kRate);
    b.init(kClock, kSega, kRate);
    const uint32_t samples = 44100;
    const Render ra        = render(a, w, samples, 64);
    const Render rb        = render(b, w, samples, 17);
    uint32_t diff          = 0;
    for (uint32_t i = 0; i < samples; ++i)
        if (ra.ch[3][i] != rb.ch[3][i]) ++diff;
    CHECK_EQ(diff, 0u);
    // Первые 30000 тактов (0.134 с) - полная громкость: канал даёт 16384.
    double sum = 0, sq = 0;
    const uint32_t from = 64, to = 5800;
    for (uint32_t i = from; i < to; ++i) {
        sum += ra.ch[3][i];
        sq  += static_cast<double>(ra.ch[3][i]) * ra.ch[3][i];
    }
    const double mean = sum / (to - from), sd = std::sqrt(sq / (to - from) - mean * mean);
    CHECK(mean > 7800 && mean < 8600);
    CHECK(sd > 3000 && sd < 4300);
}

} // namespace

void run_sn_chip_tests() {
    std::printf("chips: SN76489 core\n");
    sega_reset_is_silent();
    tone_has_the_datasheet_pitch();
    ultrasonic_tone_is_its_mean();
    noise_sounds();
    batches_do_not_change_the_output();
    silent_noise_keeps_its_place();
    fast_noise_is_averaged();
}
