// SPDX-License-Identifier: MIT
#include "player/load/progressive_loader.h"

#include "core/bank/bank_reader.h"
#include "core/formats/midi_live.h" // потолок сэмплов живого потока
#include "core/memory/psram_store.h"
#include "core/memory/sample_cache_catalog.h"

#include "player/load/sample_prefetch.h"

namespace player::load {

// Заказ в кольце не повторяется (сторож plan_first_use), поэтому разных
// заказов сразу не больше, чем сэмплов у живого потока. Кольцо обязано их
// вмещать: переполнение - молчаливый отказ заказа и молчащая нота.
static_assert(soundsinth::formats::midi::kLiveMaxSamples <= kDemandRingCapacity, "the request ring is smaller than the number of samples in the live stream");

namespace {

constexpr uint16_t kRingMask = kDemandRingCapacity - 1u;

// Demand держит своё в массивах плана: plan_indices - кольцо заказов,
// plan_first_use - признак "сэмпл в кольце", plan_last_use - часы, когда
// сэмпл звучал или загружен в последний раз. Доступ - только через эти
// функции, поля Plan читаются по своим именам.
constexpr uint16_t kDemandInRing = 0;

inline uint16_t& demand_ring_slot(ProgressiveLoader& pl, uint16_t n) {
    return pl.plan_indices[n & kRingMask];
}
inline bool demand_in_ring(const ProgressiveLoader& pl, uint16_t idx) {
    return pl.plan_first_use[idx] != kSampleNeverUsed;
}
inline void demand_mark_in_ring(ProgressiveLoader& pl, uint16_t idx) {
    pl.plan_first_use[idx] = kDemandInRing;
}
inline void demand_mark_out_of_ring(ProgressiveLoader& pl, uint16_t idx) {
    pl.plan_first_use[idx] = kSampleNeverUsed;
}
inline uint16_t& demand_last_heard(ProgressiveLoader& pl, uint16_t idx) {
    return pl.plan_last_use[idx];
}

// Demand: дольше всех не звучавший из тех, кого не держит голос.
bool evict_least_recent(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, const SamplesInUse& in_use) {
    soundsinth::memory::SampleCacheEntry* victim = nullptr;
    uint16_t oldest_age                          = 0;
    const uint16_t end                           = mem.sample_cache.used_end.load(std::memory_order_relaxed);
    for (uint16_t i = 0; i < end; ++i) {
        soundsinth::memory::SampleCacheEntry& e = mem.sample_cache.entries[i];
        const uint16_t idx                      = soundsinth::memory::sample_cache_slot_index(e);
        if (idx >= kProgressiveMaxSamples) continue;
        if (in_use.is_busy(idx)) {
            if (in_use.over_cap(idx)) ++pl.evict_capped;
            continue;
        }
        const uint16_t last = demand_last_heard(pl, idx);
        // Ни разу не отмечен - старше всех.
        const uint16_t age = last == kSampleNeverUsed ? 0xffffu : static_cast<uint16_t>(pl.clock - last);
        if (victim == nullptr || age > oldest_age) {
            victim     = &e;
            oldest_age = age;
        }
    }
    if (victim == nullptr) return false;
    soundsinth::memory::sample_cache_evict(mem.sample_cache, mem.psram, victim);
    ++pl.evicted;
    return true;
}

// Plan с картой позиций: жертва - тот, кто понадобится позже всех. Сэмпл,
// отыгравший до конца песни, получает расстояние через её начало (повтор),
// не звучащий вовсе - наибольшее. Жертва обязана быть нужна позже
// incoming: иначе одна молчащая нота меняется на другую, раньше.
bool evict_farthest(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos, const SamplesInUse& in_use, uint16_t incoming) {
    const uint16_t need                          = incoming < kProgressiveMaxSamples ? use_map_distance(pl.use_map, incoming, order_pos) : 0u;
    soundsinth::memory::SampleCacheEntry* victim = nullptr;
    uint16_t farthest                            = need;
    const uint16_t end                           = mem.sample_cache.used_end.load(std::memory_order_relaxed);
    for (uint16_t i = 0; i < end; ++i) {
        soundsinth::memory::SampleCacheEntry& e = mem.sample_cache.entries[i];
        const uint16_t idx                      = soundsinth::memory::sample_cache_slot_index(e);
        if (idx >= kProgressiveMaxSamples || idx == incoming) continue;
        const uint16_t d = use_map_distance(pl.use_map, idx, order_pos);
        if (d <= farthest) continue;
        if (in_use.is_busy(idx)) {
            if (in_use.over_cap(idx)) ++pl.evict_capped;
            continue;
        }
        victim   = &e;
        farthest = d;
    }
    if (victim == nullptr) return false;
    soundsinth::memory::sample_cache_evict(mem.sample_cache, mem.psram, victim);
    ++pl.evicted;
    return true;
}

} // namespace

void progressive_start_demand(ProgressiveLoader& pl) {
    pl.source     = ProgressiveSource::Demand;
    pl.use_map    = UseMap{};
    pl.plan_next  = 0;
    pl.plan_count = 0;
    pl.clock      = 0;
    for (uint16_t i = 0; i < kProgressiveMaxSamples; ++i) {
        demand_last_heard(pl, i) = kSampleNeverUsed;
        demand_mark_out_of_ring(pl, i);
    }
}

bool progressive_request(ProgressiveLoader& pl, uint16_t sample_index) {
    if (pl.source != ProgressiveSource::Demand || sample_index >= kProgressiveMaxSamples) return false;
    if (demand_in_ring(pl, sample_index)) return true;
    if (static_cast<uint16_t>(pl.plan_count - pl.plan_next) >= kDemandRingCapacity) return false;
    demand_ring_slot(pl, pl.plan_count) = sample_index;
    ++pl.plan_count;
    demand_mark_in_ring(pl, sample_index);
    return true;
}

void progressive_note_in_use(ProgressiveLoader& pl, const soundsinth::memory::TrackMemory& mem, const SamplesInUse& in_use, uint16_t now) {
    pl.clock           = now;
    const uint16_t end = mem.sample_cache.used_end.load(std::memory_order_relaxed);
    for (uint16_t i = 0; i < end; ++i) {
        const uint16_t idx = soundsinth::memory::sample_cache_slot_index(mem.sample_cache.entries[i]);
        if (idx >= kProgressiveMaxSamples) continue;
        if (in_use.is_busy(idx)) demand_last_heard(pl, idx) = now;
    }
}

// Без карты позиций: вытесняется сэмпл, чья последняя позиция пройдена и
// которого не держит голос (голос переживает last_use на релизе и хвосте
// NNA). Цепочку PCM, общую с другой записью (.mid), sample_cache_evict не
// освобождает.
bool progressive_evict_one(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos, const SamplesInUse& in_use, uint16_t incoming) {
    if (pl.no_eviction) return false;
    if (pl.source == ProgressiveSource::Demand) return evict_least_recent(pl, mem, in_use);
    if (pl.plan_count == 0) return false;
    if (pl.use_map.words != nullptr) return evict_farthest(pl, mem, order_pos, in_use, incoming);
    if (order_pos == 0) return false;
    // До отметки занятых, а не по всему массиву: выше неё слотов нет по
    // контракту каталога, а зовут это до kMaxEvictRetries раз на сэмпл.
    //
    // Начинать с места прошлой остановки (по кругу) пробовали - замером
    // хуже: жертвой становится сэмпл, который скоро понадобится снова.
    // Самый нижний подходящий слот держит тот, что загружен раньше всех и
    // вероятнее отыграл.
    const uint16_t end = mem.sample_cache.used_end.load(std::memory_order_relaxed);
    for (uint16_t i = 0; i < end; ++i) {
        soundsinth::memory::SampleCacheEntry& e = mem.sample_cache.entries[i];
        const uint16_t idx                      = soundsinth::memory::sample_cache_slot_index(e);
        static_assert(soundsinth::memory::kSampleCacheFreeSlot >= kProgressiveMaxSamples, "a free slot is cut off by the boundary");
        if (idx >= kProgressiveMaxSamples) continue;
        const uint16_t last = pl.plan_last_use[idx];
        if (last == kSampleNeverUsed || last >= order_pos) continue;
        if (in_use.is_busy(idx)) {
            if (in_use.over_cap(idx)) ++pl.evict_capped;
            continue;
        }
        soundsinth::memory::sample_cache_evict(mem.sample_cache, mem.psram, &e);
        ++pl.evicted;
        return true;
    }
    return false;
}

namespace {

// Шаг повторного прохода по плану. За вызов просматривает не больше
// kRescanPerStep записей и грузит не больше одной: сплошная сверка всего
// плана с каталогом стоила бы сотню тысяч действий, а трек уже играет.
//
// С картой позиций возвращается то, что прозвучит в пределах упреждения:
// дальнее вытеснило бы нужное раньше. Без карты - всё, чья последняя
// позиция не пройдена.
ProgressiveStep plan_rescan_step(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos, const SamplesInUse& in_use,
                                 ProgressiveLoadFn load, void* user, uint16_t* loaded_index, const char** reason_out) {
    // Ни вытеснения, ни отказа - всё из плана на месте, проходить незачем.
    if (pl.plan_count == 0 || (pl.evicted == 0 && pl.plan_failed == 0)) return ProgressiveStep::Done;
    const bool by_map     = pl.use_map.words != nullptr;
    const uint16_t window = static_cast<uint16_t>(((pl.lead_positions != 0 ? pl.lead_positions : 1u) >> pl.use_map.shift) + 1u);
    for (uint16_t n = 0; n < kRescanPerStep; ++n) {
        if (pl.plan_rescan >= pl.plan_count) pl.plan_rescan = 0;
        const uint16_t idx = pl.plan_indices[pl.plan_rescan++];
        if (idx >= kProgressiveMaxSamples) continue;
        if (by_map) {
            if (use_map_distance(pl.use_map, idx, order_pos) >= window) continue;
        } else {
            const uint16_t last = pl.plan_last_use[idx];
            if (last == kSampleNeverUsed || last < order_pos) continue; // отыгравший навсегда
        }
        if (soundsinth::memory::sample_cache_find(mem.sample_cache, idx) != nullptr) continue;

        if (loaded_index) *loaded_index = idx;
        const char* why = "?";
        if (!load(user, idx, &why)) {
            // Не влезло - одно вытеснение и одна попытка; следующий оборот
            // прохода вернётся сюда сам.
            if (!progressive_evict_one(pl, mem, order_pos, in_use, idx) || !load(user, idx, &why)) {
                if (reason_out) *reason_out = why;
                ++pl.rescan_failed;
                return ProgressiveStep::Waiting;
            }
        }
        ++pl.reloaded;
        return ProgressiveStep::Loaded;
    }
    return ProgressiveStep::Waiting;
}

// Упреждение: вывести к голове очереди сэмпл, который прозвучит не позже
// горизонта order_pos + lead_positions. false - своевременных нет (Waiting).
bool plan_pick_within_horizon(ProgressiveLoader& pl, uint16_t order_pos) {
    const uint16_t horizon = static_cast<uint16_t>(order_pos + pl.lead_positions);
    // Пропущенное пересматривается, только когда горизонт дорос до
    // самой ранней из пропущенных нот: иначе тот же префикс
    // просматривался бы заново на каждом шаге. После Waiting пропущен весь
    // хвост (lead_scan = plan_count), и до этой ноты ответ - снова Waiting
    // без просмотра.
    uint16_t pick      = pl.plan_next;
    uint16_t min_first = pl.lead_skip_min_first;
    if (pl.lead_scan > pl.plan_next && horizon < pl.lead_skip_min_first) {
        pick = pl.lead_scan < pl.plan_count ? pl.lead_scan : pl.plan_count;
    } else {
        min_first = kSampleNeverUsed;
    }
    while (pick < pl.plan_count) {
        const uint16_t idx_peek = pl.plan_indices[pick];
        if (idx_peek >= kProgressiveMaxSamples) break;
        const uint16_t first = pl.plan_first_use[idx_peek];
        if (first == kSampleNeverUsed || first <= horizon) break;
        if (first < min_first) min_first = first;
        ++pick;
    }
    if (pick >= pl.plan_count) {
        pl.lead_scan           = pl.plan_count;
        pl.lead_skip_min_first = min_first;
        return false;
    }
    // Своевременный вынимается сдвигом, а не перестановкой с головой:
    // план отсортирован по смещению в файле, и у плагина прыжок назад -
    // перечитывание файла с начала. Сдвиг сохраняет порядок хвоста, и
    // пропущенный префикс остаётся на месте относительно plan_next.
    if (pick != pl.plan_next) {
        const uint16_t idx_pick = pl.plan_indices[pick];
        for (uint16_t i = pick; i > pl.plan_next; --i) {
            pl.plan_indices[i] = pl.plan_indices[i - 1];
        }
        pl.plan_indices[pl.plan_next] = idx_pick;
    }
    // Просмотрено включая pick; после сдвига пропущенные лежат на
    // [plan_next + 1, pick], а plan_next вырастет на единицу ниже.
    pl.lead_scan           = static_cast<uint16_t>(pick + 1u);
    pl.lead_skip_min_first = min_first;
    return true;
}

// Снять с головы очереди (Plan) или кольца (Demand) следующий сэмпл.
uint16_t take_next_index(ProgressiveLoader& pl) {
    if (pl.source != ProgressiveSource::Demand) return pl.plan_indices[pl.plan_next++];
    const uint16_t idx = demand_ring_slot(pl, pl.plan_next);
    ++pl.plan_next;
    demand_mark_out_of_ring(pl, idx);
    // Запрошенный - свежий: вытеснение не выберет его первым.
    demand_last_heard(pl, idx) = pl.clock;
    return idx;
}

// Загрузить сэмпл idx; не влезло - вытеснить один сэмпл и попробовать
// снова. Банк не читается с карты - вытеснение не поможет.
// Повтор - только когда вытеснение что-то дало: страниц стало больше или
// отказ был по каталогу (слот освобождается и без страниц). Вытеснение
// записи на общей цепочке PCM (.mid) страниц не возвращает, и повтор при
// тех же свободных страницах отказал бы там же, распаковав прогон ещё раз.
ProgressiveStep load_with_eviction(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos, const SamplesInUse& in_use,
                                   ProgressiveLoadFn load, void* user, uint16_t idx, const char** reason_out) {
    const char* why = "?";
    bool ok         = load(user, idx, &why);
    for (uint16_t tries = 0; !ok && why != soundsinth::bank::kBankReadError && tries < kMaxEvictRetries; ++tries) {
        const uint32_t free_before = soundsinth::memory::psram_free_page_count(mem.psram);
        if (!progressive_evict_one(pl, mem, order_pos, in_use, idx)) break;
        if (why != soundsinth::memory::kSampleCatalogFull && soundsinth::memory::psram_free_page_count(mem.psram) == free_before) {
            continue;
        }
        ok = load(user, idx, &why);
    }
    if (reason_out) *reason_out = ok ? nullptr : why;
    if (!ok) {
        ++pl.plan_failed;
        return ProgressiveStep::Failed;
    }
    return ProgressiveStep::Loaded;
}

} // namespace

ProgressiveStep progressive_load_next(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos, const SamplesInUse& in_use,
                                      ProgressiveLoadFn load, void* user, uint16_t* loaded_index, const char** reason_out) {
    const bool demand = pl.source == ProgressiveSource::Demand;
    if (demand) {
        if (pl.plan_next == pl.plan_count) return ProgressiveStep::Waiting;
    } else if (pl.plan_next >= pl.plan_count) {
        // Очередь пройдена - повторный проход по кругу возвращает
        // вытесненное и не влезшее, если оно ещё прозвучит.
        return plan_rescan_step(pl, mem, order_pos, in_use, load, user, loaded_index, reason_out);
    }

    // Упреждение - только когда памяти мало: иначе оно лишь растягивает
    // загрузку на весь трек.
    // Несвоевременный пропускается, а не держит очередь: в ByFile план идёт
    // по смещению, а не по первой ноте.
    const uint32_t free_pages  = soundsinth::memory::psram_free_page_count(mem.psram);
    const bool memory_is_ample = free_pages > (soundsinth::memory::psram_sample_page_count(mem.psram) / kAmpleFreeDivisor);
    if (!demand && pl.lead_positions != 0 && !memory_is_ample && !plan_pick_within_horizon(pl, order_pos)) return ProgressiveStep::Waiting;

    const uint16_t idx = take_next_index(pl);
    if (loaded_index) *loaded_index = idx;
    if (soundsinth::memory::sample_cache_find(mem.sample_cache, idx) != nullptr) return ProgressiveStep::Loaded;
    return load_with_eviction(pl, mem, order_pos, in_use, load, user, idx, reason_out);
}

} // namespace player::load
