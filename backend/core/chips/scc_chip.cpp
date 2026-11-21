// SPDX-License-Identifier: MIT
#include "core/chips/scc_chip.h"

namespace soundsinth::chips {

void SccChip::reset() {
    for (Voice& v : v_) {
        v = Voice{};
    }
    test_ = 0;
}

void SccChip::write(uint32_t port, uint8_t reg, uint8_t data) {
    switch (port) {
        case 0:
            if ((test_ & 0x40u) != 0 || ((test_ & 0x80u) != 0 && reg >= 0x60) || reg >= 0x80) return;
            v_[reg >> 5].wave[reg & 0x1fu] = static_cast<int8_t>(data);
            if (reg >= 0x60) v_[4].wave[reg & 0x1fu] = static_cast<int8_t>(data);
            break;
        case 1: {
            if (reg >= 2u * kChannels) return;
            Voice& v = v_[reg >> 1];
            // Стоящий канал после записи начинает с целого отсчёта волны.
            if ((test_ & 0x20u) != 0) {
                v.counter = ~0u;
            } else if (v.period < 9) {
                v.counter |= 0xffffu;
            }
            v.period   = static_cast<uint16_t>((reg & 1u) != 0 ? (v.period & 0x0ffu) | ((data & 0x0fu) << 8) : (v.period & 0xf00u) | data);
            v.counter &= 0xffff0000u;
            break;
        }
        case 2:
            if ((reg & 7u) < kChannels) v_[reg & 7u].volume = data & 0x0fu;
            break;
        case 3:
            for (uint32_t c = 0; c < kChannels; c++) {
                v_[c].key = ((data >> c) & 1u) != 0;
            }
            break;
        case 4:
            if ((test_ & 0x40u) == 0 && reg < 32u * kChannels) v_[reg >> 5].wave[reg & 0x1fu] = static_cast<int8_t>(data);
            break;
        case 5:
            test_ = data;
            break;
        default:
            break;
    }
}

void SccChip::generate(int16_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        out[i] = 0;
    }
    for (Voice& v : v_) {
        if (v.period < 9) continue;
        const uint32_t step = ((32u << 16) + (v.period + 1u) / 2u) / (v.period + 1u);
        const int32_t vol   = v.key ? v.volume : 0;
        // Молчащий канал идёт по волне дальше.
        if (vol == 0) {
            v.counter += step * n;
            continue;
        }
        uint32_t c = v.counter;
        for (uint32_t i = 0; i < n; i++) {
            c      += step;
            out[i]  = static_cast<int16_t>(out[i] + v.wave[(c >> 16) & 0x1fu] * vol);
        }
        v.counter = c;
    }
}

} // namespace soundsinth::chips
