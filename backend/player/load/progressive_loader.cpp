#include "player/load/progressive_loader.h"

#include "core/bank/bank_reader.h"
#include "player/load/sample_prefetch.h"
#include "core/memory/psram_store.h"
#include "core/memory/sample_cache_catalog.h"

namespace player::load {

namespace {

constexpr uint16_t kRingMask = kProgressiveMaxSamples - 1u;

// Demand: дольше всех не звучавший из тех, кого не держит голос.
bool evict_least_recent(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, const SamplesInUse& in_use) {
    soundsinth::memory::SampleCacheEntry* victim = nullptr;
    uint16_t oldest_age = 0;
    const uint16_t end = mem.sample_cache.used_end.load(std::memory_order_relaxed);
    for (uint16_t i = 0; i < end; ++i) {
        soundsinth::memory::SampleCacheEntry& e = mem.sample_cache.entries[i];
        const uint16_t idx = soundsinth::memory::sample_cache_slot_index(e);
        if (idx >= kProgressiveMaxSamples) continue;
        if (in_use.is_busy(idx)) continue;
        const uint16_t last = pl.plan_last_use[idx];
        // Ни разу не отмечен - старше всех.
        const uint16_t age = last == kSampleNeverUsed ? 0xffffu : static_cast<uint16_t>(pl.clock - last);
        if (victim == nullptr || age > oldest_age) {
            victim = &e;
            oldest_age = age;
        }
    }
    if (victim == nullptr) return false;
    soundsinth::memory::sample_cache_evict(mem.sample_cache, mem.psram, victim);
    ++pl.evicted;
    return true;
}

} // namespace

void progressive_start_demand(ProgressiveLoader& pl) {
    pl.source = ProgressiveSource::Demand;
    pl.plan_next = 0;
    pl.plan_count = 0;
    pl.clock = 0;
    pl.waiting_min_first_use = kSampleNeverUsed;
    for (uint16_t i = 0; i < kProgressiveMaxSamples; ++i) {
        pl.plan_last_use[i] = kSampleNeverUsed;
        pl.plan_first_use[i] = kSampleNeverUsed;
    }
}

bool progressive_request(ProgressiveLoader& pl, uint16_t sample_index) {
    if (pl.source != ProgressiveSource::Demand || sample_index >= kProgressiveMaxSamples) return false;
    if (pl.plan_first_use[sample_index] != kSampleNeverUsed) return true;
    if (static_cast<uint16_t>(pl.plan_count - pl.plan_next) >= kProgressiveMaxSamples) return false;
    pl.plan_indices[pl.plan_count & kRingMask] = sample_index;
    ++pl.plan_count;
    pl.plan_first_use[sample_index] = 0; // в кольце
    return true;
}

void progressive_note_in_use(ProgressiveLoader& pl, const soundsinth::memory::TrackMemory& mem, const SamplesInUse& in_use,
                             uint16_t now) {
    pl.clock = now;
    const uint16_t end = mem.sample_cache.used_end.load(std::memory_order_relaxed);
    for (uint16_t i = 0; i < end; ++i) {
        const uint16_t idx = soundsinth::memory::sample_cache_slot_index(mem.sample_cache.entries[i]);
        if (idx >= kProgressiveMaxSamples) continue;
        if (in_use.is_busy(idx)) pl.plan_last_use[idx] = now;
    }
}

// Plan: вытесняется сэмпл, чья последняя позиция пройдена, если его не держит
// голос (голос переживает last_use на релизе и хвосте NNA). Цепочку PCM,
// общую с другой записью (.mid), sample_cache_evict не освобождает.
bool progressive_evict_one(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos,
                           const SamplesInUse& in_use) {
    if (pl.no_eviction) return false;
    if (pl.source == ProgressiveSource::Demand) return evict_least_recent(pl, mem, in_use);
    if (pl.plan_count == 0 || order_pos == 0) return false;
    for (auto& e : mem.sample_cache.entries) {
        const uint16_t idx = soundsinth::memory::sample_cache_slot_index(e);
        static_assert(soundsinth::memory::kSampleCacheFreeSlot >= kProgressiveMaxSamples, "свободный слот отсекается границей");
        if (idx >= kProgressiveMaxSamples) continue;
        const uint16_t last = pl.plan_last_use[idx];
        if (last == kSampleNeverUsed || last >= order_pos) continue;
        if (in_use.is_busy(idx)) continue;
        soundsinth::memory::sample_cache_evict(mem.sample_cache, mem.psram, &e);
        ++pl.evicted;
        return true;
    }
    return false;
}

ProgressiveStep progressive_load_next(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos,
                                      const SamplesInUse& in_use, ProgressiveLoadFn load, void* user,
                                      uint16_t* loaded_index, const char** reason_out) {
    const bool demand = pl.source == ProgressiveSource::Demand;
    if (demand) {
        if (pl.plan_next == pl.plan_count) return ProgressiveStep::Waiting;
    } else if (pl.plan_next >= pl.plan_count) {
        return ProgressiveStep::Done;
    }

    // Упреждение - только когда памяти мало: иначе оно лишь растягивает
    // загрузку на весь трек.
    // Несвоевременный пропускается, а не держит очередь: в ByFile план идёт
    // по смещению, а не по первой ноте.
    const uint32_t free_pages = soundsinth::memory::psram_free_page_count(mem.psram);
    const bool memory_is_ample = free_pages > (mem.psram.sample_page_count / kAmpleFreeDivisor);
    if (!demand && pl.lead_positions != 0 && !memory_is_ample) {
        const uint16_t horizon = static_cast<uint16_t>(order_pos + pl.lead_positions);
        // Тот же план и та же голова: все оставшиеся не раньше запомненного.
        if (pl.waiting_min_first_use != kSampleNeverUsed && pl.waiting_plan_next == pl.plan_next &&
            horizon < pl.waiting_min_first_use) {
            return ProgressiveStep::Waiting;
        }
        uint16_t pick = pl.plan_next;
        uint16_t min_first = kSampleNeverUsed;
        while (pick < pl.plan_count) {
            const uint16_t idx_peek = pl.plan_indices[pick];
            if (idx_peek >= kProgressiveMaxSamples) break;
            const uint16_t first = pl.plan_first_use[idx_peek];
            if (first == kSampleNeverUsed || first <= horizon) break;
            if (first < min_first) min_first = first;
            ++pick;
        }
        if (pick >= pl.plan_count) {
            pl.waiting_min_first_use = min_first;
            pl.waiting_plan_next = pl.plan_next;
            return ProgressiveStep::Waiting;
        }
        pl.waiting_min_first_use = kSampleNeverUsed;
        if (pick != pl.plan_next) {
            const uint16_t tmp = pl.plan_indices[pick];
            pl.plan_indices[pick] = pl.plan_indices[pl.plan_next];
            pl.plan_indices[pl.plan_next] = tmp;
        }
    }

    uint16_t idx;
    if (demand) {
        idx = pl.plan_indices[pl.plan_next & kRingMask];
        ++pl.plan_next;
        pl.plan_first_use[idx] = kSampleNeverUsed; // вышел из кольца
        // Запрошенный - свежий: вытеснение не выберет его первым.
        pl.plan_last_use[idx] = pl.clock;
    } else {
        idx = pl.plan_indices[pl.plan_next++];
    }
    if (loaded_index) *loaded_index = idx;
    if (soundsinth::memory::sample_cache_find(mem.sample_cache, idx) != nullptr) return ProgressiveStep::Loaded;

    const char* why = "?";
    bool ok = load(user, idx, &why);
    // Не влезло - освободить отыгравшее и попробовать снова, по одному. Банк
    // не читается с карты - вытеснение не поможет.
    // Повтор - только когда вытеснение что-то дало: страниц стало больше или
    // отказ был по каталогу (слот освобождается и без страниц). Вытеснение
    // записи на общей цепочке PCM (.mid) страниц не возвращает, и повтор при
    // тех же свободных страницах отказал бы там же, распаковав прогон ещё раз.
    for (uint16_t tries = 0; !ok && why != soundsinth::bank::kBankReadError && tries < kMaxEvictRetries; ++tries) {
        const uint32_t free_before = soundsinth::memory::psram_free_page_count(mem.psram);
        if (!progressive_evict_one(pl, mem, order_pos, in_use)) break;
        if (why != soundsinth::memory::kSampleCatalogFull && soundsinth::memory::psram_free_page_count(mem.psram) == free_before) continue;
        ok = load(user, idx, &why);
    }
    if (reason_out) *reason_out = ok ? nullptr : why;
    if (!ok) {
        ++pl.plan_failed;
        return ProgressiveStep::Failed;
    }
    return ProgressiveStep::Loaded;
}

} // namespace player::load
