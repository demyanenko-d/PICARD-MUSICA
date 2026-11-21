// SPDX-License-Identifier: MIT
#include "core/chips/saa_chip.h"

namespace soundsinth::chips {

namespace {

// Громкость с огибающей: разряды 3-1 громкости x уровень огибающей; числа
// SAASound (снятые с чипа).
constexpr uint8_t kPdm[8][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 23},
    {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30},
    {0, 3, 5, 8, 10, 13, 15, 18, 20, 23, 25, 28, 30, 33, 35, 38},
    {0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45},
    {0, 4, 7, 11, 14, 18, 21, 25, 28, 32, 35, 39, 42, 46, 49, 53},
};

// Формы огибающей: число участков, повтор, вид участка.
enum Shape : uint8_t { kZero, kMax, kDown, kUp };
struct Form {
    uint8_t phases;
    bool looping;
    Shape shape[2];
};
constexpr Form kForms[8] = {
    {1, false, {kZero, kZero}}, {1, true, {kMax, kMax}}, {1, false, {kDown, kZero}}, {1, true, {kDown, kZero}},
    {2, false, {kUp, kDown}},   {2, true, {kUp, kDown}}, {1, false, {kUp, kZero}},   {1, true, {kUp, kZero}},
};

uint8_t shape_level(Shape s, uint8_t pos) {
    switch (s) {
        case kMax:
            return 15;
        case kDown:
            return static_cast<uint8_t>(15u - pos);
        case kUp:
            return pos;
        default:
            return 0;
    }
}

// Шум: k шагов регистра Галуа разом - матрица M^k по 18 разрядам. Разряды
// выше 17 (начальное 0xffffffff) уходят за первые 14 шагов - их шагами.
constexpr uint32_t kNoiseBits   = 18;
constexpr uint32_t kNoisePeriod = (1u << kNoiseBits) - 1u;

constexpr uint32_t noise_step(uint32_t s) {
    return (s & 1u) ? (s >> 1) ^ 0x20400u : s >> 1;
}

constexpr uint32_t noise_apply(const uint32_t* col, uint32_t s) {
    uint32_t r = 0;
    for (uint32_t j = 0; j < kNoiseBits; ++j) {
        if ((s >> j) & 1u) r ^= col[j];
    }
    return r;
}

struct NoiseJump {
    uint32_t col[kNoiseBits][kNoiseBits];
};

constexpr NoiseJump make_noise_jump() {
    NoiseJump t{};
    for (uint32_t j = 0; j < kNoiseBits; ++j) {
        t.col[0][j] = noise_step(1u << j);
    }
    for (uint32_t i = 1; i < kNoiseBits; ++i) {
        for (uint32_t j = 0; j < kNoiseBits; ++j) {
            t.col[i][j] = noise_apply(t.col[i - 1], noise_apply(t.col[i - 1], 1u << j));
        }
    }
    return t;
}

constexpr NoiseJump kNoiseJump = make_noise_jump();

uint32_t noise_jump(uint32_t s, uint64_t k) {
    for (; k != 0 && (s >> kNoiseBits) != 0; --k) {
        s = noise_step(s);
    }
    k %= kNoisePeriod;
    for (uint32_t i = 0; i < kNoiseBits; ++i) {
        if ((k >> i) & 1u) s = noise_apply(kNoiseJump.col[i], s);
    }
    return s;
}

} // namespace

void SaaChip::init(uint32_t clock_hz, uint32_t rate_hz) {
    out_.set_clock(clock_hz, 2u * rate_hz);
    now_ = 0;
    reset();
}

void SaaChip::reset() {
    reg_     = 0;
    enabled_ = false;
    sync_    = false;
    for (uint32_t c = 0; c < kChannels; ++c) {
        amp_[c]       = 0;
        mix_[c]       = 0;
        osc_[c]       = Osc{};
        osc_[c].level = 1;
        set_period(osc_[c]);
        osc_[c].next = now_ + osc_[c].period;
    }
    for (Noise& n : noise_) {
        n        = Noise{};
        n.rand   = 0xffffffffu;
        n.period = 128;
        n.next   = now_ + n.period;
    }
    for (Env& e : env_) {
        e       = Env{};
        e.ended = true;
        e.res   = 1;
    }
    update_activity();
}

void SaaChip::noise_shift(Noise& n) {
    n.rand = noise_step(n.rand);
}

// Полупериод тона: перепад, такт шума или огибающей, новые октава и
// частота.
void SaaChip::toggle(uint32_t c, uint64_t t) {
    Osc& o   = osc_[c];
    o.level ^= 1u;
    if ((c == 0 || c == 3) && noise_[c / 3].mode == 3) noise_shift(noise_[c / 3]);
    if (c == 1 || c == 4) {
        Env& e = env_[c / 3];
        if (e.enabled && !e.external) env_tick(e);
    }
    if (o.pending) {
        o.oct = o.next_oct;
        if (!o.ignore_off) {
            o.off     = o.next_off;
            o.pending = false;
        }
        o.ignore_off = false;
        set_period(o);
    }
    o.next = t + o.period;
}

