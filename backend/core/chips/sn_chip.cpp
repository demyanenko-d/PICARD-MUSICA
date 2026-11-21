// SPDX-License-Identifier: MIT
#include "core/chips/sn_chip.h"

namespace soundsinth::chips {

namespace {

// Громкость в Q15 (1.0 = 32768): шаг 2 дБ, 15 - тишина. Как vol_table MAME.
constexpr uint16_t kVolume[16] = {32768, 26029, 20675, 16423, 13045, 10362, 8231, 6538, 5193, 4125, 3277, 2603, 2068, 1642, 1305, 0};

} // namespace

void SnChip::init(uint32_t clock_hz, const SnVariant& variant, uint32_t rate_hz) {
    v_ = variant;
    // Отводы - два младших единичных разряда маски; один - он же второй.
    tap1_     = v_.taps & (0u - v_.taps);
    tap2_     = (v_.taps & ~tap1_) ? ((v_.taps & ~tap1_) & (0u - (v_.taps & ~tap1_))) : tap1_;
    feedback_ = 1u << (v_.width - 1u);
    out_.set_clock(clock_hz, rate_hz * (v_.divide8 ? 16u : 2u));
    now_ = 0;
    reset();
}

void SnChip::reset() {
    const bool sega = !v_.freq0_max;
    latch_          = sega ? 3 : 0;
    for (uint32_t i = 0; i < 8; i += 2) {
        regs_[i]      = 0;
        regs_[i + 1u] = sega ? 0x0f : 0;
    }
    for (uint32_t c = 0; c < kChannels; ++c) {
        bit_[c]    = 0;
        period_[c] = (sega || c == 3) ? 0 : 0x400;
        next_[c]   = now_ + 1u; // счётчик в нуле: перепад на первом такте
    }
    for (uint32_t c = 0; c < 3; ++c) {
        set_period(c);
    }
    lfsr_   = feedback_;
    bit_[3] = static_cast<uint8_t>(lfsr_ & 1u);
    stereo_ = 0xff;
    update_activity();
}

void SnChip::set_period(uint32_t c) {
    const uint32_t p = period_[c] ? period_[c] : 1u;
    avg_[c]          = static_cast<uint64_t>(p) * out_.mul() < out_.div();
}

// Тон слышен: громкость не ноль и он не средний. Шум - громкость не ноль.
void SnChip::update_activity() {
    for (uint32_t c = 0; c < 3; ++c) {
        on_[c] = (regs_[2u * c + 1u] & 0x0fu) != 0x0fu && !avg_[c];
    }
    // Белый шум чаще шага на отсчёт выхода - средним по отсчёту, без событий.
    const bool heard = (regs_[7] & 0x0fu) != 0x0fu;
    flat_            = heard && (regs_[6] & 4u) != 0 && static_cast<uint64_t>(period_[3] ? period_[3] : 1u) * out_.mul() < out_.div();
    on_[3]           = heard && !flat_;
}

// Неслышные каналы событий не ставят. Перепады тона до текущего такта -
// разом, итог чётностью: счётчик перезаряжается периодом, который был на
// момент перепада, а писался он только записью - значит, до неё он один.
// Шум проходит свои шаги по одному, но выхода не трогает.
void SnChip::catch_up() {
    for (const uint32_t p = period_[3] ? period_[3] : 1u; !on_[3] && !flat_ && next_[3] <= now_; next_[3] += p) {
        noise_step();
    }
    for (uint32_t c = 0; c < 3; ++c) {
        if (on_[c] || next_[c] > now_) continue;
        const uint32_t p  = period_[c] ? period_[c] : 1u;
        const uint64_t k  = (now_ - next_[c]) / p + 1u;
        bit_[c]          ^= static_cast<uint8_t>(k & 1u);
        next_[c]         += k * p;
    }
}

// Шаги шума до такта t (шаг в самом t - уже следующего куска) с площадью
// единиц.
void SnChip::flat_to(uint64_t t) {
    for (const uint32_t p = period_[3] ? period_[3] : 1u; next_[3] < t; next_[3] += p) {
        area_ += bit_[3] * (next_[3] - from_);
        from_  = next_[3];
        noise_step();
    }
    area_ += bit_[3] * (t - from_);
    from_  = t;
}

// Среднее шума с начала куска до такта t - уровнем отсчёта перед idx.
void SnChip::flat_put(uint64_t t, uint32_t idx) {
    flat_to(t);
    if (t != start_) {
        const int32_t y = static_cast<int32_t>(kVolume[regs_[7] & 0x0fu] * area_ / (t - start_));
        out_.put_flat(idx, 3, v_.negate ? -y : y);
    }
    start_ = t;
    area_  = 0;
}

void SnChip::settle() {
    catch_up();
}

void SnChip::noise_step() {
    const bool b1   = (lfsr_ & tap1_) != 0;
    const bool b2   = (lfsr_ & tap2_) != (v_.ncr ? tap2_ : 0u);
    lfsr_         >>= 1;
    if (b1 != (b2 && (regs_[6] & 4u) != 0)) lfsr_ |= feedback_;
    bit_[3] = static_cast<uint8_t>(lfsr_ & 1u);
}

int32_t SnChip::level(uint32_t c) const {
    const int32_t full = kVolume[regs_[2u * c + 1u] & 0x0fu];
    // Половинки: средний тон - 1, иначе 0 или 2.
    const int32_t half = (c < 3 && avg_[c]) ? 1 : bit_[c] * 2;
    const int32_t y    = (full * half) >> 1;
    return v_.negate ? -y : y;
}

void SnChip::emit(uint64_t t) {
    const auto p = out_.at(t);
    for (uint32_t c = 0; c < kChannels - (flat_ ? 1u : 0u); ++c) {
        out_.put(p, c, level(c));
    }
}

void SnChip::step_to(uint64_t t) {
    for (;;) {
        uint64_t e = kNever;
        for (uint32_t c = 0; c < kChannels; ++c) {
            if (on_[c] && next_[c] < e) e = next_[c];
        }
        // Граница отсчёта: среднее шума за отсчёт перед ней.
        const uint64_t edge = flat_ && edge_ < batch_ ? out_.first(edge_) : kNever;
        if (edge <= e) {
            if (edge > t) break;
            flat_put(edge, edge_++);
            continue;
        }
        if (e > t) break;
        for (uint32_t c = 0; c < 3; ++c) {
            if (!on_[c] || next_[c] != e) continue;
            bit_[c]  ^= 1u;
            next_[c]  = e + (period_[c] ? period_[c] : 1u);
        }
        if (on_[3] && next_[3] == e) {
            noise_step();
            next_[3] = e + (period_[3] ? period_[3] : 1u);
        }
        emit(e);
    }
    if (t > now_) now_ = t;
}

void SnChip::begin(uint32_t n, Row* rows) {
    const uint64_t first = out_.begin(n, rows);
    if (now_ < first) now_ = first;
    batch_ = n < kMaxBatch ? n : kMaxBatch;
    edge_  = 0;
}

void SnChip::run_to(uint64_t units) {
    step_to(units < out_.last() ? units : out_.last());
}

void SnChip::write(uint8_t data) {
    catch_up();
    if (flat_) flat_put(now_, out_.at(now_).idx + 1u);
    uint32_t r = latch_;
    if (data & 0x80u) {
        r      = (data >> 4) & 7u;
        latch_ = static_cast<uint8_t>(r);
        if (v_.ncr && r == 6 && (data & 4u) != (regs_[6] & 4u)) lfsr_ = feedback_;
        regs_[r] = static_cast<uint16_t>((regs_[r] & 0x3f0u) | (data & 0x0fu));
    }
    const uint32_t c = r >> 1;
    switch (r) {
        case 0:
        case 2:
        case 4:
            if (!(data & 0x80u)) regs_[r] = static_cast<uint16_t>((regs_[r] & 0x0fu) | ((data & 0x3fu) << 4));
            period_[c] = (regs_[r] != 0 || !v_.freq0_max) ? regs_[r] : 0x400u;
            set_period(c);
            if (r == 4 && (regs_[6] & 3u) == 3u) period_[3] = period_[2] << 1;
            break;
        case 6:
            if (!(data & 0x80u)) regs_[6] = static_cast<uint16_t>((regs_[6] & 0x3f0u) | (data & 0x0fu));
            period_[3] = (regs_[6] & 3u) == 3u ? period_[2] << 1 : 1u << (5u + (regs_[6] & 3u));
            if (!v_.ncr) lfsr_ = feedback_;
            break;
        default: // громкость: младшая тетрада байта, и с защёлкой, и без
            regs_[r] = static_cast<uint16_t>((regs_[r] & 0x3f0u) | (data & 0x0fu));
            break;
    }
    update_activity();
    if (flat_) {
        // Среднее - с этого такта.
        start_ = from_ = now_;
        area_          = 0;
        edge_          = out_.at(now_).idx + 1u;
    }
    emit(now_);
}

bool SnChip::end(int16_t* const out[kChannels]) {
    step_to(out_.last());
    catch_up();
    return out_.end(out);
}

} // namespace soundsinth::chips
