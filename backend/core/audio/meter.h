// SPDX-License-Identifier: MIT
#pragma once

// Измерители выхода шины для телеметрии: пики каналов (VU) и спектр в 16
// полос. Спектр - БПФ по последним 512 отсчётам суммы каналов (11.6 мс):
// рендер пишет их по кругу, читатель копирует круг в рабочий буфер, на время
// копии ставя hold_. Рендер и читатель - задачи одного ядра, рендер старше:
// читатель не прерывает feed(). Счёт чаще отправки ловит короткий удар, а
// атаку, замеченную рендером, счёт берёт ещё и окном с ней в центре: круг
// вдвое длиннее окна. Отправляется максимум полос за период.
// Полосы от 86 Гц до 16 кГц, к верху шире.
//
// Шкала уровня - байт: 255 - полная шкала, шаг 1/32 октавы (0.19 дБ), 0 -
// на 48 дБ ниже полной шкалы и тише.

#include <atomic>
#include <cstdint>

namespace soundsinth::mixbus {

inline constexpr uint32_t kSpectrumBands = 16;

// Двоичный логарифм в 1/2^frac_bits; 0 при v == 0.
int32_t log2_fixed(uint32_t v, uint32_t frac_bits);

// Уровень амплитуды 0..32768 по шкале выше.
uint8_t level_of(uint32_t amplitude);

class Meter {
public:
    static constexpr uint32_t kFft = 512;

    // Измерять ли: пока выключен, рендер feed() не зовёт.
    void enable(bool on) { on_ = on; }
    bool enabled() const { return on_; }
    // Готовый буфер выхода шины; зовёт рендер.
    void feed(const int16_t* interleaved, uint32_t n_frames);
    // Пики каналов с прошлого вызова.
    void take_vu(uint8_t& left, uint8_t& right);
    // Полосы по последним kFft отсчётам и по окну с атакой в центре, если
    // она была, - в максимум за период. false - считать было нечего.
    bool analyze();
    // Максимум полос с прошлого вызова, последний счёт - по свежим отсчётам.
    // false - счёта не было.
    bool take_spectrum(uint8_t bands[kSpectrumBands]);

private:
    static constexpr uint32_t kRing       = 2 * kFft;
    static constexpr uint32_t kOnsetFloor = 1024; // -30 дБ от полной шкалы

    void count(uint32_t end);

    uint16_t peak_l_     = 0;
    uint16_t peak_r_     = 0;
    uint16_t last_peak_  = 0; // пик прошлого буфера - для атаки
    uint32_t written_    = 0; // отсчётов записано в круг, по модулю 2^32
    uint32_t counted_at_ = 0; // written_ на прошлом счёте последнего окна
    uint32_t onset_at_   = 0; // середина буфера с атакой, шкала written_
    bool onset_pending_  = false;
    bool counted_        = false; // в max_ есть счёт
    std::atomic<bool> hold_{false};
    bool on_                     = false;
    uint8_t max_[kSpectrumBands] = {};
    int16_t ring_[kRing];
    // Рабочий буфер БПФ: пара отсчётов - комплексное слово.
    alignas(4) int16_t work_[kFft];
};

} // namespace soundsinth::mixbus
