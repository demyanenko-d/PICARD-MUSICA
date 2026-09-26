#include "core/codec/pattern_reader.h"

#include "platform/hot_path.h"
#include "core/codec/pattern_cell_codec.h"

namespace soundsinth::patterns {

namespace {
uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
} // namespace

// В SRAM: зовётся из тика секвенсора (read_row_and_scan, тоже в SRAM), и
// выборка кода из флеша на каждой строке конкурировала бы за QMI с
// чтением сэмплов.
SOUNDSINTH_HOT_PATH_ATTR("pattern_read_row")
void PatternReader::read_row(uint16_t row, soundsinth::model::PatternCell* cells_out) const {
    if (row >= row_count_) {
        for (uint32_t ch = 0; ch < channel_count_; ++ch) cells_out[ch] = soundsinth::model::PatternCell{};
        return;
    }

    const uint8_t* dict = data_ + read_u16(data_);
    const uint8_t* p = data_ + read_u16(data_ + row_offset_pos(row));
    // Маска 64 бита - двумя словами: 64-битный сдвиг на Cortex-M33 - семь
    // команд на канал.
    static_assert(kRowMaskBytes == 8, "маска - два слова");
    const uint32_t mask_lo = read_u32(p);
    const uint32_t mask_hi = read_u32(p + 4);
    p += kRowMaskBytes;

    for (uint32_t ch = 0; ch < channel_count_; ++ch) {
        const uint32_t bits = ch < 32 ? mask_lo : mask_hi;
        if (((bits >> (ch & 31u)) & 1u) == 0) {
            cells_out[ch] = soundsinth::model::PatternCell{};
            continue;
        }
        const uint16_t idx = read_u16(p);
        p += kCellIndexBytes;
        cells_out[ch] = decode_cell(dict + static_cast<uint32_t>(idx) * kEncodedCellBytes);
    }
}

} // namespace soundsinth::patterns
