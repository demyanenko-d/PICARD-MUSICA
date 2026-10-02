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
        // источник. Без этой ветки план выходил пустым, и на плате трек играл
        // тишину - фоновой догрузке нечего было тянуть.
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
                const PatternCell& cell = row[ch];
                if (cell.instrument != 0) last_instrument[ch] = cell.instrument;
                if (!is_real_note(cell.note)) continue;

                uint16_t sample_idx   = 0;
                uint8_t resolved_note = cell.note;
                if (!resolve_sample_index(song, last_instrument[ch], cell.note, &sample_idx, &resolved_note)) continue;
                if (song.samples[sample_idx].length_samples == 0) continue; // грузить нечего

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

    // Префетч тоже по смещению: у WC битыми приходили сэмплы после прыжков
    // вперёд на сотни килобайт, прыжки до 195 КБ проходили чисто.
    // ByPlayback не сортируется: порядок первого появления и есть заказ.
    if (order != LoadOrder::ByFile) return {count, prefetch_count};

    // Префетч и хвост сортируются порознь, состав префетча не меняется.
    // Хвост - от последнего сэмпла префетча вперёд, затем с начала файла.
    sort_by_key(out_indices, 0, prefetch_count, song, 0);
    const uint32_t tail_origin = prefetch_count > 0 ? song.samples[out_indices[prefetch_count - 1]].file_offset : 0u;
    sort_by_key(out_indices, prefetch_count, count, song, tail_origin);

    return {count, prefetch_count};
}

void plan_collect_begin(PlanCollector& pc, const Song& song, uint16_t* out_indices, uint16_t capacity, uint16_t* out_last_use_order_pos,
                        uint16_t* out_first_use_order_pos, uint16_t prefetch_positions) {
    pc                    = PlanCollector{};
    pc.song               = &song;
    pc.indices            = out_indices;
    pc.last_use           = out_last_use_order_pos;
    pc.first_use          = out_first_use_order_pos;
    pc.capacity           = capacity;
    pc.prefetch_positions = prefetch_positions;
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        if (out_last_use_order_pos != nullptr) out_last_use_order_pos[i] = kSampleNeverUsed;
        if (out_first_use_order_pos != nullptr) out_first_use_order_pos[i] = kSampleNeverUsed;
    }
}

void plan_collect_row(void* user, const PatternCell* cells, uint8_t channel_count, uint16_t order_pos) {
    PlanCollector& pc = *static_cast<PlanCollector*>(user);
    if (pc.song == nullptr || cells == nullptr || pc.indices == nullptr || pc.last_use == nullptr) return;
    const Song& song = *pc.song;

    // Граница префетча - по позициям в порядке игры. Повторный заход на ту
    // же позицию считается новой: слушатель слышит её снова.
    if (order_pos != pc.prev_order_pos) {
        if (pc.positions_seen == pc.prefetch_positions) pc.prefetch_count = pc.count;
        ++pc.positions_seen;
        pc.prev_order_pos = order_pos;
    }

    const uint8_t channels = channel_count < kMaxPatternChannels ? channel_count : kMaxPatternChannels;
    for (uint8_t ch = 0; ch < channels; ++ch) {
        const PatternCell& cell = cells[ch];
        if (cell.instrument != 0) pc.last_instrument[ch] = cell.instrument;
        if (!is_real_note(cell.note)) continue;

        uint16_t sample_idx   = 0;
        uint8_t resolved_note = cell.note;
        if (!resolve_sample_index(song, pc.last_instrument[ch], cell.note, &sample_idx, &resolved_note)) continue;
        if (song.samples[sample_idx].length_samples == 0) continue; // грузить нечего

        // Позиция в проходе может убывать (переход назад), поэтому крайние
        // считаются минимумом и максимумом, а не присваиванием.
        const bool first_time = pc.last_use[sample_idx] == kSampleNeverUsed;
        if (first_time || order_pos > pc.last_use[sample_idx]) pc.last_use[sample_idx] = order_pos;
        if (pc.first_use != nullptr) {
            if (first_time || order_pos < pc.first_use[sample_idx]) pc.first_use[sample_idx] = order_pos;
        }
        if (first_time && pc.count < pc.capacity) pc.indices[pc.count++] = sample_idx;
    }
}

PlaybackPlan plan_collect_finish(PlanCollector& pc, LoadOrder order) {
    if (pc.song == nullptr) return {};
    // Песня короче префетча: префетч - вся песня.
    if (pc.positions_seen <= pc.prefetch_positions) pc.prefetch_count = pc.count;
    if (order != LoadOrder::ByFile) return {pc.count, pc.prefetch_count};
    sort_by_key(pc.indices, 0, pc.prefetch_count, *pc.song, 0);
    const uint32_t tail_origin = pc.prefetch_count > 0 ? pc.song->samples[pc.indices[pc.prefetch_count - 1]].file_offset : 0u;
    sort_by_key(pc.indices, pc.prefetch_count, pc.count, *pc.song, tail_origin);
    return {pc.count, pc.prefetch_count};
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
    if (order != LoadOrder::ByFile) return {count, w};
    sort_by_key(indices, 0, w, song, 0);
    const uint32_t tail_origin = w > 0 ? song.samples[indices[w - 1]].file_offset : 0u;
    sort_by_key(indices, w, count, song, tail_origin);
    return {count, w};
}

} // namespace player::load
