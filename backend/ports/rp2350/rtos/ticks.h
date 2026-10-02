// SPDX-License-Identifier: MIT
#pragma once

// Системная частота платы и тик FreeRTOS. Рядом с FreeRTOSConfig.h: числа
// отсюда сверяются с configCPU_CLOCK_HZ статической проверкой ниже, и
// разъехаться им нельзя.

#include <cstdint>

#include "FreeRTOS.h"
#include "task.h"

namespace rp2350 {

// Системная частота. configCPU_CLOCK_HZ вдвое меньше: тик FreeRTOS 0.5 мс.
// Смена частоты - ошибка сборки, пока не решено, что делать с тиком.
inline constexpr uint32_t kSysClockKhz = 300000;
static_assert(configCPU_CLOCK_HZ * 2 == kSysClockKhz * 1000u, "the system clock disagrees with configCPU_CLOCK_HZ: decide what to do with the FreeRTOS tick");

// Тик FreeRTOS: SysTick считает от configCPU_CLOCK_HZ, а ядро вдвое
// быстрее - тик 500 мкс, а не 1000. Паузы задач - в тиках отсюда.
inline constexpr uint32_t kRtosTickUs = (configCPU_CLOCK_HZ / 1000u) * (1000000u / configTICK_RATE_HZ) / kSysClockKhz;
// По этому же числу platform/os.h переводит миллисекунды в тики. Разойдутся
// - паузы задач станут вдвое короче названного молча.
static_assert(SOUNDSINTH_OS_TICK_US == kRtosTickUs, "the tick length in FreeRTOSConfig.h disagrees with the value computed from the system clock");

inline constexpr TickType_t ticks_for_us(uint32_t us) {
    return us / kRtosTickUs;
}

} // namespace rp2350
