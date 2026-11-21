// SPDX-License-Identifier: MIT
#pragma once

// Ядро SN76489 и родни (SN76489A, SN76496, PSG Sega Master System / Game
// Gear / Mega Drive, NCR8496). Три тона и шум; такт чипа - вход / 16 (у
// SN76494 и SN94624 - вход / 2). Поведение - по MAME sn76496 (BSD-3):
// счётчик тона убывает с периода, у перепада перезаряжается новым периодом;
// шум - регистр сдвига с отводами из настроек, выход однополярный.
//
// Счёт - событиями на сетке тактов, перепад уровня - ступенькой (blep.h) в
// дробную позицию выходного отсчёта, как у ядра AY. Тон выше половины
// выходной частоты звучит средним уровнем. Неслышный тон событий не ставит,
// его счёт догоняется разом на записи. Шум с нулевой громкостью шагает по
// регистру сдвига без ступенек на выходе, не реже раза в батч. Белый шум
// быстрее выхода (больше шага на отсчёт - тарелка у Mega Drive) идёт средним
// по отсчёту: мощность в полосе та же, что у ступенек, а ступеньки не нужны.
//
// Батч - как у AyChip: begin(n), run_to(такт) и write() по порядку записей,
// end(out) - отсчёты каналов 0-2 (тоны) и 3 (шум), тишина 0, полный уровень
// 16384. Стерео Game Gear - маска для громкости голосов снаружи.

#include <cstdint>

#include "core/chips/blep.h"

namespace soundsinth::chips {

// Разновидность чипа; у VGM - из заголовка (0x28 отводы, 0x2A длина
// регистра, 0x2B признаки).
struct SnVariant {
    uint8_t width  = 16;     // длина регистра шума
    uint16_t taps  = 0x0009; // отводы белого шума
    bool freq0_max = false;  // период 0 - как 0x400 (TI), иначе 0 (Sega)
    bool negate    = false;  // выход в обратной полярности
    bool divide8   = true;   // такт чипа - вход / 16, иначе / 2
    bool ncr       = false;  // NCR8496: шум сбрасывается только сменой режима
};

class SnChip {
public:
    static constexpr uint32_t kChannels = 4;
    static constexpr uint32_t kMaxBatch = 64;

    void init(uint32_t clock_hz, const SnVariant& variant, uint32_t rate_hz);
    void reset();

    // Строки разностей выхода на время батча, kChannels штук (blep.h).
    using Row = BlepOut<kChannels, kMaxBatch>::Row;
    void begin(uint32_t n, Row* rows);
    void run_to(uint64_t units);
    // Запись байта в порт чипа (защёлка регистра или данные).
    void write(uint8_t data);
    // Стерео Game Gear: разряды 4-7 - каналы 0-3 слева, 0-3 - справа.
    void write_stereo(uint8_t mask) { stereo_ = mask; }
    bool end(int16_t* const out[kChannels]);

    uint64_t last_units() const { return out_.last(); }
    uint64_t units_at(uint32_t sample) const { return out_.first(sample); }
    uint64_t now() const { return now_; }
    uint8_t stereo() const { return stereo_; }
    uint16_t reg(uint32_t r) const { return regs_[r & 7u]; }
    // Каналов с ослаблением меньше полного, для телеметрии.
    uint32_t heard() const {
        uint32_t n = 0;
        for (uint32_t c = 0; c < kChannels; ++c) {
            n += (regs_[2u * c + 1u] & 0x0fu) != 0x0fu ? 1u : 0u;
        }
        return n;
    }
    uint32_t lfsr() const { return lfsr_; }
    uint8_t output(uint32_t c) const { return bit_[c]; }
    void settle();

private:
    static constexpr uint64_t kNever = ~0ull;

    void step_to(uint64_t t);
    void emit(uint64_t t);
    void set_period(uint32_t c);
    void noise_step();
    void catch_up();
    void flat_to(uint64_t t);
    void flat_put(uint64_t t, uint32_t idx);
    void update_activity();
    int32_t level(uint32_t c) const;

    SnVariant v_{};
    uint32_t tap1_ = 0, tap2_ = 0, feedback_ = 0;

    uint64_t now_ = 0;

    uint16_t regs_[8] = {};
    uint8_t latch_    = 0;
    uint8_t stereo_   = 0;

    // Каналы 0-2 - тоны, 3 - шум: период в тактах, следующее событие.
    uint32_t period_[kChannels] = {};
    uint64_t next_[kChannels]   = {};
    uint8_t bit_[kChannels]     = {};
    bool avg_[3]                = {};
    bool on_[kChannels]         = {}; // события канала ставятся: он слышен
    uint32_t lfsr_              = 0;

    // Белый шум средним по отсчёту: кусок с такта start_, шум учтён до такта
    // from_, площадь единиц area_; edge_ - отсчёт батча, на границе которого
    // кладётся очередное среднее.
    bool flat_      = false;
    uint32_t edge_  = 0;
    uint32_t batch_ = 0;
    uint64_t start_ = 0;
    uint64_t from_  = 0;
    uint64_t area_  = 0;

    BlepOut<kChannels, kMaxBatch> out_;
};

} // namespace soundsinth::chips
