// SPDX-License-Identifier: MIT
#pragma once

// Ядро Philips SAA1099: 6 тонов, 2 шума, 2 огибающие, громкость слева и
// справа у каждого канала. Поведение - по SAASound (Dave Hooper, Simon Owen;
// BSD-3), сверенной с чипом:
// - октава и частота тона защёлкиваются и вступают на полупериоде, одна
//   частота без смены октавы - через полупериод;
// - шум - 18-разрядный регистр Галуа x^18 + x^11 + x; такт - вход / 256,
//   512, 1024 или полупериоды тона 0 (шум 0) и 3 (шум 1);
// - огибающая 0 правит канал 2, огибающая 1 - канал 5; шаг - на
//   полупериоде тона 1 (4) или на записи адреса 24 (25); новая форма -
//   с конца периода прежней;
// - громкость с огибающей - по таблице (разряды 3-1 громкости x уровень).
//
// Сетка тактов - вход / 2: полупериод тона (511 - частота) << (7 - октава),
// период шума 128 << режим, все целые. Перепады - ступеньками (blep.h), как
// у AyChip. Неслышный тон, который ничего не тактует, и шум, который никто
// не слышит, событий не ставят - счёт догоняется на записи.
//
// Батч: begin(n), run_to(такт) и write_address()/write_data() по порядку,
// end(out) - 12 выходов: канал c слева - 2c, справа - 2c + 1; тишина 0,
// полный уровень около 16384.

#include <cstdint>

#include "core/chips/blep.h"

namespace soundsinth::chips {

class SaaChip {
public:
    static constexpr uint32_t kChannels = 6;
    static constexpr uint32_t kOutputs  = 2 * kChannels;
    static constexpr uint32_t kMaxBatch = 64;

    void init(uint32_t clock_hz, uint32_t rate_hz);
    void reset();

    // Строки разностей выхода на время батча, kOutputs штук (blep.h).
    using Row = BlepOut<kOutputs, kMaxBatch>::Row;
    void begin(uint32_t n, Row* rows);
    void run_to(uint64_t units);
    void write_address(uint8_t reg);
    void write_data(uint8_t value);
    bool end(int16_t* const out[kOutputs]);

    uint64_t last_units() const { return out_.last(); }
    uint64_t units_at(uint32_t sample) const { return out_.first(sample); }
    uint64_t now() const { return now_; }
    // Состояние для сверки.
    uint8_t tone_level(uint32_t c) const { return osc_[c].level; }
    uint32_t noise_state(uint32_t g) const { return noise_[g].rand; }
    uint8_t env_level(uint32_t g, uint32_t side) const { return env_[g].level[side]; }
    // Уровень канала c (side 0 - лево, 1 - право) в единицах чипа: 0..480.
    int32_t output(uint32_t c, uint32_t side) const { return level(c, side) / kScale; }
    // Каналов с амплитудой и тоном или шумом, для телеметрии.
    uint32_t heard() const {
        uint32_t n = 0;
        for (uint32_t c = 0; c < kChannels; ++c) {
            n += enabled_ && amp_[c] != 0 && mix_[c] != 0 ? 1u : 0u;
        }
        return n;
    }
    void settle();

private:
    static constexpr uint64_t kNever = ~0ull;
    // Полный уровень канала - 480 (громкость 15 x 2 x 16) - в Q15.
    static constexpr int32_t kScale = 68;

    struct Osc {
        uint8_t off, oct, next_off, next_oct;
        bool pending, ignore_off; // новые октава и частота ждут полупериода
        uint8_t level;
        bool on; // события ставятся
        uint32_t period;
        uint64_t next;
    };
    struct Noise {
        uint8_t mode;
        bool on;
        uint32_t rand;
        uint32_t period;
        uint32_t held; // режим 3: доли до сдвига, оставшиеся от прежнего режима
        uint64_t next;
    };
    struct Env {
        bool enabled, ended, looping, invert, external, pending;
        uint8_t form, phase, pos, phases, res, next_data;
        uint8_t level[2];
    };

    void step_to(uint64_t t);
    void emit(uint64_t t);
    void toggle(uint32_t c, uint64_t t);
    void catch_up();
    void update_activity();
    void set_period(Osc& o) const { o.period = static_cast<uint32_t>(511u - o.off) << (7u - o.oct); }
    void noise_shift(Noise& n);
    void env_control(Env& e, uint8_t data);
    void env_tick(Env& e);
    void env_load(Env& e, uint8_t data);
    void env_levels(Env& e);
    int32_t level(uint32_t c, uint32_t side) const;

    uint64_t now_           = 0;
    uint8_t reg_            = 0;
    uint8_t amp_[kChannels] = {};
    uint8_t mix_[kChannels] = {}; // разряд 0 - тон, 1 - шум
    bool enabled_           = false;
    bool sync_              = false;
    bool reload_            = false; // огибающая взяла новую форму: кто тактует - заново
    Osc osc_[kChannels]     = {};
    Noise noise_[2]         = {};
    Env env_[2]             = {};

    BlepOut<kOutputs, kMaxBatch> out_;
};

} // namespace soundsinth::chips
