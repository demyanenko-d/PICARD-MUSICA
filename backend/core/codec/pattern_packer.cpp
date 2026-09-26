#include "core/codec/pattern_packer.h"

#include <cstring>

#include "platform/compiler.h"
#include "core/codec/pattern_cell_codec.h"

namespace soundsinth::patterns {

uint32_t g_max_dict_bytes = 0;
uint32_t g_max_rows_bytes = 0;
uint32_t g_max_dict_cells = 0;
#if SOUNDSINTH_TEST_COUNTERS
uint32_t g_max_pattern_bytes = 0;
#endif
uint32_t g_dict_compares = 0;

PatternPacker::PatternPacker(uint8_t* buffer, uint32_t buffer_size, uint16_t row_count, uint8_t channel_count)
    : buffer_(buffer), buffer_size_(buffer_size), row_count_(row_count), channel_count_(channel_count) {
    const uint32_t header_size = row_offset_pos(row_count_);
    write_cursor_ = header_size;
    dict_cursor_ = buffer_size_;
    ok_ = header_size <= buffer_size_;
}

bool PatternPacker::cell_is_active(const soundsinth::model::PatternCell& c) {
    return c.note != soundsinth::model::kNoteNone || c.instrument != 0 || c.volume.type != soundsinth::model::VolumeColumnType::None ||
           c.effect.type != soundsinth::model::Effect::None;
}

uint8_t* PatternPacker::dict_entry(uint32_t k) {
    return buffer_ + buffer_size_ - (k + 1u) * kEncodedCellBytes;
}

uint16_t PatternPacker::dict_lookup_or_insert(const soundsinth::model::PatternCell& cell) {
    // Смещения в блоке 16-битные, буфер не больше 64 КБ: индекс kDictFull
    // недостижим.
    static_assert(65536u / kEncodedCellBytes < kDictFull, "индекс словаря в 16 бит");
    uint8_t encoded[kEncodedCellBytes];
    encode_cell(cell, encoded);

    // Линейный поиск от новых записей к старым, запись сравнивается словом
    // и полусловом, а не memcmp: вызов memcmp через винир с проверкой
    // выравнивания стоит около 40 тактов на запись. По две записи за
    // проход: несовпадение младшего слова - прямой путь без перехода.
    // Записи уникальны, поэтому найденный индекс тот же, что при побайтовом
    // сравнении. Запись выровнена на 2, а не на 4: читается memcpy.
    static_assert(kEncodedCellBytes == 6, "сравнение ниже - слово и полуслово");
    uint32_t key_lo;
    uint16_t key_hi;
    std::memcpy(&key_lo, encoded, 4);
    std::memcpy(&key_hi, encoded + 4, 2);
    const uint8_t* const first = buffer_ + dict_cursor_;
    const uint8_t* const end = first + dict_count_ * kEncodedCellBytes;
    const uint8_t* entry = first;
    auto hi_matches = [key_hi](const uint8_t* e) {
        uint16_t hi;
        std::memcpy(&hi, e + 4, 2);
        return hi == key_hi;
    };
    const uint8_t* found = nullptr;
    for (; end - entry >= static_cast<ptrdiff_t>(2 * kEncodedCellBytes); entry += 2 * kEncodedCellBytes) {
        uint32_t a, b;
        std::memcpy(&a, entry, 4);
        std::memcpy(&b, entry + kEncodedCellBytes, 4);
        if (SOUNDSINTH_UNLIKELY(a == key_lo) && hi_matches(entry)) { found = entry; break; }
        if (SOUNDSINTH_UNLIKELY(b == key_lo) && hi_matches(entry + kEncodedCellBytes)) { found = entry + kEncodedCellBytes; break; }
    }
    if (found == nullptr && entry != end) {
        uint32_t a;
        std::memcpy(&a, entry, 4);
        if (a == key_lo && hi_matches(entry)) found = entry;
    }
    if (found != nullptr) {
        const uint32_t m = static_cast<uint32_t>(found - first) / kEncodedCellBytes;
        g_dict_compares += m + 1;
        return static_cast<uint16_t>(dict_count_ - 1 - m);
    }
    g_dict_compares += dict_count_;

    if (dict_cursor_ < write_cursor_ + kEncodedCellBytes) return kDictFull;
    dict_cursor_ -= kEncodedCellBytes;
    std::memcpy(buffer_ + dict_cursor_, encoded, kEncodedCellBytes);
    return dict_count_++;
}

bool PatternPacker::add_row(const soundsinth::model::PatternCell* cells) {
    if (!ok_ || rows_written_ >= row_count_) {
        ok_ = false;
        return false;
    }

    uint64_t mask = 0;
    uint32_t active_count = 0;
    for (uint8_t ch = 0; ch < channel_count_; ++ch) {
        if (cell_is_active(cells[ch])) {
            mask |= (uint64_t{1} << ch);
            ++active_count;
        }
    }
    const uint32_t table_pos = row_offset_pos(rows_written_);

    // Пустые строки паттерна делят одно тело: читатель идёт только по
    // таблице смещений. У .mid пустых строк около половины (19% зоны
    // паттернов), у трекерных форматов 14-15%.
    if (active_count == 0 && empty_row_offset_ != 0) {
        write_u16(buffer_ + table_pos, empty_row_offset_);
        ++rows_written_;
        return true;
    }

    const uint32_t required = kRowMaskBytes + active_count * kCellIndexBytes;

    if (write_cursor_ + required > dict_cursor_) {
        ok_ = false;
        return false;
    }

    // Место под всю строку (маска и индексы) резервируется до вставок в
    // словарь, иначе вставка откусила бы ещё не записанное место строки.
    const uint32_t row_start = write_cursor_;
    write_cursor_ = row_start + required;
    if (active_count == 0) empty_row_offset_ = row_start;

    write_u16(buffer_ + table_pos, row_start);
    if (!write_row_body(row_start, mask, cells)) {
        ok_ = false;
        return false;
    }
    ++rows_written_;
    return true;
}

bool PatternPacker::write_row_body(uint32_t row_start, uint64_t mask, const soundsinth::model::PatternCell* cells) {
    for (uint32_t b = 0; b < kRowMaskBytes; ++b) {
        buffer_[row_start + b] = static_cast<uint8_t>((mask >> (b * 8)) & 0xffu);
    }
    uint32_t idx_pos = row_start + kRowMaskBytes;
    for (uint8_t ch = 0; ch < channel_count_; ++ch) {
        if (!(mask & (uint64_t{1} << ch))) continue;
        const uint16_t idx = dict_lookup_or_insert(cells[ch]);
        if (idx == kDictFull) return false;
        write_u16(buffer_ + idx_pos, idx);
        idx_pos += kCellIndexBytes;
    }
    return true;
}

uint32_t PatternPacker::finish(memory::PsramStore& psram) {
    if (!ok_) {
        return memory::kPatternAllocFailed;
    }

    const uint32_t dict_bytes = buffer_size_ - dict_cursor_;

    // Записи лежат от новых к старым, индексы в строках - в порядке
    // вставки: после разворота index*6 от начала словаря - нужная запись.
    for (uint32_t k = 0; k < dict_count_ / 2u; ++k) {
        uint8_t* a = dict_entry(k);
        uint8_t* b = dict_entry(dict_count_ - 1u - k);
        uint8_t tmp[kEncodedCellBytes];
        std::memcpy(tmp, a, kEncodedCellBytes);
        std::memcpy(a, b, kEncodedCellBytes);
        std::memcpy(b, tmp, kEncodedCellBytes);
    }

    const uint32_t dict_offset = write_cursor_; // от начала паттерна
    std::memmove(buffer_ + write_cursor_, buffer_ + dict_cursor_, dict_bytes);

    write_u16(buffer_, dict_offset);

    // Словарь меряется отдельно: строки могли бы писаться в PSRAM сразу,
    // словарю нужен произвольный доступ в SRAM.
    if (dict_bytes > g_max_dict_bytes) g_max_dict_bytes = dict_bytes;
    if (write_cursor_ > g_max_rows_bytes) g_max_rows_bytes = write_cursor_;
    if (dict_count_ > g_max_dict_cells) g_max_dict_cells = dict_count_;

    const uint32_t total_size = dict_offset + dict_bytes;
#if SOUNDSINTH_TEST_COUNTERS
    if (total_size > g_max_pattern_bytes) g_max_pattern_bytes = total_size;
#endif
    const uint32_t offset = memory::psram_pattern_alloc(psram, total_size);
    if (offset == memory::kPatternAllocFailed) {
        return offset;
    }
    std::memcpy(memory::psram_pattern_ptr(psram, offset), buffer_, total_size);
    return offset;
}

} // namespace soundsinth::patterns
