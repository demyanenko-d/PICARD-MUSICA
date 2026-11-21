// SPDX-License-Identifier: MIT
#include "core/chips/ay_chip.h"

#include <cstring>

namespace soundsinth::chips {

namespace {

// Уровни ЦАП в Q15 (1.0 = 32768) по индексу 0..31: громкость регистра даёт
// vol * 2 + 1, огибающая - свой шаг. Числа - замеры ayumi (Peter Sovietov,
// MIT). У AY соседние пары равны: шагов у него 16.
constexpr uint16_t kDacAy[32] = {0,    0,    328,  328,  474,  474,  690,   690,   1006,  1006,  1493,  1493,  2114,  2114,  3518,  3518,
                                 4148, 4148, 6717, 6717, 9575, 9575, 12217, 12217, 16139, 16139, 20818, 20818, 26397, 26397, 32768, 32768};
constexpr uint16_t kDacYm[32] = {0,    0,    153,  253,  359,  458,  557,  656,  799,   973,   1149,  1324,  1591,  1912,  2230,  2549,
                                 3032, 3640, 4252, 4866, 5789, 6932, 8074, 9211, 10936, 13121, 15315, 17512, 20813, 24838, 28833, 32768};

// Значащие разряды регистров.
constexpr uint8_t kRegMask[AyChip::kRegs] = {0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff, 0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff};

// Огибающая: два участка на форму.
enum Seg : uint8_t { kDown, kUp, kHoldTop, kHoldBottom };
constexpr Seg kEnvSeg[16][2] = {
    {kDown, kHoldBottom}, {kDown, kHoldBottom}, {kDown, kHoldBottom}, {kDown, kHoldBottom}, {kUp, kHoldBottom}, {kUp, kHoldBottom},
    {kUp, kHoldBottom},   {kUp, kHoldBottom},   {kDown, kDown},       {kDown, kHoldBottom}, {kDown, kUp},       {kDown, kHoldTop},
    {kUp, kUp},           {kUp, kHoldTop},      {kUp, kDown},         {kUp, kHoldBottom},
};

bool is_hold(Seg s) {
    return s == kHoldTop || s == kHoldBottom;
}

// Шум - регистр сдвига на 17 разрядов, линейный: k шагов разом - матрица
// M^k. Столбцы степеней M^(2^i) считает компилятор, прыжок - не больше 17
// умножений на вектор. Период последовательности - 2^17 - 1.
constexpr uint32_t kLfsrBits   = 17;
constexpr uint32_t kLfsrPeriod = (1u << kLfsrBits) - 1u;

constexpr uint32_t lfsr_step(uint32_t s) {
    return (s >> 1) | (((s ^ (s >> 3)) & 1u) << 16);
}

constexpr uint32_t lfsr_apply(const uint32_t* col, uint32_t s) {
    uint32_t r = 0;
    for (uint32_t j = 0; j < kLfsrBits; ++j) {
        if ((s >> j) & 1u) r ^= col[j];
    }
    return r;
}

struct LfsrJump {
    uint32_t col[kLfsrBits][kLfsrBits];
};

constexpr LfsrJump make_lfsr_jump() {
    LfsrJump t{};
    for (uint32_t j = 0; j < kLfsrBits; ++j) {
        t.col[0][j] = lfsr_step(1u << j);
    }
    for (uint32_t i = 1; i < kLfsrBits; ++i) {
        for (uint32_t j = 0; j < kLfsrBits; ++j) {
            t.col[i][j] = lfsr_apply(t.col[i - 1], lfsr_apply(t.col[i - 1], 1u << j));
        }
    }
    return t;
}

constexpr LfsrJump kLfsrJump = make_lfsr_jump();

uint32_t lfsr_jump(uint32_t s, uint64_t k) {
    k %= kLfsrPeriod;
    for (uint32_t i = 0; i < kLfsrBits; ++i) {
        if ((k >> i) & 1u) s = lfsr_apply(kLfsrJump.col[i], s);
    }
    return s;
}

} // namespace

void AyChip::init(uint32_t clock_hz, bool ym, uint32_t rate_hz) {
    dac_ = ym ? kDacYm : kDacAy;
    out_.set_clock(clock_hz, 8u * rate_hz);
    now_ = 0;
    reset();
}

void AyChip::reset() {
    std::memset(regs_, 0, sizeof(regs_));
    for (uint32_t c = 0; c < kChannels; ++c) {
        tone_[c]        = 0;
        tone_last_[c]   = now_;
        tone_next_[c]   = now_;
        tone_period_[c] = 1;
        set_tone_period(c);
    }
    lfsr_         = 1;
    noise_period_ = 2u << shift_;
    noise_last_   = now_;
    noise_next_   = now_ + noise_period_;
    env_period_   = 1u << shift_;
    env_shape_    = 0;
    env_segment_  = 0;
    env_value_    = 0;
    env_last_     = now_;
    env_next_     = now_ + env_period_;
    update_activity();
}

uint64_t AyChip::retime(uint64_t last, uint32_t period, uint64_t now, uint64_t next) {
    if (next == kNever) return kNever;
    // Счётчик не сбрасывается: дошёл до нового периода - сработает на
    // следующем такте.
    const uint64_t at = last + period;
    return at > now ? at : now + 1u;
}

void AyChip::set_tone_period(uint32_t c) {
    uint32_t p = regs_[2u * c] | (static_cast<uint32_t>(regs_[2u * c + 1u]) << 8);
    if (p == 0) p = 1;
    p               <<= shift_;
    tone_period_[c]   = p;
    tone_avg_[c]      = static_cast<uint64_t>(p) * out_.mul() < out_.div();
    tone_next_[c]     = retime(tone_last_[c], p, now_, tone_next_[c]);
}

void AyChip::set_noise_period() {
    const uint32_t np = regs_[6] ? regs_[6] : 1u;
    noise_period_     = (2u * np) << shift_;
    noise_next_       = retime(noise_last_, noise_period_, now_, noise_next_);
}

void AyChip::set_env_period() {
    const uint32_t ep = regs_[11] | (static_cast<uint32_t>(regs_[12]) << 8);
    env_period_       = (ep ? ep : 1u) << shift_;
    env_next_         = retime(env_last_, env_period_, now_, env_next_);
}

void AyChip::set_period_shift(uint32_t shift) {
    catch_up();
    shift_ = static_cast<uint8_t>(shift);
    for (uint32_t c = 0; c < kChannels; ++c) {
        set_tone_period(c);
    }
    set_noise_period();
    set_env_period();
    update_activity();
    emit(now_);
}

void AyChip::reset_segment() {
    const Seg s = kEnvSeg[env_shape_][env_segment_];
    env_value_  = (s == kDown || s == kHoldTop) ? 31 : 0;
}

void AyChip::set_envelope_shape() {
    env_shape_   = regs_[13] & 0x0fu;
    env_segment_ = 0;
    reset_segment();
    env_last_ = now_;
    env_next_ = is_hold(kEnvSeg[env_shape_][0]) ? kNever : now_ + env_period_;
}

// Шаги огибающей разом. Непрерывная форма повторяется через 64 шага, у
// формы с удержанием оно наступает не позже 64-го.
void AyChip::env_advance(uint64_t k) {
    const bool holds = is_hold(kEnvSeg[env_shape_][0]) || is_hold(kEnvSeg[env_shape_][1]);
    k                = holds ? (k > 64u ? 64u : k) : k % 64u;
    for (; k != 0 && !is_hold(kEnvSeg[env_shape_][env_segment_]); --k) {
        env_step();
    }
}

// Таймеры, которых не слышно, событий не ставят: здесь их счёт доходит до
// текущего такта разом - число срабатываний делением, их итог - чётностью
// у тона, прыжком у шума, шагами у огибающей.
void AyChip::catch_up() {
    for (uint32_t c = 0; c < kChannels; ++c) {
        if (tone_on_[c] || tone_next_[c] > now_) continue;
        const uint64_t k  = (now_ - tone_next_[c]) / tone_period_[c] + 1u;
        tone_[c]         ^= static_cast<uint8_t>(k & 1u);
        tone_last_[c]     = tone_next_[c] + (k - 1u) * tone_period_[c];
        tone_next_[c]     = tone_last_[c] + tone_period_[c];
    }
    if (!noise_on_ && noise_next_ <= now_) {
        const uint64_t k = (now_ - noise_next_) / noise_period_ + 1u;
        lfsr_            = lfsr_jump(lfsr_, k);
        noise_last_      = noise_next_ + (k - 1u) * noise_period_;
        noise_next_      = noise_last_ + noise_period_;
    }
    if (!env_on_ && env_next_ <= now_) {
        const uint64_t k = (now_ - env_next_) / env_period_ + 1u;
        env_advance(k);
        env_last_ = env_next_ + (k - 1u) * env_period_;
        env_next_ = is_hold(kEnvSeg[env_shape_][env_segment_]) ? kNever : env_last_ + env_period_;
    }
}

void AyChip::settle() {
    catch_up();
}

// Слышен ли таймер: тон - канал его пропускает, не средний и громкость не
// ноль; шум - так хоть у одного канала; огибающая - включена хоть у одного.
// Уровни ЦАП 0 и 1 - ноль: громкость 0 молчит.
void AyChip::update_activity() {
    const uint8_t mix = regs_[7];
    noise_on_         = false;
    env_on_           = false;
    for (uint32_t c = 0; c < kChannels; ++c) {
        const uint8_t v  = regs_[8u + c];
        const bool loud  = (v & 0x10u) != 0 || (v & 0x0fu) != 0;
        tone_on_[c]      = loud && !tone_avg_[c] && (mix & (1u << c)) == 0;
        noise_on_       |= loud && (mix & (8u << c)) == 0;
        env_on_         |= (v & 0x10u) != 0;
    }
}

void AyChip::env_step() {
    const Seg s = kEnvSeg[env_shape_][env_segment_];
    if (s == kUp) {
        if (++env_value_ > 31) {
            env_segment_ ^= 1u;
            reset_segment();
        }
    } else if (s == kDown) {
        if (--env_value_ < 0) {
            env_segment_ ^= 1u;
            reset_segment();
        }
    }
}

int32_t AyChip::level(uint32_t c) const {
    const uint8_t mix  = regs_[7];
    const uint8_t v    = regs_[8u + c];
    const uint32_t amp = (v & 0x10u) ? static_cast<uint32_t>(env_value_) : (v & 0x0fu) * 2u + 1u;
    const int32_t full = dac_[amp];
    // Половинки: тон выключен - 2, звучит средним - 1, иначе 0 или 2.
    const int32_t tone  = (mix & (1u << c)) ? 2 : (tone_avg_[c] ? 1 : tone_[c] * 2);
    const int32_t noise = (mix & (8u << c)) ? 1 : static_cast<int32_t>(lfsr_ & 1u);
    return (full * tone * noise) >> 1;
}

void AyChip::emit(uint64_t t) {
    const auto p = out_.at(t);
    for (uint32_t c = 0; c < kChannels; ++c) {
        out_.put(p, c, level(c));
    }
}

void AyChip::step_to(uint64_t t) {
    for (;;) {
        uint64_t e = kNever;
        if (noise_on_) e = noise_next_;
        if (env_on_ && env_next_ < e) e = env_next_;
        for (uint32_t c = 0; c < kChannels; ++c) {
            if (tone_on_[c] && tone_next_[c] < e) e = tone_next_[c];
        }
        if (e > t) break;
        for (uint32_t c = 0; c < kChannels; ++c) {
            if (!tone_on_[c] || tone_next_[c] != e) continue;
            tone_[c]      ^= 1u;
            tone_last_[c]  = e;
            tone_next_[c]  = e + tone_period_[c];
        }
        if (noise_on_ && noise_next_ == e) {
            const uint32_t bit = (lfsr_ ^ (lfsr_ >> 3)) & 1u;
            lfsr_              = (lfsr_ >> 1) | (bit << 16);
            noise_last_        = e;
            noise_next_        = e + noise_period_;
        }
        if (env_on_ && env_next_ == e) {
            env_step();
            env_last_ = e;
            env_next_ = is_hold(kEnvSeg[env_shape_][env_segment_]) ? kNever : e + env_period_;
        }
        emit(e);
    }
    if (t > now_) now_ = t;
}

void AyChip::begin(uint32_t n, Row* rows) {
    const uint64_t first = out_.begin(n, rows);
    if (now_ < first) now_ = first;
}

void AyChip::run_to(uint64_t units) {
    step_to(units < out_.last() ? units : out_.last());
}

void AyChip::write(uint8_t reg, uint8_t value) {
    catch_up();
    reg        &= 0x0fu;
    regs_[reg]  = value & kRegMask[reg];
    switch (reg) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
            set_tone_period(reg >> 1);
            break;
        case 6:
            set_noise_period();
            break;
        case 11:
        case 12:
            set_env_period();
            break;
        case 13:
            set_envelope_shape();
            break;
        default:
            break;
    }
    update_activity();
    emit(now_);
}

bool AyChip::end(int16_t* const out[kChannels]) {
    step_to(out_.last());
    return out_.end(out);
}

} // namespace soundsinth::chips
