// SPDX-License-Identifier: MIT
#include "core/memory/psram_alloc.h"

#include <cstring>

namespace soundsinth::memory {

namespace {

bool page_busy(const PsramAlloc& a, uint16_t page) {
    return (a.busy[page >> 5] & (1u << (page & 31u))) != 0u;
}

void mark(PsramAlloc& a, uint16_t first, uint16_t pages, bool busy) {
    for (uint16_t i = 0; i < pages; ++i) {
        const uint16_t p = static_cast<uint16_t>(first + i);
        if (busy) {
            a.busy[p >> 5] |= (1u << (p & 31u));
        } else {
            a.busy[p >> 5] &= ~(1u << (p & 31u));
        }
    }
}

// Свободны ли pages страниц подряд с first. За page_count - заняты.
bool run_free(const PsramAlloc& a, uint16_t first, uint16_t pages) {
    if (static_cast<uint32_t>(first) + pages > a.page_count) return false;
    for (uint16_t i = 0; i < pages; ++i) {
        if (page_busy(a, static_cast<uint16_t>(first + i))) return false;
    }
    return true;
}

} // namespace

void psram_alloc_reset(PsramAlloc& a, uint16_t page_count) {
    a.page_count  = page_count > kMaxSamplePageCount ? static_cast<uint16_t>(kMaxSamplePageCount) : page_count;
    a.free_pages  = a.page_count;
    a.double_free = 0;
    a.high_water  = 0;
    std::memset(a.busy, 0, sizeof(a.busy));
    for (PsramBlock& k : a.keep) {
        k = PsramBlock{};
    }
}

PsramBlock psram_alloc(PsramAlloc& a, uint16_t pages) {
    PsramBlock block;
    if (pages == 0 || pages > a.free_pages) return block;
    // Первый подходящий: прогоны берутся с начала, поэтому долгоживущие
    // блоки, взятые при загрузке, ложатся с краю и не режут середину.
    uint16_t run = 0;
    for (uint16_t p = 0; p < a.page_count; ++p) {
        if (page_busy(a, p)) {
            run = 0;
            continue;
        }
        if (++run < pages) continue;
        const uint16_t first = static_cast<uint16_t>(p + 1u - pages);
        mark(a, first, pages, true);
        a.free_pages       = static_cast<uint16_t>(a.free_pages - pages);
        block.first        = first;
        block.pages        = pages;
        const uint16_t top = static_cast<uint16_t>(first + pages);
        if (top > a.high_water) a.high_water = top;
        return block;
    }
    return block;
}

bool psram_alloc_extend(PsramAlloc& a, PsramBlock& block, uint16_t more) {
    if (!block.valid() || more == 0) return false;
    const uint16_t next = static_cast<uint16_t>(block.first + block.pages);
    if (!run_free(a, next, more)) return false;
    mark(a, next, more, true);
    a.free_pages        = static_cast<uint16_t>(a.free_pages - more);
    block.pages         = static_cast<uint16_t>(block.pages + more);
    const uint16_t top2 = static_cast<uint16_t>(block.first + block.pages);
    if (top2 > a.high_water) a.high_water = top2;
    return true;
}

bool psram_alloc_keep(PsramAlloc& a, const PsramBlock& block) {
    if (!block.valid()) return false;
    for (PsramBlock& k : a.keep) {
        if (!k.valid()) {
            k = block;
            return true;
        }
    }
    return false;
}

void psram_alloc_release_track(PsramAlloc& a, uint16_t page_count) {
    PsramBlock kept[kPsramKeepBlocks];
    for (uint32_t i = 0; i < kPsramKeepBlocks; ++i) {
        kept[i] = a.keep[i];
    }
    const uint16_t lost_before = a.kept_lost;
    psram_alloc_reset(a, page_count);
    a.kept_lost = lost_before;
    for (uint32_t i = 0; i < kPsramKeepBlocks; ++i) {
        if (!kept[i].valid()) continue;
        // Выше новой границы - страницы уже не наши, держать отметку
        // нельзя: иначе учёт говорит "занято" о том, чего нет.
        if (static_cast<uint32_t>(kept[i].first) + kept[i].pages > a.page_count) {
            ++a.kept_lost;
            continue;
        }
        a.keep[i] = kept[i];
        mark(a, kept[i].first, kept[i].pages, true);
        a.free_pages       = static_cast<uint16_t>(a.free_pages - kept[i].pages);
        const uint16_t top = static_cast<uint16_t>(kept[i].first + kept[i].pages);
        if (top > a.high_water) a.high_water = top;
    }
}

void psram_alloc_drop_kept(PsramAlloc& a, PsramBlock& block) {
    for (PsramBlock& k : a.keep) {
        if (k.valid() && k.first == block.first && k.pages == block.pages) {
            k = PsramBlock{};
            break;
        }
    }
    psram_alloc_free(a, block);
}

void psram_alloc_shrink(PsramAlloc& a, PsramBlock& block, uint16_t pages) {
    if (!block.valid() || pages >= block.pages) return;
    if (pages == 0) {
        psram_alloc_free(a, block);
        return;
    }
    const uint16_t first_freed = static_cast<uint16_t>(block.first + pages);
    const uint16_t freed       = static_cast<uint16_t>(block.pages - pages);
    mark(a, first_freed, freed, false);
    a.free_pages = static_cast<uint16_t>(a.free_pages + freed);
    block.pages  = pages;
}

void psram_alloc_free(PsramAlloc& a, PsramBlock& block) {
    if (!block.valid()) return;
    // Свободная страница внутри возвращаемого блока значит, что блок
    // возвращают второй раз или он налез на чужой. Учёт после этого врёт.
    for (uint16_t i = 0; i < block.pages; ++i) {
        if (!page_busy(a, static_cast<uint16_t>(block.first + i))) {
            ++a.double_free;
            break;
        }
    }
    mark(a, block.first, block.pages, false);
    a.free_pages = static_cast<uint16_t>(a.free_pages + block.pages);
    block        = PsramBlock{};
}

uint16_t psram_alloc_free_pages(const PsramAlloc& a) {
    return a.free_pages;
}

uint16_t psram_alloc_largest_run(const PsramAlloc& a) {
    uint16_t best = 0;
    uint16_t run  = 0;
    for (uint16_t p = 0; p < a.page_count; ++p) {
        if (page_busy(a, p)) {
            run = 0;
            continue;
        }
        if (++run > best) best = run;
    }
    return best;
}

} // namespace soundsinth::memory
