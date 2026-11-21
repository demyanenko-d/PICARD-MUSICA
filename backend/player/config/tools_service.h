// SPDX-License-Identifier: MIT
#pragma once

// Раздел TOOLS конфигуратора, сторона Core1: экраны сведений и проверка
// дисков. Карта и флешка живут на Core1.

#include <cstdint>

#include "core/memory/track_memory.h"

namespace player::config {

// Обслужить экран инструмента. Зовётся из цикла Core1 на каждом обороте,
// только в режиме конфигуратора; долгая работа идёт шагами.
void tools_serve(uint8_t* page, soundsinth::memory::TrackScratch& scratch);

} // namespace player::config
