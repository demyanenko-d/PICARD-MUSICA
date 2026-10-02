// SPDX-License-Identifier: MIT
#pragma once

// Произвольный доступ к потоку Dpcm8 через контрольные точки: O(страниц)
// переходов по page_next вместо O(позиция) декодов с начала сэмпла.

#include <cstdint>

#include "core/codec/dpcm8.h"
#include "core/memory/psram_store.h"

namespace soundsinth::dpcm8 {

// Позиция декодера ровно в начале блока block_index (исходный отсчёт
// block_index*kCheckpointIntervalSamples). Ничего не декодирует, только
// вычисляет, откуда продолжать.
struct BlockPosition {
    uint16_t page        = memory::kPageChainEnd;
    uint16_t byte_offset = 0;
    Dpcm8State state;
};

// Читает контрольную точку block_index из таблицы, дописанной
// упаковщиком после данных сэмпла. Точка 0 тоже в
// таблице, но всегда Dpcm8State{} (predictor = 0), поэтому locate_block
// для block_index == 0 PSRAM не читает.
Dpcm8Checkpoint read_checkpoint(memory::PsramStore& psram, uint16_t checkpoint_first_page, uint32_t block_index);

// BlockPosition для блока block_index сэмпла с first_page по точке из
// checkpoint_first_page. kPageChainEnd (точек нет) допустим только при
// block_index == 0; наличие точек проверяет вызывающий (voice_trigger).
BlockPosition locate_block(memory::PsramStore& psram, uint16_t first_page, uint16_t checkpoint_first_page, uint32_t block_index);

} // namespace soundsinth::dpcm8
