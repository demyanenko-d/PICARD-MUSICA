// SPDX-License-Identifier: MIT
#pragma once

// Распаковщик, симметричный PatternPacker. Движок читает им строки прямо
// из PSRAM.

#include <cstdint>

#include "core/model/song.h"

namespace soundsinth::patterns {

class PatternReader {
public:
    // data - начало сжатого блока паттерна в PSRAM (PatternPacker::finish).
    // Блок описывает себя сам (таблица смещений строк и смещение словаря в
    // начале), общий размер читателю не нужен.
    PatternReader(const uint8_t* data, uint16_t row_count, uint8_t channel_count) : data_(data), row_count_(row_count), channel_count_(channel_count) {}

    // Пишет ровно channel_count_ ячеек в cells_out (неактивные каналы -
    // PatternCell по умолчанию, пусто).
    void read_row(uint16_t row, soundsinth::model::PatternCell* cells_out) const;

private:
    const uint8_t* data_;
    uint16_t row_count_;
    uint8_t channel_count_;
};

} // namespace soundsinth::patterns
