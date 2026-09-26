#pragma once

// 6-байтная сериализация PatternCell для словаря упаковщика и
// распаковщика. Порядок байт свой, только для представления в PSRAM.
//
// Определения здесь, а не в .cpp: decode_cell зовётся на каждую активную
// ячейку строки из PatternReader::read_row, который лежит в SRAM, и вызов
// в другую единицу трансляции уводил бы тик во флеш.

#include <cstdint>

#include "core/model/song.h"

namespace soundsinth::patterns {

inline constexpr uint32_t kEncodedCellBytes = 6;

// Раскладка блока паттерна: смещение словаря, таблица смещений строк,
// строки (маска каналов и индексы в словарь), словарь. Числа 16-битные,
// младшим байтом вперёд.
inline constexpr uint32_t kDictOffsetBytes = 2;
inline constexpr uint32_t kRowOffsetBytes = 2;
inline constexpr uint32_t kRowMaskBytes = 8;
inline constexpr uint32_t kCellIndexBytes = 2;

// Где в блоке смещение строки row.
constexpr uint32_t row_offset_pos(uint32_t row) {
    return kDictOffsetBytes + row * kRowOffsetBytes;
}

inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline void write_u16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xffu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
}

// Байт 1 - младшие 8 бит номера инструмента, бит 7 байта 2 - девятый: тип
// колонки громкости занимает биты 6:0. Трекерам хватает байта (IT - 99, XM -
// 128), номер сверх 255 бывает только у .mid (инструменты банка).
inline constexpr uint32_t kVolumeTypeBits = 0x7fu;
inline constexpr uint32_t kInstrumentHighBit = 0x80u;
inline constexpr uint16_t kMaxCellInstrument = 511;
static_assert(static_cast<uint32_t>(soundsinth::model::VolumeColumnType::Offset) <= kVolumeTypeBits,
              "тип колонки громкости делит байт с девятым битом номера инструмента");

// Байт 4: тип эффекта в битах 5:0, SlideRate в битах 7:6.
inline constexpr uint32_t kEffectTypeBits = 0x3fu;
inline constexpr uint32_t kEffectRateLsb = 6;
inline constexpr uint32_t kEffectRateBits = 0x3u;

inline void encode_cell(const soundsinth::model::PatternCell& cell, uint8_t out[kEncodedCellBytes]) {
    out[0] = cell.note;
    out[1] = static_cast<uint8_t>(cell.instrument & 0xffu);
    out[2] = static_cast<uint8_t>(static_cast<uint32_t>(cell.volume.type) |
                                  ((cell.instrument >> 8) != 0 ? kInstrumentHighBit : 0u));
    out[3] = cell.volume.param;
    out[4] = static_cast<uint8_t>(static_cast<uint32_t>(cell.effect.type) |
                                  (static_cast<uint32_t>(cell.effect.rate) << kEffectRateLsb));
    out[5] = cell.effect.param;
}

inline soundsinth::model::PatternCell decode_cell(const uint8_t in[kEncodedCellBytes]) {
    soundsinth::model::PatternCell cell;
    cell.note = in[0];
    cell.instrument = static_cast<uint16_t>(in[1] | ((in[2] & kInstrumentHighBit) << 1));
    cell.volume.type = static_cast<soundsinth::model::VolumeColumnType>(in[2] & kVolumeTypeBits);
    cell.volume.param = in[3];
    cell.effect.type = static_cast<soundsinth::model::Effect>(in[4] & kEffectTypeBits);
    cell.effect.rate = static_cast<soundsinth::model::SlideRate>((in[4] >> kEffectRateLsb) & kEffectRateBits);
    cell.effect.param = in[5];
    return cell;
}

} // namespace soundsinth::patterns