void SaaChip::env_levels(Env& e) {
    const uint8_t mask = e.res == 2 ? 0x0e : 0x0f;
    const uint8_t l    = (e.ended && !e.looping) ? 0 : static_cast<uint8_t>(shape_level(kForms[e.form].shape[e.phase], e.pos) & mask);
    e.level[0]         = l;
    e.level[1]         = e.invert ? static_cast<uint8_t>(mask - l) : l;
}

void SaaChip::env_load(Env& e, uint8_t data) {
    e.phase    = 0;
    e.pos      = 0;
    e.form     = (data >> 1) & 7u;
    e.invert   = (data & 0x01u) != 0;
    e.external = (data & 0x20u) != 0;
    e.phases   = kForms[e.form].phases;
    e.looping  = kForms[e.form].looping;
    e.res      = (data & 0x10u) ? 2 : 1;
    e.enabled  = (data & 0x80u) != 0;
    e.ended    = !e.enabled;
    env_levels(e);
}

void SaaChip::env_tick(Env& e) {
    if (e.ended) return;
    e.pos           += e.res;
    bool period_end  = false;
    if (e.pos >= 16) {
        if (++e.phase == e.phases) {
            period_end = true;
            if (e.looping) {
                e.phase  = 0;
                e.pos   -= 16;
            } else {
                e.ended = true;
            }
        } else {
            e.pos -= 16;
        }
    }
    if (e.pending && period_end) {
        e.pending = false;
        env_load(e, e.next_data);
        reload_ = true;
    } else {
        env_levels(e);
    }
}

void SaaChip::env_control(Env& e, uint8_t data) {
    const bool on = (data & 0x80u) != 0;
    if (!on && !e.enabled) return;
    e.enabled = on;
    if (!on) {
        e.ended = true;
        return;
    }
    const uint8_t res = (data & 0x10u) ? 2 : 1;
    if (e.res == 1 && res == 2) e.pos &= 0x0eu;
    if (e.res == 2 && res == 1) e.pos |= 0x01u;
    e.res = res;
    if (e.ended) {
        env_load(e, data);
        e.pending = false;
    } else {
        env_levels(e);
        e.pending   = true;
        e.next_data = data;
    }
}

// Слышен ли канал; нужен ли тон (слышен с тоном или тактует шум либо
// огибающую) и шум (его слышит хоть один канал группы).
void SaaChip::update_activity() {
    bool heard[kChannels];
    for (uint32_t c = 0; c < kChannels; ++c) {
        heard[c] = enabled_ && mix_[c] != 0 && amp_[c] != 0;
    }
    for (uint32_t g = 0; g < 2; ++g) {
        bool used = false;
        for (uint32_t c = 3 * g; c < 3 * g + 3; ++c) {
            used |= heard[c] && (mix_[c] & 2u) != 0;
        }
        noise_[g].on = !sync_ && used && noise_[g].mode != 3;
    }
    for (uint32_t c = 0; c < kChannels; ++c) {
        bool drives = false;
        if (c == 0 || c == 3) drives = noise_[c / 3].mode == 3;
        if (c == 1 || c == 4) drives = env_[c / 3].enabled && !env_[c / 3].external;
        osc_[c].on = !sync_ && (drives || (heard[c] && (mix_[c] & 1u) != 0));
    }
}

// Неслышное - к текущему такту разом: тон с ждущими октавой и частотой -
// по полупериоду, дальше чётностью; шум - прыжком.
void SaaChip::catch_up() {
    if (sync_) return;
    for (uint32_t c = 0; c < kChannels; ++c) {
        Osc& o = osc_[c];
        if (o.on) continue;
        while (o.pending && o.next <= now_) {
            toggle(c, o.next);
        }
        if (o.next > now_) continue;
        const uint64_t k  = (now_ - o.next) / o.period + 1u;
        o.level          ^= static_cast<uint8_t>(k & 1u);
        o.next           += k * o.period;
    }
    for (Noise& n : noise_) {
        if (n.on || n.mode == 3 || n.next > now_) continue;
        const uint64_t k  = (now_ - n.next) / n.period + 1u;
        n.rand            = noise_jump(n.rand, k);
        n.next           += k * n.period;
    }
}

void SaaChip::settle() {
    catch_up();
}

int32_t SaaChip::level(uint32_t c, uint32_t side) const {
    if (!enabled_ || sync_) return 0;
    const uint32_t a     = side ? amp_[c] >> 4 : amp_[c] & 0x0fu;
    const uint32_t tone  = osc_[c].level;
    const uint32_t noise = noise_[c / 3].rand & 1u;
    uint32_t mid         = 0;
    switch (mix_[c]) {
        case 1:
            mid = tone * 2u;
            break;
        case 2:
            mid = noise * 2u;
            break;
        case 3:
            mid = tone * (2u - noise);
            break;
        default:
            break;
    }
    const Env& e = env_[c / 3];
    if ((c == 2 || c == 5) && e.enabled) return static_cast<int32_t>(kPdm[a >> 1][e.level[side]] * 4u * (2u - mid)) * kScale;
    return static_cast<int32_t>(a * mid * 16u) * kScale;
}

