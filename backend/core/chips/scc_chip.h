// SPDX-License-Identifier: MIT
#pragma once

// Konami SCC (K051649) и SCC+ (K052539): пять каналов, у каждого волна из 32
// отсчётов со знаком, период 12 разрядов и громкость 4 разряда. Счёт - на
// частоте вход / 16: место в волне идёт на 32 / (период + 1) за отсчёт, тон
// канала - вход / (16 * (период + 1)). Канал с периодом меньше 9 стоит.
//
// Регистры - шесть портов:
//   0 - волны SCC: четыре по 32 байта, канал 5 играет волну канала 4;
//   1 - периоды: младший и старший байт на канал;
//   2 - громкости; 3 - включение каналов, разряд на канал;
//   4 - волны SCC+: пять по 32 байта;
//   5 - проверка: разряд 6 запрещает запись волн, разряд 7 - волн каналов
//       4 и 5 через порт 0, разряд 5 - запись периода сбрасывает счётчик.

#include <cstdint>

namespace soundsinth::chips {

class SccChip {
public:
    static constexpr uint32_t kChannels = 5;

    void reset();
    void write(uint32_t port, uint8_t reg, uint8_t data);
    // n отсчётов суммы каналов на частоте вход / 16. Отсчёт канала - волна
    // на громкость: до 128 * 15.
    void generate(int16_t* out, uint32_t n);
    // Каналов с ключом и громкостью не ноль, для телеметрии.
    uint32_t heard() const {
        uint32_t n = 0;
        for (const Voice& v : v_) {
            n += v.key && v.volume != 0 ? 1u : 0u;
        }
        return n;
    }

private:
    struct Voice {
        uint32_t counter = 0; // место в волне, Q16
        uint16_t period  = 0;
        uint8_t volume   = 0;
        bool key         = false;
        int8_t wave[32]  = {};
    };
    Voice v_[kChannels];
    uint8_t test_ = 0;
};

} // namespace soundsinth::chips
