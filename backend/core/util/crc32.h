// SPDX-License-Identifier: MIT
#pragma once

// CRC-32 с отражённым полиномом и БЕЗ финальной инверсии: на "123456789"
// даёт 0x340BC6D9, стандартный CRC-32 - его дополнение.
//
// Одна функция на банк и на настройки, она же у пекаря банков на ПК:
// сумму, посчитанную на одной стороне, проверяет другая.

#include <cstdint>

namespace soundsinth::util {

inline constexpr uint32_t kCrc32Poly = 0xedb88320u;
inline constexpr uint32_t kCrc32Init = 0xffffffffu;

// Досчитать сумму по куску. Части одного потока считаются подряд, начиная
// с kCrc32Init.
inline uint32_t crc32_update(uint32_t crc, const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (kCrc32Poly & (0u - (crc & 1u)));
        }
    }
    return crc;
}

inline uint32_t crc32(const uint8_t* p, uint32_t n) {
    return crc32_update(kCrc32Init, p, n);
}

} // namespace soundsinth::util