void SaaChip::emit(uint64_t t) {
    const auto p = out_.at(t);
    for (uint32_t c = 0; c < kChannels; ++c) {
        out_.put(p, 2u * c, level(c, 0));
        out_.put(p, 2u * c + 1u, level(c, 1));
    }
}

void SaaChip::step_to(uint64_t t) {
    if (!sync_) {
        for (;;) {
            uint64_t e = kNever;
            for (const Osc& o : osc_) {
                if (o.on && o.next < e) e = o.next;
            }
            for (const Noise& n : noise_) {
                if (n.on && n.next < e) e = n.next;
            }
            if (e > t) break;
            for (Noise& n : noise_) {
                if (!n.on || n.next != e) continue;
                noise_shift(n);
                n.next = e + n.period;
            }
            for (uint32_t c = 0; c < kChannels; ++c) {
                if (osc_[c].on && osc_[c].next == e) toggle(c, e);
            }
            if (reload_) {
                now_ = e;
                catch_up();
                update_activity();
                reload_ = false;
            }
            emit(e);
        }
    }
    if (t > now_) now_ = t;
}

void SaaChip::begin(uint32_t n, Row* rows) {
    const uint64_t first = out_.begin(n, rows);
    if (now_ < first) now_ = first;
}

void SaaChip::run_to(uint64_t units) {
    step_to(units < out_.last() ? units : out_.last());
}

void SaaChip::write_address(uint8_t reg) {
    catch_up();
    reg_ = reg & 0x1fu;
    if (reg_ == 24 || reg_ == 25) {
        Env& e = env_[reg_ - 24];
        if (e.enabled && e.external) env_tick(e);
        update_activity();
        reload_ = false;
    }
    emit(now_);
}

void SaaChip::write_data(uint8_t v) {
    catch_up();
    if (reg_ < 6) {
        amp_[reg_] = v;
    } else if (reg_ >= 8 && reg_ < 14) {
        Osc& o = osc_[reg_ - 8];
        if (!sync_) {
            o.next_off = v;
            o.pending  = true;
            if (o.next_oct == o.oct) o.ignore_off = true;
        } else {
            o.pending    = false;
            o.ignore_off = false;
            o.off = o.next_off = v;
            o.oct              = o.next_oct;
            set_period(o);
        }
    } else if (reg_ >= 16 && reg_ < 19) {
        for (uint32_t i = 0; i < 2; ++i) {
            Osc& o            = osc_[2u * (reg_ - 16u) + i];
            const uint8_t oct = (v >> (4u * i)) & 7u;
            if (!sync_) {
                o.next_oct   = oct;
                o.pending    = true;
                o.ignore_off = false;
            } else {
                o.pending    = false;
                o.ignore_off = false;
                o.oct = o.next_oct = oct;
                o.off              = o.next_off;
                set_period(o);
            }
        }
    } else if (reg_ == 20 || reg_ == 21) {
        const uint8_t bit = reg_ == 20 ? 1u : 2u;
        for (uint32_t c = 0; c < kChannels; ++c) {
            mix_[c] = static_cast<uint8_t>(((v >> c) & 1u) ? mix_[c] | bit : mix_[c] & ~bit);
        }
    } else if (reg_ == 22) {
        for (uint32_t g = 0; g < 2; ++g) {
            Noise& n           = noise_[g];
            const uint8_t mode = (v >> (4u * g)) & 3u;
            if (mode == n.mode) continue;
            // Счётчик сдвига переживает смену режима: до сдвига остаётся
            // 512 долей, такт прибавляет 4 >> режим; в режиме 3 счётчик стоит.
            const uint32_t left = n.mode == 3 ? n.held : (sync_ ? 512u : static_cast<uint32_t>(n.next - now_) * (4u >> n.mode));
            n.mode              = mode;
            n.period            = 128u << (mode == 3 ? 0u : mode);
            n.held              = left;
            if (mode == 3 || sync_) {
                n.next = kNever;
            } else {
                const uint32_t step = 4u >> mode;
                n.next              = now_ + (left + step - 1u) / step;
            }
        }
    } else if (reg_ == 24 || reg_ == 25) {
        env_control(env_[reg_ - 24], v);
    } else if (reg_ == 28) {
        const bool sync = (v & 0x02u) != 0;
        if (sync != sync_) {
            sync_ = sync;
            for (Osc& o : osc_) {
                if (sync) {
                    o.level = 1;
                    o.oct   = o.next_oct;
                    o.off   = o.next_off;
                    set_period(o);
                }
                o.next = sync ? kNever : now_ + o.period;
            }
            // Синхронизация обнуляет счётчик шума.
            for (Noise& n : noise_) {
                n.held = 512;
                n.next = (sync || n.mode == 3) ? kNever : now_ + n.period;
            }
        }
        enabled_ = (v & 0x01u) != 0;
    }
    update_activity();
    emit(now_);
}

bool SaaChip::end(int16_t* const out[kOutputs]) {
    step_to(out_.last());
    return out_.end(out);
}

} // namespace soundsinth::chips
