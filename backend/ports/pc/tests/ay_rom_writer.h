// SPDX-License-Identifier: MIT
#pragma once

// Записи в порты AY так, как их делает ПЗУ 128 (PLAY): выбор регистра 14 и
// по записи на бит, $FA - линия в 0, $FE - в 1. Кадр 8N1: старт-бит,
// восемь бит младшим вперёд, стоп-бит.

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "core/live_midi/ay_midi.h"

namespace pc_tests {

struct AyRomWriter {
    std::vector<uint16_t> w;

    void select(uint8_t reg) { w.push_back(static_cast<uint16_t>(soundsinth::midi_in::kAyWriteSelect | reg)); }
    void data(uint8_t v) { w.push_back(v); }
    void bit(bool one) { data(one ? 0xFEu : 0xFAu); }
    void byte(uint8_t b) {
        bit(false);
        for (int i = 0; i < 8; ++i)
            bit(((b >> i) & 1u) != 0);
        bit(true);
    }
    void bytes(std::initializer_list<uint8_t> list) {
        select(soundsinth::midi_in::kAyRegPortA);
        for (uint8_t b : list)
            byte(b);
    }
};

} // namespace pc_tests
