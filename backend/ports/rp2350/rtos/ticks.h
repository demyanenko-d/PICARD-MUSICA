// SPDX-License-Identifier: MIT
#pragma once

// Системная частота платы и тик FreeRTOS. Рядом с FreeRTOSConfig.h: числа
// отсюда сверяются с configCPU_CLOCK_HZ статической проверкой ниже, и
// разъехаться им нельзя.

#include <cstdint>

#include "FreeRTOS.h"
#include "task.h"

namespace rp2350 {

// Системная частота - она же configCPU_CLOCK_HZ: от неё тактуется SysTick.
inline constexpr uint32_t kSysClockKhz = 300000;
static_assert(configCPU_CLOCK_HZ == kSysClockKhz * 1000u, "the system clock disagrees with configCPU_CLOCK_HZ");

// Тик FreeRTOS, 500 мкс. Паузы задач - в тиках отсюда.
inline constexpr uint32_t kRtosTickUs = 1000000u / configTICK_RATE_HZ;
static_assert(kRtosTickUs == 500u, "the FreeRTOS tick is 500 us: task delays are counted in it");

// С округлением вверх, как platform::ticks_for_ms: срок не выходит раньше
// названного.
inline constexpr TickType_t ticks_for_us(uint32_t us) {
    return (us + kRtosTickUs - 1u) / kRtosTickUs;
}

} // namespace rp2350
