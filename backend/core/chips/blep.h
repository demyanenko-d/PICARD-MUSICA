// SPDX-License-Identifier: MIT
#pragma once

// Ступеньки с ограниченной полосой для чипов с перепадами (AY/YM и
// родня). Перепад уровня d в дробной позиции phase / kBlepPhases отсчёта idx
// раскладывается по kBlepTaps отсчётам буфера разностей; нарастающий итог
// буфера - выход. Задержка - kBlepTaps / 2 отсчётов.
//
// Сумма строки таблицы - ровно 32768, а последний отвод берёт остаток
// округления: сумма вклада всегда равна d, и итог после любой серии
// перепадов возвращается к тому же уровню без ухода.

#include <cstdint>
#include <cstring>

namespace soundsinth::chips {

inline constexpr uint32_t kBlepTaps   = 16;
inline constexpr uint32_t kBlepPhases = 32;

extern const int16_t kBlepTable[kBlepPhases][kBlepTaps];

// buf - не меньше idx + kBlepTaps элементов.
inline void blep_add(int32_t* buf, uint32_t idx, uint32_t phase, int32_t d) {
    const int16_t* k = kBlepTable[phase];
    int32_t* out     = buf + idx;
    int32_t sum      = 0;
    for (uint32_t i = 0; i + 1 < kBlepTaps; ++i) {
        const int32_t c  = (d * k[i] + 16384) >> 15;
        out[i]          += c;
        sum             += c;
    }
    out[kBlepTaps - 1] += d - sum;
}

// Выход чипа с перепадами: N каналов, батч до kMaxBatch отсчётов. Такт t
// ложится в отсчёт t * mul / div от начала (q, r - начало батча в тактах и
// остаток), без накопления ошибки. Перепады канала - в строку разностей,
// end() сводит их в отсчёты int16 (уровень / 2).
//
// Строки разностей даёт вызывающий на время от begin() до end(): чипы,
// которые считаются по очереди, делят одни и те же. В объекте между
// батчами живёт только хвост ступенек, kBlepTaps разностей на канал.
template <uint32_t N, uint32_t kMaxBatch>
class BlepOut {
public:
    using Row = int32_t[kMaxBatch + kBlepTaps];

    void set_clock(uint32_t div, uint32_t mul) {
        div_ = div;
        mul_ = mul;
        q_   = 0;
        r_   = 0;
        lim_ = 0;
    }

    // Батч из n отсчётов; возвращает его первый такт. Запись до первого
    // события встанет в его отсчёт, а не в прошлый батч. rows - N строк.
    uint64_t begin(uint32_t n, Row* rows) {
        rows_ = rows;
        for (bool& open : open_) {
            open = false;
        }
        n_                   = n < kMaxBatch ? n : kMaxBatch;
        const uint64_t total = static_cast<uint64_t>(n_) * div_ + r_;
        lim_                 = q_ + (total + mul_ - 1u) / mul_ - 1u;
        return q_ + (r_ != 0 ? 1u : 0u);
    }
    uint64_t last() const { return lim_; }
    // Первый такт отсчёта idx батча.
    uint64_t first(uint32_t idx) const { return q_ + (static_cast<uint64_t>(idx) * div_ + r_ + mul_ - 1u) / mul_; }
    uint32_t div() const { return div_; }
    uint32_t mul() const { return mul_; }

    // Позиция такта t в батче: отсчёт и фаза ступеньки.
    struct Pos {
        uint32_t idx, phase;
    };
    Pos at(uint64_t t) const {
        const uint32_t n = static_cast<uint32_t>((t - q_) * mul_ - r_);
        return {n / div_, static_cast<uint32_t>((static_cast<uint64_t>(n % div_) * kBlepPhases) / div_)};
    }
    // Уровень канала c с позиции p.
    void put(Pos p, uint32_t c, int32_t y) {
        if (y == y_[c]) return;
        blep_add(row(c), p.idx, p.phase, y - y_[c]);
        y_[c] = y;
    }

    // Уровень канала c за отсчёт перед idx, уже усреднённый по отсчёту: одна
    // разность без ступеньки, с той же задержкой.
    void put_flat(uint32_t idx, uint32_t c, int32_t y) {
        if (y == y_[c]) return;
        row(c)[idx + kBlepTaps / 2 - 1] += y - y_[c];
        y_[c]                            = y;
    }

    // true - в батче был хоть один ненулевой отсчёт.
    bool end(int16_t* const out[N]) {
        bool sounded = false;
        for (uint32_t c = 0; c < N; ++c) {
            int32_t acc = acc_[c];
            // Ступеньки в батче, хвост прошлых или уровень не ноль - канал
            // звучал.
            const bool steps   = open_[c] || tail_set_[c];
            sounded           |= steps || acc != 0;
            int16_t* const to  = out[c];
            // Без ступенек уровень постоянный.
            if (!steps) {
                const int16_t v = clip(acc);
                for (uint32_t i = 0; i < n_; ++i) {
                    to[i] = v;
                }
                continue;
            }
            const int32_t* const d = row(c);
            for (uint32_t i = 0; i < n_; ++i) {
                acc   += d[i];
                to[i]  = clip(acc);
            }
            acc_[c]   = acc;
            bool tail = false;
            for (uint32_t i = 0; i < kBlepTaps; ++i) {
                tail_[c][i]  = d[n_ + i];
                tail        |= d[n_ + i] != 0;
            }
            tail_set_[c] = tail;
        }
        const uint64_t total  = static_cast<uint64_t>(n_) * div_ + r_;
        q_                   += total / mul_;
        r_                    = static_cast<uint32_t>(total % mul_);
        return sounded;
    }

private:
    static int16_t clip(int32_t acc) {
        const int32_t v = acc >> 1;
        return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }

    // Строка канала c, готовая к батчу: хвост прошлых ступенек, дальше нули.
    int32_t* row(uint32_t c) {
        int32_t* const d = rows_[c];
        if (!open_[c]) {
            open_[c] = true;
            if (tail_set_[c]) {
                std::memcpy(d, tail_[c], sizeof(tail_[c]));
            } else {
                std::memset(d, 0, sizeof(tail_[c]));
            }
            std::memset(d + kBlepTaps, 0, n_ * sizeof(int32_t));
        }
        return d;
    }

    // Поля в нуле до set_clock(): объект с ненулевыми начальными значениями
    // лёг бы в .data целиком.
    uint32_t div_ = 0;
    uint32_t mul_ = 0;
    uint64_t q_   = 0;
    uint32_t r_   = 0;
    uint32_t n_   = 0;
    uint64_t lim_ = 0;

    Row* rows_                  = nullptr;
    int32_t y_[N]               = {};
    int32_t acc_[N]             = {};
    bool open_[N]               = {}; // строка канала заведена в этом батче
    bool tail_set_[N]           = {}; // в хвосте есть ненулевые разности
    int32_t tail_[N][kBlepTaps] = {};
};

} // namespace soundsinth::chips
