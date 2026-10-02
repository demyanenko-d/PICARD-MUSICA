// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#if defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

namespace soundsinth {

// v, ограниченное отрезком [lo, hi].
constexpr int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// v, ограниченное шкалой int16.
constexpr int32_t clamp_s16(int32_t v) {
    return clamp_i32(v, -32768, 32767);
}

// То же в горячем цикле: на ARM одна команда SSAT без ветвлений (сравнения
// clamp_s16 GCC в неё не сворачивает), на ПК - clamp_s16.
inline int32_t sat_s16(int32_t v) {
#if defined(__ARM_FEATURE_SAT)
    return __ssat(v, 16);
#else
    return clamp_s16(v);
#endif
}

} // namespace soundsinth
