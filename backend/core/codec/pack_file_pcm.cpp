#include "core/codec/pack_file_pcm.h"

#include <algorithm>
#include <cstdio>

#include "core/codec/loop_unroll.h"
#include "core/formats/binary_reader.h"
#include "core/memory/track_memory.h"
#include "core/codec/sample_pack.h"

namespace soundsinth::model {

namespace {

char s_reason_buf[kLoadReasonBytes];

// Сырой кусок файла -> int16 для SamplePacker. Raw8 хранит 8-битную шкалу
// как есть, без *256: 8-битные отсчёты остаются в [-128, 127].
void convert_chunk(const uint8_t* raw, uint32_t count, SampleEncoding encoding, bool signed_pcm, int32_t& running,
                   int16_t* out) {
    switch (encoding) {
        case SampleEncoding::Pcm8:
            for (uint32_t k = 0; k < count; ++k) {
                out[k] = static_cast<int16_t>(signed_pcm ? static_cast<int8_t>(raw[k])
                                                         : static_cast<int8_t>(raw[k] - 128));
            }
            break;
        case SampleEncoding::Pcm16:
            for (uint32_t k = 0; k < count; ++k) {
                const uint16_t v = static_cast<uint16_t>(raw[k * 2] | (raw[k * 2 + 1] << 8));
                out[k] = signed_pcm ? static_cast<int16_t>(v) : static_cast<int16_t>(v - 32768);
            }
            break;
        case SampleEncoding::XmDelta8:
            for (uint32_t k = 0; k < count; ++k) {
                running = static_cast<int8_t>(running + static_cast<int8_t>(raw[k])); // по модулю 256
                out[k] = static_cast<int16_t>(static_cast<int8_t>(running));
            }
            break;
        case SampleEncoding::XmDelta16:
            for (uint32_t k = 0; k < count; ++k) {
                const int16_t delta = static_cast<int16_t>(raw[k * 2] | (raw[k * 2 + 1] << 8));
                running = static_cast<int16_t>(running + delta); // по модулю 65536
                out[k] = static_cast<int16_t>(running);
            }
            break;
        default:
            break;
    }
}

} // namespace

char* load_reason_buffer() { return s_reason_buf; }

bool resident_pages_fit(const memory::TrackMemory& mem, const SampleDescriptor& sd) {
    return memory::psram_free_page_count(mem.psram) >= resident_pages(sd.resident_encoding, sd.length_samples);
}

bool pack_file_pcm(memory::TrackMemory& mem, formats::BinaryReader& r, const SampleDescriptor& sd, uint16_t sample_index,
                   const char** reason_out) {
    auto fail = [&](const char* msg) {
        if (reason_out) *reason_out = msg;
        memory::scratch_release_all(mem.scratch); // отказ выселяет сценарий: место возвращается арене
        return false;
    };
    // Внутри цикла причина ставится без выхода: после первой add_samples
    // цепочка страниц обязана дойти до finish_and_publish.
    auto set_reason = [&](const char* msg) {
        if (reason_out) *reason_out = msg;
    };

    const SampleEncoding e = sd.encoding;
    if (e == SampleEncoding::S3mAdpcm4) return fail("S3M ADPCM не поддержан");
    if (e != SampleEncoding::Pcm8 && e != SampleEncoding::Pcm16 && e != SampleEncoding::XmDelta8 &&
        e != SampleEncoding::XmDelta16) {
        return fail("кодек файла не распаковывается");
    }
    if (sd.unsupported_codec) return fail("ModPlug-ADPCM не поддержан");
    if (sd.source_length_samples == 0) return fail("нулевая длина");
    if (!resident_pages_fit(mem, sd)) return fail(kPsramFull);
    if (!r.seek(sd.file_offset)) return fail("не встать на данные сэмпла");

    constexpr uint32_t kChunkSamples = 4096;
    uint8_t* raw_chunk =
        memory::scratch_take(mem.scratch, memory::Scratch::SampleRepack, memory::kSampleRepackBufferBytes);
    if (!raw_chunk) return fail("резидентная память переполнена (буфер перепаковки)");
    auto* pcm_chunk = reinterpret_cast<int16_t*>(raw_chunk + kChunkSamples * sizeof(int16_t));
    static_assert(kChunkSamples * sizeof(int16_t) * 2 <= memory::kSampleRepackBufferBytes,
                  "буфер перепаковки переполнен");
    const uint32_t bytes_per_sample = (e == SampleEncoding::Pcm16 || e == SampleEncoding::XmDelta16) ? 2u : 1u;

    // Решение о кодеке уже в дескрипторе; sd.length_samples уже после
    // прореживания, поэтому из файла читается sd.source_length_samples.
    sample_pack::SamplePacker packer(mem.psram, sd.resident_encoding, sd.decimated);
    if (sd.loop_unroll != LoopUnroll::None) {
        packer.set_loop_unroll(sd.loop_unroll, sd.loop_start, loop_end_before_unroll(sd));
    }
    uint32_t remaining = sd.source_length_samples;
    bool ok = true;
    int32_t running = 0; // накопительная сумма дельт XM
    while (remaining > 0) {
        const uint32_t chunk = std::min(remaining, kChunkSamples);
        r.bytes(raw_chunk, chunk * bytes_per_sample);
        if (!r.ok()) {
            // Числа отличают сэмпл за концом файла (разбор заголовков) от
            // короткого чтения (шина).
            if (reason_out) {
                std::snprintf(s_reason_buf, sizeof(s_reason_buf),
                              "чтение оборвалось: сэмпл с %lu, всего %lu байт, осталось %lu, файл %lu",
                              static_cast<unsigned long>(sd.file_offset),
                              static_cast<unsigned long>(sd.source_length_samples) * bytes_per_sample,
                              static_cast<unsigned long>(remaining) * bytes_per_sample,
                              static_cast<unsigned long>(r.size()));
                *reason_out = s_reason_buf;
            }
            ok = false;
            break;
        }
        convert_chunk(raw_chunk, chunk, e, sd.signed_pcm, running, pcm_chunk);
        if (!packer.add_samples(pcm_chunk, chunk)) {
            set_reason(kPsramFull);
            ok = false;
            break;
        }
        remaining -= chunk;
    }
    // !ok: пропускается только этот сэмпл, он остаётся нерезидентным, файл
    // грузится дальше.
    memory::scratch_leave(mem.scratch, memory::Scratch::SampleRepack);
    return sample_pack::finish_and_publish(packer, ok, mem.psram, mem.sample_cache, sample_index, reason_out);
}

} // namespace soundsinth::model
