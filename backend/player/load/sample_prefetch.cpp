// SPDX-License-Identifier: MIT
#include "player/load/sample_prefetch.h"

#include "core/codec/pattern_reader.h"

namespace player::load {

using soundsinth::model::is_real_note;
using soundsinth::model::kMaxPatternChannels;
using soundsinth::model::Pattern;
using soundsinth::model::PatternCell;
using soundsinth::model::resolve_sample_index;
using soundsinth::model::Song;

namespace {

// Вставками по [begin, end): ключ - file_offset - origin с оборотом uint32,
// то есть сначала смещения от origin вперёд, затем до него.
void sort_by_key(uint16_t* indices, uint16_t begin, uint16_t end, const Song& song, uint32_t origin) {
    const auto key_of = [&](uint16_t idx) { return song.samples[idx].file_offset - origin; };
    for (uint16_t i = static_cast<uint16_t>(begin + 1); i < end; ++i) {
        const uint16_t v   = indices[i];
        const uint32_t key = key_of(v);
        uint16_t j         = i;
        while (j > begin && key_of(indices[j - 1]) > key) {
            indices[j] = indices[j - 1];
            --j;
        }
        indices[j] = v;
    }
}

// Досортировка по стратегии. ByPlayback не сортируется: порядок первого
// появления и есть заказ. ByFile: префетч [0, prefetch) и хвост
// [prefetch, count) порознь, состав префетча не меняется; хвост - от
// смещения последнего сэмпла префетча вперёд, затем с начала файла.
void sort_plan_parts(const Song& song, uint16_t* indices, uint16_t prefetch, uint16_t count, LoadOrder order) {
    if (order != LoadOrder::ByFile) return;
    sort_by_key(indices, 0, prefetch, song, 0);
    const uint32_t tail_origin = prefetch > 0 ? song.samples[indices[prefetch - 1]].file_offset : 0u;
    sort_by_key(indices, prefetch, count, song, tail_origin);
}

// Сэмпл, который звучит в ячейке. Инструмент канала переносится: нота без
// номера инструмента играет прежним инструментом канала.
// false - ноты нет, сэмпл не найден или пуст (грузить нечего).
bool sample_of_cell(const Song& song, uint16_t& last_instrument, const PatternCell& cell, uint16_t* sample) {
    if (cell.instrument != 0) last_instrument = cell.instrument;
    if (!is_real_note(cell.note)) return false;
    uint8_t resolved_note = cell.note;
    if (!resolve_sample_index(song, last_instrument, cell.note, sample, &resolved_note)) return false;
    return song.samples[*sample].length_samples != 0;
}

} // namespace

PlaybackPlan plan_playback_order(const Song& song, soundsinth::memory::PsramStore& psram, uint16_t* out_indices, uint16_t capacity,
                                 uint16_t* out_last_use_order_pos, LoadOrder order, uint16_t prefetch_positions, uint16_t* out_first_use_order_pos) {
    if (out_indices == nullptr || out_last_use_order_pos == nullptr || capacity == 0) return {};

    for (uint16_t i = 0; i < song.sample_count; ++i) {
        out_last_use_order_pos[i] = kSampleNeverUsed;
    }
    if (out_first_use_order_pos != nullptr) {
        for (uint16_t i = 0; i < song.sample_count; ++i) {
            out_first_use_order_pos[i] = kSampleNeverUsed;
        }
    }

    // Строка на стеке, 384 байта: нужна на один вызов.
    PatternCell row[kMaxPatternChannels];

    // Инструмент канала переносится через границы паттернов, как в движке:
    // нота без номера инструмента играет прежним инструментом канала.
    uint16_t last_instrument[kMaxPatternChannels] = {};

    uint16_t count              = 0;
    uint16_t playable_positions = 0;
    uint16_t prefetch_count     = 0;

    for (uint16_t pos = 0; pos < song.order_count; ++pos) {
        const uint16_t pat_index = song.order[pos];
        if (pat_index == soundsinth::model::kOrderEnd) break; // конец линейного содержимого песни
        if (pat_index == soundsinth::model::kOrderSkip || pat_index >= song.pattern_count) continue;

        const Pattern& pat = song.patterns[pat_index];
        // Строки может не быть в зоне паттернов вовсе: у .mid её делает
        // источник. Без этой ветки план пуст, и на плате трек играет тишину.
        const bool from_source = song.row_fetch != nullptr;
        if ((!from_source && pat.psram_offset == Pattern::kInvalidOffset) || pat.channel_count == 0) continue;

        // Граница префетча - по воспроизводимым позициям: маркеры и битые
        // индексы звук не задерживают и в префетч не входят.
        if (playable_positions == prefetch_positions) prefetch_count = count;
        ++playable_positions;

        const uint8_t channels = pat.channel_count < kMaxPatternChannels ? pat.channel_count : kMaxPatternChannels;
        // Читатель - на channels, а не на pat.channel_count: read_row пишет
        // столько ячеек, сколько ему сказано, а row - на kMaxPatternChannels.
        // Каналы в строке идут по порядку, остановка раньше конца безопасна.
        for (uint16_t r = 0; r < pat.row_count; ++r) {
            if (from_source) {
                for (uint8_t ch = 0; ch < channels; ++ch) {
                    row[ch] = PatternCell{};
                }
                song.row_fetch(song.row_fetch_user, pat_index, r, row, channels);
            } else {
                const soundsinth::patterns::PatternReader reader(soundsinth::memory::psram_pattern_ptr(psram, pat.psram_offset), pat.row_count, channels);
                reader.read_row(r, row);
            }
            for (uint8_t ch = 0; ch < channels; ++ch) {
                uint16_t sample_idx = 0;
                if (!sample_of_cell(song, last_instrument[ch], row[ch], &sample_idx)) continue;

                // pos растёт монотонно, поэтому присваивание и есть максимум
                // по всем вхождениям, в том числе когда паттерн стоит в order
                // несколько раз.
                const bool first_time              = out_last_use_order_pos[sample_idx] == kSampleNeverUsed;
                out_last_use_order_pos[sample_idx] = pos;
                if (first_time && out_first_use_order_pos != nullptr) {
                    out_first_use_order_pos[sample_idx] = pos;
                }
                if (first_time && count < capacity) out_indices[count++] = sample_idx;
            }
        }
    }

    // Песня короче prefetch_positions: префетч - вся песня.
    if (playable_positions <= prefetch_positions) prefetch_count = count;

    // Префетч тоже по смещению: у Wild Commander прыжок вперёд на сотни
    // килобайт портит сэмплы, до 195 КБ проходит чисто.
    sort_plan_parts(song, out_indices, prefetch_count, count, order);
    return {count, prefetch_count};
}

void plan_collect_begin(PlanCollector& pc, const Song& song, uint16_t* out_indices, uint16_t capacity, uint16_t* out_last_use_order_pos,
                        uint16_t* out_first_use_order_pos) {
    pc           = PlanCollector{};
    pc.song      = &song;
    pc.indices   = out_indices;
    pc.last_use  = out_last_use_order_pos;
    pc.first_use = out_first_use_order_pos;
    pc.capacity  = capacity;
    // Массивы - на capacity элементов: дальше не пишется, даже если
    // сэмплов в песне больше.
    const uint16_t n = song.sample_count < capacity ? song.sample_count : capacity;
    for (uint16_t i = 0; i < n; ++i) {
        if (out_last_use_order_pos != nullptr) out_last_use_order_pos[i] = kSampleNeverUsed;
        if (out_first_use_order_pos != nullptr) out_first_use_order_pos[i] = kSampleNeverUsed;
    }
}

void plan_collect_row(void* user, const PatternCell* cells, uint8_t channel_count, uint16_t order_pos) {
    PlanCollector& pc = *static_cast<PlanCollector*>(user);
    if (pc.song == nullptr || cells == nullptr || pc.indices == nullptr || pc.last_use == nullptr) return;
    const Song& song = *pc.song;

    const uint8_t channels = channel_count < kMaxPatternChannels ? channel_count : kMaxPatternChannels;
    for (uint8_t ch = 0; ch < channels; ++ch) {
        uint16_t sample_idx = 0;
        if (!sample_of_cell(song, pc.last_instrument[ch], cells[ch], &sample_idx)) continue;
        if (sample_idx >= pc.capacity) continue; // за массивами плана

        // Позиция в проходе может убывать (переход назад), поэтому крайние
        // считаются минимумом и максимумом, а не присваиванием.
        const bool first_time = pc.last_use[sample_idx] == kSampleNeverUsed;
        if (first_time || order_pos > pc.last_use[sample_idx]) pc.last_use[sample_idx] = order_pos;
        if (pc.first_use != nullptr) {
            if (first_time || order_pos < pc.first_use[sample_idx]) pc.first_use[sample_idx] = order_pos;
        }
        if (first_time && pc.count < pc.capacity) pc.indices[pc.count++] = sample_idx;
        if (pc.use_map != nullptr) use_map_mark(*pc.use_map, sample_idx, order_pos);
    }
}

UseMap use_map_layout(uint16_t sample_count, uint16_t order_count) {
    UseMap m;
    if (sample_count == 0 || order_count == 0) return m;
    while ((static_cast<uint32_t>(order_count - 1u) >> m.shift) >= kUseMapBuckets) {
        ++m.shift;
    }
    m.buckets          = static_cast<uint16_t>(((order_count - 1u) >> m.shift) + 1u);
    m.words_per_sample = static_cast<uint8_t>((m.buckets + 31u) / 32u);
    m.sample_count     = sample_count;
    return m;
}

void use_map_mark(UseMap& m, uint16_t sample, uint16_t order_pos) {
    if (m.words == nullptr || sample >= m.sample_count) return;
    const uint32_t b = static_cast<uint32_t>(order_pos) >> m.shift;
    if (b >= m.buckets) return;
    m.words[static_cast<uint32_t>(sample) * m.words_per_sample + b / 32u] |= 1u << (b % 32u);
}

uint16_t use_map_distance(const UseMap& m, uint16_t sample, uint16_t order_pos) {
    if (m.words == nullptr || sample >= m.sample_count) return kUseMapNever;
    uint32_t from = static_cast<uint32_t>(order_pos) >> m.shift;
    if (from >= m.buckets) from = m.buckets - 1u;
    const uint32_t* w = m.words + static_cast<uint32_t>(sample) * m.words_per_sample;
    // Вперёд от from до конца, затем с начала до from: по слову за шаг.
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const uint32_t begin = pass == 0 ? from : 0u;
        const uint32_t end   = pass == 0 ? m.buckets : from;
        for (uint32_t b = begin; b < end;) {
            uint32_t bits = w[b / 32u] >> (b % 32u);
            if (bits == 0u) {
                b = (b / 32u + 1u) * 32u; // пустой остаток слова - целиком
                continue;
            }
            // Сдвигом, а не счётом младших нулей: __builtin_ctz есть не у всех
            // компиляторов проекта.
            uint32_t hit = b;
            for (; (bits & 1u) == 0u; bits >>= 1) {
                ++hit;
            }
            if (hit >= end) break;
            return static_cast<uint16_t>(pass == 0 ? hit - from : m.buckets - from + hit);
        }
    }
    return kUseMapNever;
}

PlaybackPlan plan_split_prefetch(const Song& song, uint16_t* indices, uint16_t count, const uint16_t* first_use, uint16_t prefetch_positions, LoadOrder order) {
    if (indices == nullptr || first_use == nullptr) return {};
    // Обменами, без сохранения порядка: обе части всё равно сортируются
    // следом, а у ByPlayback состав важен, а не порядок внутри части.
    uint16_t w = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t fu = first_use[indices[i]];
        if (fu == kSampleNeverUsed || fu >= prefetch_positions) continue;
        const uint16_t t = indices[w];
        indices[w]       = indices[i];
        indices[i]       = t;
        ++w;
    }
    sort_plan_parts(song, indices, w, count, order);
    return {count, w};
}

} // namespace player::load
