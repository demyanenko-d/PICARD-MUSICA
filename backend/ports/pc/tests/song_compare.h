#pragma once

// Побитная сверка двух Song, полученных загрузкой одного и того же файла
// через разные formats::ByteSource: совпадение доказывает, что источники
// взаимозаменяемы. Используется тестами FatFs и симулятора хоста для
// всех форматов, не только MOD.

#include "testing.h"

#include <vector>

#include "core/codec/dpcm8.h"
#include "core/model/song.h"
#include "core/memory/psram_store.h"
#include "core/memory/sample_cache_catalog.h"
#include "core/codec/pattern_reader.h"

namespace song_compare {

// Байты сэмпла в PSRAM по каталогу: данные (длина * байт на отсчёт) и у
// Dpcm8 - точки блоков по своей цепочке. Нерезидентный - пустой вектор.
inline std::vector<uint8_t> collect_sample_bytes(soundsinth::memory::PsramStore& psram,
                                                 soundsinth::memory::SampleCacheCatalog& catalog, uint16_t sample_index,
                                                 const soundsinth::model::SampleDescriptor& sd) {
    using namespace soundsinth;
    std::vector<uint8_t> out;
    const memory::SampleCacheEntry* e = memory::sample_cache_find(catalog, sample_index);
    if (e == nullptr) return out;
    auto take = [&](uint16_t page, uint32_t remaining) {
        while (remaining > 0 && page != memory::kPageChainEnd) {
            const uint32_t chunk = remaining < memory::kPsramPageBytes ? remaining : memory::kPsramPageBytes;
            const uint8_t* p = memory::psram_page_ptr(psram, page);
            out.insert(out.end(), p, p + chunk);
            remaining -= chunk;
            page = memory::psram_page_next(psram, page);
        }
    };
    take(e->first_page, sd.length_samples * soundsinth::model::resident_bytes_per_sample(sd.resident_encoding));
    if (e->checkpoint_first_page != memory::kPageChainEnd) {
        const uint32_t blocks = (sd.length_samples + dpcm8::kCheckpointIntervalSamples - 1) / dpcm8::kCheckpointIntervalSamples;
        take(e->checkpoint_first_page, blocks * static_cast<uint32_t>(sizeof(dpcm8::Dpcm8Checkpoint)));
    }
    return out;
}

inline void check_envelopes_equal(const soundsinth::model::Envelope* a,
                                  const soundsinth::model::Envelope* b) {
    CHECK_EQ(a == nullptr, b == nullptr);
    if (a == nullptr || b == nullptr) return;
    CHECK_EQ(a->enabled, b->enabled);
    CHECK_EQ(a->sustain_enabled, b->sustain_enabled);
    CHECK_EQ(a->loop_enabled, b->loop_enabled);
    CHECK_EQ(a->carry, b->carry);
    CHECK_EQ(a->point_count, b->point_count);
    CHECK_EQ(a->sustain_point, b->sustain_point);
    CHECK_EQ(a->sustain_end, b->sustain_end);
    CHECK_EQ(a->loop_start, b->loop_start);
    CHECK_EQ(a->loop_end, b->loop_end);
    for (uint8_t i = 0; i < a->point_count && i < b->point_count; ++i) {
        CHECK_EQ(a->points[i].tick, b->points[i].tick);
        CHECK_EQ(a->points[i].value, b->points[i].value);
    }
}

// catalog_a/catalog_b заданы - сверяются и байты каждого сэмпла в PSRAM.
inline void check_songs_equal(const soundsinth::model::Song& a, soundsinth::memory::PsramStore& psram_a,
                               const soundsinth::model::Song& b, soundsinth::memory::PsramStore& psram_b,
                               soundsinth::memory::SampleCacheCatalog* catalog_a = nullptr,
                               soundsinth::memory::SampleCacheCatalog* catalog_b = nullptr) {
    using namespace soundsinth;
    CHECK(a.pan_law == b.pan_law);
    CHECK(a.mix_levels == b.mix_levels);
    CHECK_EQ(a.sample_preamp, b.sample_preamp);
    CHECK_EQ(a.filter_units_per_octave, b.filter_units_per_octave);
    CHECK(a.channel_muted == b.channel_muted);
    CHECK(a.channel_surround == b.channel_surround);
    for (uint32_t ch = 0; ch < SOUNDSINTH_MAX_VOICES; ++ch) {
        CHECK_EQ(a.channel_pan[ch], b.channel_pan[ch]);
        CHECK_EQ(a.channel_volume[ch], b.channel_volume[ch]);
    }

    CHECK_EQ(a.channel_count, b.channel_count);
    CHECK_EQ(a.default_speed, b.default_speed);
    CHECK_EQ(a.default_tempo, b.default_tempo);
    CHECK_EQ(a.default_global_volume, b.default_global_volume);
    CHECK(a.frequency_model == b.frequency_model);
    CHECK_EQ(a.order_count, b.order_count);
    CHECK_EQ(a.restart_position, b.restart_position);
    CHECK_EQ(a.pattern_count, b.pattern_count);
    CHECK_EQ(a.sample_count, b.sample_count);
    CHECK_EQ(a.instrument_count, b.instrument_count);
    CHECK_EQ(a.quirks, b.quirks);
    CHECK_EQ(a.flow_mode, b.flow_mode);

    const uint16_t order_count = a.order_count < b.order_count ? a.order_count : b.order_count;
    for (uint16_t i = 0; i < order_count; ++i) {
        CHECK_EQ(a.order[i], b.order[i]);
    }

    const uint16_t sample_count = a.sample_count < b.sample_count ? a.sample_count : b.sample_count;
    for (uint16_t i = 0; i < sample_count; ++i) {
        const auto& sa = a.samples[i];
        const auto& sb = b.samples[i];
        CHECK(sa.encoding == sb.encoding);
        CHECK_EQ(sa.channels, sb.channels);
        CHECK_EQ(sa.length_samples, sb.length_samples);
        CHECK_EQ(sa.loop_start, sb.loop_start);
        CHECK_EQ(sa.loop_end, sb.loop_end);
        CHECK_EQ(sa.loop_enabled, sb.loop_enabled);
        CHECK_EQ(sa.loop_bidirectional, sb.loop_bidirectional);
        CHECK_EQ(sa.c5_speed, sb.c5_speed);
        CHECK_EQ(sa.relative_note, sb.relative_note);
        CHECK_EQ(sa.finetune, sb.finetune);
        CHECK_EQ(sa.default_volume, sb.default_volume);
        CHECK_EQ(sa.default_panning, sb.default_panning);
        CHECK_EQ(sa.global_volume, sb.global_volume);
        CHECK(sa.resident_encoding == sb.resident_encoding);
        CHECK(sa.loop_unroll == sb.loop_unroll);
        if (catalog_a != nullptr && catalog_b != nullptr) {
            CHECK(collect_sample_bytes(psram_a, *catalog_a, i, sa) == collect_sample_bytes(psram_b, *catalog_b, i, sb));
        }
        // file_offset нарочно не сверяется: он зависит от разбора файла тем же
        // кодом, а не от источника байт. Для MOD-загрузчика он и так совпадёт,
        // но эта сверка проверяет взаимозаменяемость ByteSource, поэтому поле не
        // включаем.
    }

    const uint16_t instrument_count = a.instrument_count < b.instrument_count ? a.instrument_count : b.instrument_count;
    for (uint16_t i = 0; i < instrument_count; ++i) {
        const auto& ia = a.instruments[i];
        const auto& ib = b.instruments[i];
        CHECK_EQ(ia.global_volume, ib.global_volume);
        CHECK_EQ(ia.fadeout_rate, ib.fadeout_rate);
        CHECK_EQ(ia.default_sample_index, ib.default_sample_index);
        CHECK_EQ(ia.note_to_sample_ranges == nullptr, ib.note_to_sample_ranges == nullptr);
        CHECK_EQ(ia.note_to_sample_range_count, ib.note_to_sample_range_count);
        if (ia.note_to_sample_ranges != nullptr && ib.note_to_sample_ranges != nullptr) {
            for (uint8_t r = 0; r < ia.note_to_sample_range_count && r < ib.note_to_sample_range_count; ++r) {
                CHECK_EQ(ia.note_to_sample_ranges[r].start_note, ib.note_to_sample_ranges[r].start_note);
                CHECK_EQ(ia.note_to_sample_ranges[r].sample_index, ib.note_to_sample_ranges[r].sample_index);
                CHECK_EQ(ia.note_to_sample_ranges[r].note_offset, ib.note_to_sample_ranges[r].note_offset);
            }
        }
        check_envelopes_equal(ia.volume_envelope, ib.volume_envelope);
        check_envelopes_equal(ia.panning_envelope, ib.panning_envelope);
        check_envelopes_equal(ia.pitch_envelope, ib.pitch_envelope);
        check_envelopes_equal(ia.filter_envelope, ib.filter_envelope);
        CHECK(ia.nna == ib.nna);
        CHECK(ia.dct == ib.dct);
        CHECK(ia.dca == ib.dca);
    }

    const uint16_t pattern_count = a.pattern_count < b.pattern_count ? a.pattern_count : b.pattern_count;
    for (uint16_t p = 0; p < pattern_count; ++p) {
        const auto& pa = a.patterns[p];
        const auto& pb = b.patterns[p];
        CHECK_EQ(pa.row_count, pb.row_count);
        CHECK_EQ(pa.channel_count, pb.channel_count);
        if (pa.row_count != pb.row_count || pa.channel_count != pb.channel_count) {
            continue;
        }

        patterns::PatternReader reader_a(memory::psram_pattern_ptr(psram_a, pa.psram_offset), pa.row_count, pa.channel_count);
        patterns::PatternReader reader_b(memory::psram_pattern_ptr(psram_b, pb.psram_offset), pb.row_count, pb.channel_count);
        std::vector<soundsinth::model::PatternCell> cells_a(pa.channel_count);
        std::vector<soundsinth::model::PatternCell> cells_b(pb.channel_count);
        for (uint16_t row = 0; row < pa.row_count; ++row) {
            reader_a.read_row(row, cells_a.data());
            reader_b.read_row(row, cells_b.data());
            for (uint8_t ch = 0; ch < pa.channel_count; ++ch) {
                CHECK_EQ(cells_a[ch].note, cells_b[ch].note);
                CHECK_EQ(cells_a[ch].instrument, cells_b[ch].instrument);
                CHECK(cells_a[ch].volume.type == cells_b[ch].volume.type);
                CHECK_EQ(cells_a[ch].volume.param, cells_b[ch].volume.param);
                CHECK(cells_a[ch].effect.type == cells_b[ch].effect.type);
                CHECK(cells_a[ch].effect.rate == cells_b[ch].effect.rate);
                CHECK_EQ(cells_a[ch].effect.param, cells_b[ch].effect.param);
            }
        }
    }
}

} // namespace song_compare
