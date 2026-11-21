// SPDX-License-Identifier: MIT
#include "core/codec/loop_unroll.h"

#include "core/codec/dpcm8.h"
#include "core/codec/resident_encoding_policy.h"
#include "core/memory/psram_store.h"

namespace soundsinth::model {

namespace {

uint32_t div_up(uint32_t a, uint32_t b) {
    return (a + b - 1u) / b;
}

bool can_unroll(const SampleDescriptor& sd) {
    return sample_is_resident(sd) && sd.loop_enabled && sd.loop_bidirectional && sd.loop_unroll == LoopUnroll::None && sd.loop_start + 2u < sd.loop_end &&
           sd.loop_end <= sd.length_samples;
}

// Обычная петля сэмпла с петлёй удержания: её координаты двигаются вместе с
// loop_* при прореживании и развороте.
Song::SusRelease* sus_release_of(Song& song, uint16_t sample_index) {
    for (uint16_t k = 0; k < song.sus_release_count; ++k) {
        if (song.sus_release[k].sample_index == sample_index) return &song.sus_release[k];
    }
    return nullptr;
}

} // namespace

uint32_t resident_pages(ResidentEncoding e, uint32_t length_samples) {
    uint32_t pages = div_up(length_samples * resident_bytes_per_sample(e), memory::kPsramPageBytes);
    if (e == ResidentEncoding::Dpcm8) {
        const uint32_t points  = div_up(length_samples, dpcm8::kCheckpointIntervalSamples);
        pages                 += div_up(points * static_cast<uint32_t>(sizeof(dpcm8::Dpcm8Checkpoint)), memory::kPsramPageBytes);
    }
    return pages;
}

bool choose_resident_encoding(Song& song, uint32_t free_pages) {
    uint32_t need = 0;
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        const SampleDescriptor& sd = song.samples[i];
        if (!sample_is_resident(sd)) continue;
        const ResidentEncodingDecision d  = decide_resident_encoding(sample_is_16bit(sd.encoding), sd.source_length_samples, sd.c5_speed, true);
        need                             += resident_pages(d.mode, d.decimate ? (sd.source_length_samples + 1) / 2 : sd.source_length_samples);
    }
    const bool allow_raw16 = need <= free_pages;
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        SampleDescriptor& sd = song.samples[i];
        if (sd.length_samples == 0) continue;
        const ResidentEncodingDecision d = decide_resident_encoding(sample_is_16bit(sd.encoding), sd.source_length_samples, sd.c5_speed, allow_raw16);
        sd.resident_encoding             = d.mode;
        if (d.decimate) {
            sd.decimated      = true;
            sd.length_samples = (sd.source_length_samples + 1) / 2;
            sd.c5_speed       = sd.c5_speed / 2;
            sd.loop_start     = sd.loop_start / 2;
            sd.loop_end       = sd.loop_end / 2;
            if (sd.sus_loop) {
                if (Song::SusRelease* rel = sus_release_of(song, i)) {
                    rel->loop_start /= 2;
                    rel->loop_end   /= 2;
                }
            }
        }
    }
    return allow_raw16;
}

uint16_t unroll_pingpong_loops(Song& song, uint32_t free_pages, LoopUnroll mode) {
    if (mode == LoopUnroll::None) return 0;

    uint32_t need = 0;
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        const SampleDescriptor& sd = song.samples[i];
        if (sample_is_resident(sd)) need += resident_pages(sd.resident_encoding, sd.length_samples);
    }
    if (need >= free_pages) return 0;
    uint32_t budget = free_pages - need;

    // Короткие первыми: у них слышнее всего и период (L против 2L), и
    // стоят они меньше. Выбор минимума за проход - сэмплов не больше
    // нескольких сотен, памяти под сортировку не нужно.
    uint16_t done = 0;
    for (;;) {
        uint16_t best = song.sample_count;
        for (uint16_t i = 0; i < song.sample_count; ++i) {
            const SampleDescriptor& sd = song.samples[i];
            if (!can_unroll(sd)) continue;
            if (best == song.sample_count || sd.loop_end - sd.loop_start < song.samples[best].loop_end - song.samples[best].loop_start) {
                best = i;
            }
        }
        if (best == song.sample_count) break;

        SampleDescriptor& sd = song.samples[best];
        const uint32_t extra = loop_unroll_extra(mode, sd.loop_end - sd.loop_start);
        const uint32_t more  = resident_pages(sd.resident_encoding, sd.length_samples + extra) - resident_pages(sd.resident_encoding, sd.length_samples);
        if (more > budget) break; // остальные петли длиннее
        budget         -= more;
        sd.loop_unroll  = mode;
        // Разворот петли удержания вставляет extra отсчётов в середину
        // сэмпла (хвост упаковщик пишет следом): обычная петля за ней
        // сдвигается. Петля, начавшаяся раньше конца удержания, не
        // двигается (пересечение областей - не по формату).
        if (sd.sus_loop) {
            if (Song::SusRelease* rel = sus_release_of(song, best)) {
                if (rel->loop_start >= sd.loop_end) {
                    rel->loop_start += extra;
                    rel->loop_end   += extra;
                }
            }
        }
        sd.loop_end       += extra;
        sd.length_samples += extra;
        ++done;
    }
    return done;
}

} // namespace soundsinth::model
