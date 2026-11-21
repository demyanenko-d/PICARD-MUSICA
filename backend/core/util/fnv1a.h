// SPDX-License-Identifier: MIT
#pragma once

// FNV-1a, 32 разряда: свёртка h = (h ^ байт) * kFnvPrime с начала kFnvSeed.

#include <cstdint>

namespace soundsinth::util {

inline constexpr uint32_t kFnvSeed  = 2166136261u;
inline constexpr uint32_t kFnvPrime = 16777619u;

// Свёртка n байт от p поверх h (kFnvSeed или своя затравка).
inline uint32_t fnv1a(uint32_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * kFnvPrime;
    }
    return h;
}

} // namespace soundsinth::util
