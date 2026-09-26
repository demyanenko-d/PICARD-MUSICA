#include "core/memory/sample_cache_catalog.h"

#include <atomic>

#include "platform/hot_path.h"

namespace soundsinth::memory {

void sample_cache_reset(SampleCacheCatalog& catalog) {
    for (auto& e : catalog.entries) {
        e.sample_index.store(kSampleCacheFreeSlot, std::memory_order_relaxed);
        e.first_page = 0;
        e.checkpoint_first_page = kPageChainEnd;
    }
    catalog.used_end.store(0, std::memory_order_relaxed);
}

// В SRAM: зовётся из тика движка на каждой ноте.
SampleCacheEntry* SOUNDSINTH_HOT_PATH(sample_cache_find)(SampleCacheCatalog& catalog, uint16_t sample_index) {
    // По указателю, а не по индексу: цикл без пересчёта адреса записи.
    SampleCacheEntry* e = catalog.entries;
    SampleCacheEntry* const end = e + catalog.used_end.load(std::memory_order_acquire);
    for (; e != end; ++e) {
        if (e->sample_index.load(std::memory_order_relaxed) == sample_index) {
            // Парный к release в sample_cache_alloc_slot: виден ключ - видны и поля.
            std::atomic_thread_fence(std::memory_order_acquire);
            return e;
        }
    }
    return nullptr;
}

SampleCacheEntry* sample_cache_alloc_slot(SampleCacheCatalog& catalog, uint16_t sample_index, uint16_t first_page,
                                           uint16_t checkpoint_first_page) {
    for (uint32_t i = 0; i < kSampleCacheCatalogCapacity; ++i) {
        SampleCacheEntry& e = catalog.entries[i];
        if (e.sample_index.load(std::memory_order_relaxed) == kSampleCacheFreeSlot) {
            // Ключ последним, после барьера: иначе sample_cache_find на другом ядре
            // увидит ключ с first_page вытесненного сэмпла и уведёт голос по чужой цепочке.
            e.first_page = first_page;
            e.checkpoint_first_page = checkpoint_first_page;
            e.sample_index.store(sample_index, std::memory_order_release);
            if (i + 1 > catalog.used_end.load(std::memory_order_relaxed)) {
                catalog.used_end.store(static_cast<uint16_t>(i + 1), std::memory_order_release);
            }
            return &e;
        }
    }
    return nullptr;
}

void sample_cache_free_slot(SampleCacheCatalog& /*catalog*/, SampleCacheEntry* entry) {
    // Барьер после снятия ключа: освобождение страниц не переставится до него.
    entry->sample_index.store(kSampleCacheFreeSlot, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

void sample_cache_evict(SampleCacheCatalog& catalog, PsramStore& psram, SampleCacheEntry* entry) {
    const uint16_t first_page = entry->first_page;
    const bool shared_chain = sample_cache_chain_shared(catalog, entry);
    sample_cache_free_slot(catalog, entry);
    if (!shared_chain && first_page != kPageChainEnd) psram_free_chain(psram, first_page);
}

uint32_t sample_cache_used_count(const SampleCacheCatalog& catalog) {
    uint32_t n = 0;
    for (const auto& e : catalog.entries) {
        if (sample_cache_slot_index(e) != kSampleCacheFreeSlot) ++n;
    }
    return n;
}

bool sample_cache_chain_shared(const SampleCacheCatalog& catalog, const SampleCacheEntry* entry) {
    if (entry == nullptr || entry->first_page == kPageChainEnd) return false;
    const uint32_t end = catalog.used_end.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < end; ++i) {
        const SampleCacheEntry& e = catalog.entries[i];
        if (&e == entry) continue;
        if (e.sample_index.load(std::memory_order_relaxed) == kSampleCacheFreeSlot) continue;
        if (e.first_page == entry->first_page) return true;
    }
    return false;
}

} // namespace soundsinth::memory
