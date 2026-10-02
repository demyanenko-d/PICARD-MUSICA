// SPDX-License-Identifier: MIT
#include "core/memory/psram_store.h"

#include <cstring>

#include "platform/memory.h"
#include "core/memory/arena.h"

namespace soundsinth::memory {

namespace {
// Отдать все страницы и взять блок трека заново - на весь чип, доставшийся
// треку. Данные растут снизу блока, временное загрузчика - сверху вниз.
void take_track_block(PsramStore& store) {
    const uint16_t pages = static_cast<uint16_t>(store.track_bytes / kPsramPageBytes);
    // Отмеченные блоки остаются на своих страницах и при смене трека, и при
    // ужатии хранилища под таблицы банка: кэш носителей взят при подъёме и
    // лежит с краю, трек его не касается.
    psram_alloc_release_track(store.alloc, pages);
    store.track               = psram_alloc(store.alloc, psram_alloc_largest_run(store.alloc));
    store.pattern_bump_offset = 0;
    store.temp_floor          = store.track.bytes();
    store.free_list_broken    = false;
}
} // namespace

void psram_create(PsramStore& store) {
    // ПК: куча; RP2350: фиксированный адрес PSRAM в окне QMI XIP.
    // Берётся весь чип: указатель один, границу хранилища держит
    // track_bytes.
    store.base = platform::psram_base_acquire(kPsramChipBytes);
    take_track_block(store);
}

void psram_set_track_bytes(PsramStore& store, uint32_t bytes) {
    // Потолок - весь чип: база отображена на весь чип с самого начала.
    store.track_bytes = bytes > kPsramChipBytes ? kPsramChipBytes : bytes;
    take_track_block(store);
}

void psram_destroy(PsramStore& store) {
    platform::psram_base_release(store.base);
    store.base = nullptr;
}

void poison_words(uint8_t* base, size_t bytes) {
    constexpr uint32_t kPoison = 0xdeadbeefu;
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        std::memcpy(base + i, &kPoison, 4);
    }
}

void psram_poison_track(PsramStore& store) {
    // Данные трека - только занятая часть, остальное и так не читалось;
    // страницы сэмплов - до высшей точки, выше неё трек ничего не брал.
    poison_words(store.base + psram_track_byte_base(store), store.pattern_bump_offset);
    const uint32_t high = static_cast<uint32_t>(store.alloc.high_water) * kPsramPageBytes;
    const uint32_t from = psram_track_byte_base(store) + store.pattern_bump_offset;
    if (high > from) std::memset(store.base + from, 0xefu, high - from);
}

void psram_reset_track(PsramStore& store) {
    take_track_block(store);
}

uint32_t psram_free_list_length(const PsramStore& store) {
    uint32_t n = 0;
    for (uint16_t p = 0; p < store.alloc.page_count; ++p) {
        if ((store.alloc.busy[p >> 5] & (1u << (p & 31u))) == 0u) ++n;
    }
    return n;
}

uint32_t psram_freeze_pattern_zone(PsramStore& store) {
    // Вверх до страницы: хвост блока раздаётся страницами.
    const uint32_t kept = align_up(store.pattern_bump_offset, kPsramPageBytes) / kPsramPageBytes;
    psram_alloc_shrink(store.alloc, store.track, static_cast<uint16_t>(kept));
    store.temp_floor = store.track.bytes(); // временное загрузчика - сэмплам
    return psram_alloc_free_pages(store.alloc);
}

uint8_t* psram_take_permanent(PsramStore& store, uint32_t bytes) {
    if (bytes == 0) return nullptr;
    const uint16_t pages = static_cast<uint16_t>(align_up(bytes, kPsramPageBytes) / kPsramPageBytes);
    // Блок трека отпускается на время просьбы: иначе он занимает всё, и
    // постоянному места нет. Взятое ляжет с краю - распределитель отдаёт
    // первый подходящий прогон, - и середину чипа не разрежет.
    psram_alloc_free(store.alloc, store.track);
    PsramBlock b = psram_alloc(store.alloc, pages);
    uint8_t* p   = nullptr;
    if (b.valid()) {
        if (psram_alloc_keep(store.alloc, b)) {
            p = store.base + static_cast<uint32_t>(b.first) * kPsramPageBytes;
        } else {
            psram_alloc_free(store.alloc, b); // список отметок полон - не держать втихую
        }
    }
    take_track_block(store);
    return p;
}

void psram_reserve_track_bytes(PsramStore& store, uint32_t bytes) {
    const uint32_t pages = align_up(bytes, kPsramPageBytes) / kPsramPageBytes;
    if (pages >= store.track.pages) return;
    psram_alloc_shrink(store.alloc, store.track, static_cast<uint16_t>(pages));
    store.temp_floor = store.track.bytes();
}

uint32_t psram_pattern_alloc(PsramStore& store, uint32_t size) {
    // На 4: структуры с uint32_t компилятор копирует через LDRD, а LDRD по
    // невыровненному адресу на Cortex-M33 - HardFault (на x86 проходит молча).
    // Блок и смещение кратны 4: size влезает - влезает и округлённый.
    const uint32_t limit = store.temp_floor;
    if (store.pattern_bump_offset > limit || size > limit - store.pattern_bump_offset) return kPatternAllocFailed;
    const uint32_t aligned     = align_up(size, 4u);
    const uint32_t offset      = store.pattern_bump_offset;
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
    PsramBlock page = psram_alloc(store.alloc, 1);
    if (!page.valid() && store.temp_floor == store.track.bytes()) {
        // Свободных страниц нет, потому что блок трека ещё не усечён.
        // Временного в нём не осталось, значит усечь можно прямо сейчас -
        // это ровно то, что делает заморозка. Загрузчику она по-прежнему
        // нужна явно: у него временное живо до самого конца разбора.
        (void)psram_freeze_pattern_zone(store);
        page = psram_alloc(store.alloc, 1);
    }
    return page.valid() ? page.first : kPageChainEnd;
}

void psram_free_chain(PsramStore& store, uint16_t first_page) {
    uint16_t page = first_page;
    // По одной: страницы цепочки лежат вразнобой. Длина ограничена числом
    // страниц чипа - испорченная цепочка не зациклит.
    for (uint32_t guard = 0; page != kPageChainEnd && guard <= kMaxSamplePageCount; ++guard) {
        const uint16_t next = store.page_next[page];
        PsramBlock one{page, 1};
        psram_alloc_free(store.alloc, one);
        store.page_next[page] = kPageChainEnd;
        page                  = next;
    }
    if (store.alloc.double_free != 0) store.free_list_broken = true;
}

} // namespace soundsinth::memory
