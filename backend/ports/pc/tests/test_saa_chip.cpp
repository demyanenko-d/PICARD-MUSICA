// SPDX-License-Identifier: MIT
// Ядро SAA1099: тишина без разрешения выхода, частота тона, громкость слева и
// справа, огибающая на канале 2 и независимость выхода от нарезки батчей.

#include "testing.h"

#include <cstdio>
#include <vector>

#include "core/chips/saa_chip.h"

namespace {

using soundsinth::chips::SaaChip;

constexpr uint32_t kRate  = 44100;
constexpr uint32_t kClock = 8000000;

struct Write {
    uint64_t units;
    uint8_t reg;
    uint8_t value;
};

struct Render {
    std::vector<int16_t> out[SaaChip::kOutputs];
};

Render render(SaaChip& saa, const std::vector<Write>& writes, uint32_t samples, uint32_t batch) {
    Render r;
    size_t w = 0;
    SaaChip::Row rows[SaaChip::kOutputs];
    for (uint32_t done = 0; done < samples;) {
        const uint32_t n = samples - done < batch ? samples - done : batch;
        saa.begin(n, rows);
        while (w < writes.size() && writes[w].units <= saa.last_units()) {
            saa.run_to(writes[w].units);
            saa.write_address(writes[w].reg);
            saa.write_data(writes[w].value);
            ++w;
        }
        int16_t buf[SaaChip::kOutputs][SaaChip::kMaxBatch];
        int16_t* out[SaaChip::kOutputs];
        for (uint32_t c = 0; c < SaaChip::kOutputs; ++c)
            out[c] = buf[c];
        saa.end(out);
        for (uint32_t c = 0; c < SaaChip::kOutputs; ++c)
            r.out[c].insert(r.out[c].end(), buf[c], buf[c] + n);
        done += n;
    }
    return r;
}

// Тон канала 0 слева 15, справа 0, октава 3, частота 255: 8 МГц / 512 * 8 /
// 256 = 488.28 Гц.
const std::vector<Write> kTone = {{0, 0, 0x0f}, {0, 16, 0x03}, {0, 8, 255}, {0, 20, 0x01}};

void disabled_chip_is_silent() {
    SaaChip saa;
    saa.init(kClock, kRate);
    const Render r = render(saa, kTone, 4410, 64);
    uint32_t off   = 0;
    for (uint32_t c = 0; c < SaaChip::kOutputs; ++c)
        for (int16_t v : r.out[c])
            if (v != 0) ++off;
    CHECK_EQ(off, 0u);
}

void tone_has_the_datasheet_pitch() {
    SaaChip saa;
    saa.init(kClock, kRate);
    // Под синхронизацией октава и частота вступают сразу, а не с полупериода.
    std::vector<Write> w = {{0, 28, 0x02}};
    w.insert(w.end(), kTone.begin(), kTone.end());
    w.push_back({0, 28, 0x01});
    const Render r = render(saa, w, kRate + 100, 64);
    uint32_t up    = 0;
    int16_t peak   = 0;
    for (uint32_t i = 101; i < kRate + 100; ++i) {
        if (r.out[0][i - 1] < 8160 && r.out[0][i] >= 8160) ++up;
        if (r.out[1][i] > peak) peak = r.out[1][i];
    }
    std::printf("  tone oct 3 freq 255: %u rising crossings in 1 s (want 488), right peak %d\n", up, peak);
    CHECK(up >= 487 && up <= 489);
    CHECK_EQ(static_cast<int>(peak), 0);
}

// Огибающая 0, форма 1 (максимум с повтором): канал 2 с громкостью 15 -
// постоянный уровень 53 x 4 x 2 из таблицы громкости с огибающей.
void envelope_holds_channel_2() {
    SaaChip saa;
    saa.init(kClock, kRate);
    const Render r = render(saa, {{0, 2, 0xff}, {0, 24, 0x82}, {0, 28, 0x01}}, 400, 64);
    CHECK_EQ(static_cast<int>(r.out[4].back()), 53 * 4 * 2 * 68 / 2);
    CHECK_EQ(static_cast<int>(r.out[5].back()), 53 * 4 * 2 * 68 / 2);
}

void batches_do_not_change_the_output() {
    std::vector<Write> w = {{0, 28, 0x01}, {0, 20, 0x3f}, {0, 21, 0x24}, {0, 22, 0x13}};
    uint32_t seed        = 7;
    uint64_t t           = 0;
    for (int i = 0; i < 400; ++i) {
        seed                  = seed * 1103515245u + 12345u;
        t                    += 1 + (seed >> 16) % 4000;
        const uint8_t regs[]  = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 24, 25};
        w.push_back({t, regs[(seed >> 8) % sizeof(regs)], static_cast<uint8_t>(seed >> 3)});
    }
    SaaChip a, b;
    a.init(kClock, kRate);
    b.init(kClock, kRate);
    const uint32_t samples = 20000;
    const Render ra        = render(a, w, samples, 64);
    const Render rb        = render(b, w, samples, 17);
    uint32_t diff          = 0;
    for (uint32_t c = 0; c < SaaChip::kOutputs; ++c)
        for (uint32_t i = 0; i < samples; ++i)
            if (ra.out[c][i] != rb.out[c][i]) ++diff;
    CHECK_EQ(diff, 0u);
}

} // namespace

void run_saa_chip_tests() {
    std::printf("chips: SAA1099 core\n");
    disabled_chip_is_silent();
    tone_has_the_datasheet_pitch();
    envelope_holds_channel_2();
    batches_do_not_change_the_output();
}
