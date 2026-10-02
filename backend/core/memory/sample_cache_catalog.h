// SPDX-License-Identifier: MIT
#pragma once

// Каталог резидентных сэмплов трека. Ёмкость - с запасом над максимумом
// сэмплов банка на файл .mid по корпусу (476). Кого вытеснять, решает вызывающий.

#include <atomic>
#include <cstdint>

#include "core/memory/psram_store.h"

namespace soundsinth::memory {

inline constexpr uint16_t kSampleCacheCatalogCapacity = 512;
inline constexpr uint16_t kSampleCacheFreeSlot        = 0xffffu;

// Причина отказа загрузки сэмпла "в каталоге нет слота": одна строка на все
// загрузчики, вызывающий сравнивает указатель. Отличает отказ, который лечит
// вытеснение записи без страниц, от нехватки PSRAM.
inline constexpr char kSampleCatalogFull[] = "no room in the sample catalog";

struct SampleCacheEntry {
    // Индекс в Song::samples[], kSampleCacheFreeSlot - слот свободен. Ключ
    // публикации: пишется последним (sample_cache_alloc_slot), читатель на
    // другом ядре - sample_cache_find.
    std::atomic<uint16_t> sample_index{kSampleCacheFreeSlot};
    uint16_t first_page = 0; // первая страница цепочки в зоне сэмплов PsramStore
    // Первая страница контрольных точек Dpcm8, в той же цепочке после данных; kPageChainEnd - точек нет.
    uint16_t checkpoint_first_page = kPageChainEnd;
};
// Каждые 2 байта записи - 1 КБ SRAM на kSampleCacheCatalogCapacity слотах.
static_assert(sizeof(SampleCacheEntry) == 6, "the catalog entry has grown");

struct SampleCacheCatalog {
    SampleCacheEntry entries[kSampleCacheCatalogCapacity];
    // Выше отметки занятых слотов нет, поиск идёт только до неё. Растёт только в
    // загрузке (Core1, release после ключа); устаревшее значение у читателя
    // прячет свежую запись - нота молчит до следующей.
    std::atomic<uint16_t> used_end{0};
};

void sample_cache_reset(SampleCacheCatalog& catalog);

// nullptr, если сэмпл сейчас не резидентен.
SampleCacheEntry* sample_cache_find(SampleCacheCatalog& catalog, uint16_t sample_index);

// Занимает первый свободный слот; nullptr, если каталог полон (заняты
// все kSampleCacheCatalogCapacity). Вытеснение - забота вызывающего.
SampleCacheEntry* sample_cache_alloc_slot(SampleCacheCatalog& catalog, uint16_t sample_index, uint16_t first_page,
                                          uint16_t checkpoint_first_page = kPageChainEnd);

// Снимает ключ; страницы вызывающий освобождает только после этого.
// Читателя, который ключ уже увидел (между sample_cache_find и чтением
// first_page), барьер не защищает: сэмпл, который ещё играет или вот-вот
// заиграет, вытеснять нельзя - это держит вызывающий.
void sample_cache_free_slot(SampleCacheCatalog& catalog, SampleCacheEntry* entry);

// Вытеснить сэмпл: снять слот и вернуть цепочку страниц, если её не делит
// другой слот.
void sample_cache_evict(SampleCacheCatalog& catalog, PsramStore& psram, SampleCacheEntry* entry);

// Индекс сэмпла слота или kSampleCacheFreeSlot; для обхода вне модуля.
inline uint16_t sample_cache_slot_index(const SampleCacheEntry& e) {
    return e.sample_index.load(std::memory_order_relaxed);
}

// Занятых слотов.
uint32_t sample_cache_used_count(const SampleCacheCatalog& catalog);

// Делит ли цепочку страниц другой слот. У .mid записи одного прогона PCM
// (ослабление, панорама, строй) живут на одной цепочке; общую освобождать нельзя.
bool sample_cache_chain_shared(const SampleCacheCatalog& catalog, const SampleCacheEntry* entry);

} // namespace soundsinth::memory
