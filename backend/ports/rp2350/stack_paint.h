// SPDX-License-Identifier: MIT
#pragma once

// Запас стеков вне задач FreeRTOS: Core1 (SCRATCH_X) и MSP Core0
// (SCRATCH_Y) никто не стережёт. Каждое ядро заливает свой стек узором от
// дна до текущего sp, а строка лога считает, докуда узор цел.

#include <cstdint>

#include "pico/platform.h"

#include "platform/compiler.h"

namespace rp2350 {

// Узор незанятого стека. Заливает каждое ядро своё, от дна до текущего sp с
// запасом: ниже sp кадры ещё не легли.
inline constexpr uint32_t kStackPattern          = 0xa5a5a5a5u;
inline constexpr uint32_t kStackPaintMarginWords = 16;

SOUNDSINTH_NOINLINE inline void stack_paint_below_sp(uint32_t* bottom) {
    uint32_t* sp;
    pico_default_asm_volatile("mov %0, sp" : "=r"(sp));
    for (uint32_t* p = bottom; p < sp - kStackPaintMarginWords; ++p) {
        *p = kStackPattern;
    }
}

// Сколько байт от дна узор ещё цел - запас стека за всё время с заливки.
inline uint32_t stack_unpainted_bytes(const uint32_t* bottom, const uint32_t* top) {
    const uint32_t* p = bottom;
    while (p < top && *p == kStackPattern) {
        ++p;
    }
    return static_cast<uint32_t>(p - bottom) * sizeof(uint32_t);
}

} // namespace rp2350
