// SPDX-License-Identifier: MIT
#pragma once

// Упаковщик паттернов, сжатый формат:
//
//   [2 байта]             смещение словаря от начала паттерна
//   [rowCount x 2 байта]  смещения начала строк
//   [строки]              на строку: 8 байт маски активных каналов +
//                          N x 2 байта индексов в словарь (N = popcount);
//                          все пустые строки паттерна - одно тело
//   [словарь]             uniqueCells x 6 байт (PatternCell)
//
// Однопроходный: загрузчик вызывает add_row() на каждую строку по мере
// разбора, распакованный паттерн нигде не хранится.
//
// Без кучи и хэш-таблицы: рабочая память - только буфер упаковщика,
// растущий с двух концов - таблица смещений и строки от начала (row_count
// известен из заголовка паттерна до первой ячейки), словарь от конца.
// Дедупликация - линейный поиск, самое дорогое место загрузки, поэтому
// сравнение словом. finish() сдвигает словарь вплотную к строкам и одним
// копированием переносит блок в PSRAM.

#include <cstdint>

#include "core/model/song.h"
#include "core/memory/psram_store.h"

namespace soundsinth::patterns {

// Максимумы за прогон: сколько байт заняли словарь и строки на самом
// тяжёлом паттерне.
extern uint32_t g_max_dict_bytes;
extern uint32_t g_max_rows_bytes;
extern uint32_t g_max_dict_cells;
#if SOUNDSINTH_TEST_COUNTERS
// Весь блок паттерна (заголовок, строки, словарь) - столько занимает буфер
// упаковки. Только в сборке ПК.
extern uint32_t g_max_pattern_bytes;
#endif
// Сравнений записей словаря за прогон, накопительно: цена упаковки без цены
// сравнения, одинаковая на ПК и плате (m + 1 при попадании, весь словарь
// при промахе). Самое дорогое место загрузки паттернов.
extern uint32_t g_dict_compares;

class PatternPacker {
public:
    // buffer/buffer_size - сценарий PatternPack буфера трека
    // (kPatternPackBufferBytes). row_count/channel_count известны из
    // заголовка паттерна до первой ячейки.
    PatternPacker(uint8_t* buffer, uint32_t buffer_size, uint16_t row_count, uint8_t channel_count);

    // cells - ровно channel_count элементов строки, по каналам. Активная
    // ячейка - не полностью пустая (не note == kNoteNone && instrument == 0
    // && volume.type == None && effect.type == None). false - переполнение
    // буфера (ok()), вызывающий прекращает добавление строк.
    bool add_row(const soundsinth::model::PatternCell* cells);

    bool ok() const { return ok_; }

    // Сдвигает словарь к строкам и копирует блок в зону паттернов PSRAM.
    // kPatternAllocFailed - блок не собран (ok() == false, переполнен
    // рабочий буфер в SRAM) или переполнена зона паттернов в PSRAM; причину
    // различает ok().
    uint32_t finish(memory::PsramStore& psram);

private:
    static bool cell_is_active(const soundsinth::model::PatternCell& c);

    // Ищет cell в словаре [dict_cursor_, buffer_size_), не нашёл - вставляет
    // (словарь растёт на 6 байт к началу буфера). Возвращает индекс в
    // порядке появления; словарь не пересортировывается, поэтому индексы,
    // выданные раньше, не меняются. kDictFull - переполнение буфера.
    static constexpr uint16_t kDictFull = 0xffffu;
    uint16_t dict_lookup_or_insert(const soundsinth::model::PatternCell& cell);

    // Запись словаря с индексом вставки k: новые ближе к началу буфера.
    uint8_t* dict_entry(uint32_t k);

    // Маска и индексы строки с row_start; false - переполнение словаря.
    bool write_row_body(uint32_t row_start, uint64_t mask, const soundsinth::model::PatternCell* cells);

    uint8_t* buffer_;
    uint32_t buffer_size_;
    uint16_t row_count_;
    uint8_t channel_count_;

    uint32_t write_cursor_; // растёт вперёд от конца таблицы смещений: строки
    uint32_t dict_cursor_;  // растёт назад от buffer_size_: словарь
    uint16_t dict_count_       = 0;
    uint16_t rows_written_     = 0;
    uint32_t empty_row_offset_ = 0; // тело первой пустой строки, 0 - ещё не было (0 занят заголовком)
    bool ok_                   = true;
};

} // namespace soundsinth::patterns
