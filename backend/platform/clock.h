// SPDX-License-Identifier: MIT
#pragma once

// Микросекундный счётчик для замеров занятости ядра (загрузка рендера,
// самый дорогой тик, время цикла голосов). На плате - таймер SDK: чтение
// регистра, инлайн, потому что зовётся из горячего пути в SRAM. На ПК
// замер не собирается, всегда 0: загрузка рендера управляет сбросом
// голосов, и выход тестов и pc_player от времени зависеть не должен.

#include <cstdint>

#if PICO_ON_DEVICE
#include "hardware/timer.h"
#endif

namespace platform {

// false - замеров нет. Производные от времени величины, которые можно
// задать и снаружи (загрузка рендера в тестах сброса голосов), при false
// не пересчитываются.
#if PICO_ON_DEVICE
inline constexpr bool kHasClock = true;
#else
inline constexpr bool kHasClock = false;
#endif

inline uint32_t time_us() {
#if PICO_ON_DEVICE
    return time_us_32();
#else
    return 0;
#endif
}

} // namespace platform
