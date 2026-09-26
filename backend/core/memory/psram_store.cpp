#include "core/memory/psram_store.h"

#include <cstring>

#include "platform/memory.h"
#include "core/memory/arena.h"

namespace soundsinth::memory {

namespace {
void rebuild_free_list(PsramStore& store) {
    // По фактическому числу страниц: граница зон подвижная.
    store.free_page_count = static_cast<uint16_t>(store.sample_page_count);
    store.free_list_broken = false;
    if (store.sample_page_count == 0) {
        store.free_list_head = kPageChainEnd;
        return;
    }
    for (uint32_t i = 0; i + 1 < store.sample_page_count; ++i) {
        store.page_next[i] = static_cast<uint16_t>(i + 1);
    }
    store.page_next[store.sample_page_count - 1] = kPageChainEnd;
    store.free_list_head = 0;
}

void reset_zones(PsramStore& store) {
    store.pattern_bump_offset = 0;
    store.temp_floor = store.track_bytes;
    store.sample_zone_base = kPatternZoneBytes;
    // Хранилище меньше зоны паттернов (pc_player --psram-kb) - сэмплам места нет.
    store.sample_page_count =
        store.track_bytes > kPatternZoneBytes ? (store.track_bytes - kPatternZoneBytes) / kPsramPageBytes : 0;
    rebuild_free_list(store);
}
} // namespace

void psram_create(PsramStore& store) {
    // ПК: куча; RP2350: фиксированный адрес PSRAM в окне QMI XIP.
    // Берётся весь чип: указатель один, границу хранилища держит
    // track_bytes.
    store.base = platform::psram_base_acquire(kPsramChipBytes);
    reset_zones(store);
}

void psram_set_track_bytes(PsramStore& store, uint32_t bytes) {
    // Потолок - весь чип: база отображена на весь чип с самого начала.
    store.track_bytes = bytes > kPsramChipBytes ? kPsramChipBytes : bytes;
    reset_zones(store);
}

void psram_destroy(PsramStore& store) {
    platform::psram_base_release(store.base);
    store.base = nullptr;
}

void poison_words(uint8_t* base, size_t bytes) {
    constexpr uint32_t kPoison = 0xdeadbeefu;
    for (size_t i = 0; i + 4 <= bytes; i += 4) std::memcpy(base + i, &kPoison, 4);
}

void psram_poison_track(PsramStore& store) {
    // Зона паттернов - только занятая часть, остальное и так не читалось;
    // зона сэмплов - целиком, по фактической границе прошлого трека.
    poison_words(store.base, store.pattern_bump_offset);
    if (store.sample_page_count > 0) {
        std::memset(store.base + store.sample_zone_base, 0xefu, store.sample_page_count * kPsramPageBytes);
    }
}

void psram_reset_track(PsramStore& store) {
    reset_zones(store);
}

uint32_t psram_free_list_length(const PsramStore& store) {
    uint32_t n = 0;
    uint16_t p = store.free_list_head;
    // Потолок - от зацикленного списка. По sample_page_count, а не по
    // константе: после заморозки зоны страниц больше.
    while (p != kPageChainEnd && n <= store.sample_page_count) {
        ++n;
        p = store.page_next[p];
    }
    return n;
}

uint32_t psram_freeze_pattern_zone(PsramStore& store) {
    // Вверх до страницы: зона сэмплов адресуется страницами.
    const uint32_t base = align_up(store.pattern_bump_offset, kPsramPageBytes);
    store.sample_zone_base = base;
    store.sample_page_count = (store.track_bytes - base) / kPsramPageBytes;
    store.temp_floor = store.track_bytes;   // временное загрузчика - сэмплам
    rebuild_free_list(store);
    return store.sample_page_count;
}

uint32_t psram_pattern_alloc(PsramStore& store, uint32_t size) {
    // На 4: структуры с uint32_t компилятор копирует через LDRD, а LDRD по
    // невыровненному адресу на Cortex-M33 - HardFault (на x86 проходит молча).
    // Зона и смещение кратны 4: size влезает - влезает и округлённый.
    const uint32_t limit = store.temp_floor < kPatternZoneBytes ? store.temp_floor : kPatternZoneBytes;
    if (store.pattern_bump_offset > limit || size > limit - store.pattern_bump_offset) return kPatternAllocFailed;
    const uint32_t aligned = align_up(size, 4u);
    const uint32_t offset = store.pattern_bump_offset;
    store.pattern_bump_offset += aligned;
    return offset;
}

uint32_t psram_temp_alloc(PsramStore& store, uint32_t size) {
    // На 8: в событиях и картах uint32_t, копии через LDRD.
    const uint32_t aligned = align_up(size, 8u);
    if (aligned < size || aligned > store.temp_floor) return kPatternAllocFailed;
    const uint32_t floor = (store.temp_floor - aligned) & ~7u;
    if (floor < store.pattern_bump_offset) return kPatternAllocFailed;
    store.temp_floor = floor;
    return floor;
}

uint16_t psram_alloc_page(PsramStore& store) {
    const uint16_t page = store.free_list_head;
    if (page == kPageChainEnd) {
        return kPageChainEnd;
    }
    // До заморозки сверху может лежать временное загрузчика - страницы на нём
    // не выдаются.
    if (store.sample_zone_base + (static_cast<uint32_t>(page) + 1u) * kPsramPageBytes > store.temp_floor) {
        return kPageChainEnd;
    }
    store.free_list_head = store.page_next[page];
    --store.free_page_count;
    return page;
}

void psram_free_chain(PsramStore& store, uint16_t first_page) {
    if (first_page == kPageChainEnd) {
        return;
    }
    // Найти хвост цепочки и подшить её целиком перед free_list_head:
    // O(длина цепочки). O(1) потребовало бы хранить хвост в каталоге
    // сэмплов.
    uint16_t tail = first_page;
    uint16_t length = 1;
    while (store.page_next[tail] != kPageChainEnd) {
        tail = store.page_next[tail];
        ++length;
    }
    store.page_next[tail] = store.free_list_head;
    store.free_list_head = first_page;
    store.free_page_count = static_cast<uint16_t>(store.free_page_count + length);
    if (store.free_page_count > store.sample_page_count) store.free_list_broken = true;
}

} // namespace soundsinth::memory
