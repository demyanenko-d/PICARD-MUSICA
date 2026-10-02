// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/formats/load_stats.h"
#include "core/model/song.h"
#include "core/formats/it.h"
#include "core/formats/s3m.h"
#include "core/formats/xm.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_reader.h"

using namespace soundsinth;
using soundsinth::model::Effect;
using soundsinth::model::Song;
using soundsinth::model::VolumeColumnType;

namespace {

// Собирает минимальный синтетический XM вручную, поле за полем, в порядке
// файла. Backpatch не нужен: в отличие от S3M, в XM нет парапоинтеров,
// заголовки и данные идут последовательно. Смещения сверены с OpenMPT
// soundlib/XMTools.h (см. formats/xm.cpp).
std::vector<uint8_t> build_synthetic_xm() {
    std::vector<uint8_t> f;
    auto put8  = [&](uint8_t v) { f.push_back(v); };
    auto put16 = [&](uint16_t v) {
        f.push_back(static_cast<uint8_t>(v & 0xFF));
        f.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            f.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    auto put_str = [&](const char* s, size_t n) {
        size_t len = std::strlen(s);
        for (size_t i = 0; i < n; ++i)
            f.push_back(i < len ? static_cast<uint8_t>(s[i]) : 0);
    };

    put_str("Extended Module: ", 17);
    put_str("XMTEST", 20);
    put8(0x1A);
    put_str("FastTracker v2.00", 20); // проверяем детекцию FT2-квирков по имени трекера
    put16(0x0104);

    CHECK_EQ(f.size(), static_cast<size_t>(60));
    put32(21);     // headerSize = 20(фикс.часть) + 1(таблица воспроизведения)
    put16(1);      // orders
    put16(0);      // restartPos
    put16(2);      // channels
    put16(1);      // patterns
    put16(1);      // instruments
    put16(0x0001); // flags: linearSlides=1
    put16(6);      // speed
    put16(125);    // tempo
    CHECK_EQ(f.size(), static_cast<size_t>(80));
    put8(0); // order[0] = паттерн 0

    const size_t pattern_offset = f.size();
    CHECK_EQ(pattern_offset, static_cast<size_t>(81));

    std::vector<uint8_t> packed;
    auto pcell = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes)
            packed.push_back(b);
    };
    // Row0: ch0 note=C-4 (raw49 -> canonical48) + instr1 + vol48 (0x40);
    // ch1 пусто.
    pcell({0x87, 49, 1, 0x40});
    pcell({0x80});
    // Row1: ch0 PatternBreak BCD 0x12 -> строка 12; ch1 пусто.
    pcell({0x98, 13, 0x12});
    pcell({0x80});
    // Row2: ch0 E1x (fine porta up, sub_param=5); ch1 volume-колонка
    // VibratoSpeed (0xA5, p=5).
    pcell({0x98, 14, 0x15});
    pcell({0x84, 0xA5});
    // Row3: ch0 несжатая ("легаси") запись - note=97 (KeyOff), остальное 0;
    // ch1 пусто.
    pcell({97, 0, 0, 0, 0});
    pcell({0x80});

    put32(9);                                    // patHeaderLen
    put8(0);                                     // packing_type
    put16(4);                                    // numRows
    put16(static_cast<uint16_t>(packed.size())); // packedDataSize
    for (uint8_t b : packed)
        put8(b);

    const size_t inst_start = f.size();
    CHECK_EQ(inst_start, pattern_offset + 9 + packed.size());

    constexpr uint32_t kInstHeaderMin = 29;
    constexpr uint32_t kSampleHeaderSizeField =
        4; // sampleHeaderSize - поле XMInstrumentHeader (фикс в formats/xm.cpp); раньше его не было и в этой синтетике, вслед за тем же багом в загрузчике
    constexpr uint32_t kXmInstrumentBytes = 230;
    put32(kInstHeaderMin + kSampleHeaderSizeField + kXmInstrumentBytes); // size
    put_str("INSTR1", 22);
    put8(0);   // type
    put16(2);  // numSamples = 2 -> keymap возможен
    put32(40); // sampleHeaderSize (XMSample size - see kXmSampleHeaderBytes)

    const size_t xm_instrument_start = f.size();
    CHECK_EQ(xm_instrument_start, inst_start + kInstHeaderMin + kSampleHeaderSizeField);

    uint8_t sample_map[96];
    for (int i = 0; i < 96; ++i)
        sample_map[i] = (i < 48) ? 0 : 1; // 2 разных локальных сэмпла -> keymap
    for (uint8_t v : sample_map)
        put8(v);

    // volEnv[24] (12 точек tick,value) - используются только первые 2.
    put16(0);
    put16(64); // точка 0: tick=0, value=64
    put16(10);
    put16(0); // точка 1: tick=10, value=0
    for (int i = 2; i < 12; ++i) {
        put16(0);
        put16(0);
    }
    // panEnv[24] - тоже первые 2 точки (см. panFlags=0x01 ниже), а не нули:
    // тест должен ловить сдвиг чтения на 4 байта (найденный баг, см.
    // formats/xm.cpp). С нулевой огибающей результат enabled=false совпал бы
    // и при сдвинутом, и при верном чтении.
    put16(0);
    put16(20); // точка 0: tick=0, value=20
    put16(15);
    put16(50); // точка 1: tick=15, value=50
    for (int i = 2; i < 12; ++i) {
        put16(0);
        put16(0);
    }

    put8(2);    // volPoints
    put8(2);    // panPoints
    put8(0);    // volSustain
    put8(0);    // volLoopStart
    put8(0);    // volLoopEnd
    put8(0);    // panSustain
    put8(0);    // panLoopStart
    put8(0);    // panLoopEnd
    put8(0x01); // volFlags: enabled, без sustain/loop
    put8(0x01); // panFlags: enabled, без sustain/loop
    put8(0);
    put8(0);
    put8(0);
    put8(0);    // vibType/vibSweep/vibDepth/vibRate - не моделируется
    put16(256); // volFade
    // Остаток XMInstrument (midiEnabled..reserved, 22 байта) загрузчиком не
    // используется, нули.
    for (int i = 0; i < 22; ++i)
        put8(0);
    CHECK_EQ(f.size(), xm_instrument_start + kXmInstrumentBytes);

    const size_t sample_headers_start = f.size();
    CHECK_EQ(sample_headers_start, inst_start + kInstHeaderMin + kSampleHeaderSizeField + kXmInstrumentBytes);

    auto put_sample_header = [&](uint32_t length_bytes, uint8_t vol, uint8_t flags, uint32_t loop_start_bytes = 0, uint32_t loop_len_bytes = 0) {
        put32(length_bytes);
        put32(loop_start_bytes);
        put32(loop_len_bytes);
        put8(vol);
        put8(0); // finetune
        put8(flags);
        put8(128); // pan
        put8(0);   // relnote
        put8(0);   // reserved
        for (int i = 0; i < 22; ++i)
            put8(0); // name
    };
    put_sample_header(6, 64, 0x00); // сэмпл0: 6 байт, без петли, 8-бит, моно
    // сэмпл1: 4 байта, флаг 0x02 (ping-pong loop) без 0x01 (forward loop).
    // Найденный баг (final_fantasy.xm, канал 5): раньше петля включалась
    // только по 0x01, файлы с одним 0x02 читались как незацикленные. В OpenMPT
    // soundlib/XMTools.cpp "flags & (sampleLoop|sampleBidiLoop)" - петлю
    // включает любой из двух битов.
    put_sample_header(4, 48, 0x02, 1, 2);

    CHECK_EQ(f.size(), sample_headers_start + 2 * 40);

    // PCM: дельта-кодированные 8-бит сэмплы.
    const int8_t sample0_deltas[6] = {10, -5, 20, -15, 5, -3};
    for (int8_t d : sample0_deltas)
        put8(static_cast<uint8_t>(d));
    const int8_t sample1_deltas[4] = {50, -100, 80, -20};
    for (int8_t d : sample1_deltas)
        put8(static_cast<uint8_t>(d));

    return f;
}

void test_synthetic_exact() {
    std::printf("test_xm_loader_synthetic_exact\n");

    const std::vector<uint8_t> file = build_synthetic_xm();
    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));

    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok     = formats::xm::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);
    if (!ok) {
        memory::track_memory_destroy(mem);
        return;
    }

    CHECK_EQ(song.channel_count, static_cast<uint8_t>(2));
    CHECK_EQ(song.sample_preamp, static_cast<uint8_t>(48)); // XM: константа, см. song.h
    CHECK_EQ(song.pattern_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.instrument_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.sample_count, static_cast<uint16_t>(2));
    CHECK_EQ(song.order_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.order[0], static_cast<uint16_t>(0));
    CHECK(song.frequency_model == soundsinth::model::FrequencyModel::Linear);
    CHECK((song.quirks & soundsinth::model::kQuirkXmFt2ArpeggioTable) != 0); // "FastTracker v2.00" должен включить FT2-квирки
    CHECK((song.quirks & soundsinth::model::kQuirkXmVolColumnBeforeEffect) != 0);

    const auto& inst = song.instruments[0];
    CHECK(inst.note_to_sample_ranges != nullptr); // 2 разных локальных сэмпла -> keymap
    auto sample_for_note = [&](uint8_t note) -> uint16_t {
        const soundsinth::model::KeymapRange* range = nullptr;
        for (uint8_t r = 0; r < inst.note_to_sample_range_count; ++r) {
            if (soundsinth::model::keymap_range_start(inst.note_to_sample_ranges[r]) > note) break;
            range = &inst.note_to_sample_ranges[r];
        }
        return range != nullptr ? range->sample_index : inst.default_sample_index;
    };
    CHECK_EQ(sample_for_note(0), static_cast<uint16_t>(0));  // нота 0 -> локальный сэмпл 0
    CHECK_EQ(sample_for_note(48), static_cast<uint16_t>(1)); // нота 48 -> локальный сэмпл 1
    CHECK(inst.volume_envelope != nullptr);
    CHECK(inst.volume_envelope->enabled);
    CHECK(!inst.volume_envelope->sustain_enabled);
    CHECK_EQ(inst.volume_envelope->point_count, static_cast<uint8_t>(2));
    CHECK_EQ(inst.volume_envelope->points[0].tick, static_cast<uint16_t>(0));
    CHECK_EQ(inst.volume_envelope->points[0].value, static_cast<int16_t>(64));
    CHECK_EQ(inst.volume_envelope->points[1].tick, static_cast<uint16_t>(10));
    CHECK(inst.panning_envelope != nullptr); // panFlags=0x01 - найденный баг: раньше читался со сдвигом на 4 байта и почти всегда давал "выключено"
    CHECK(inst.panning_envelope->enabled);
    CHECK(!inst.panning_envelope->sustain_enabled);
    CHECK_EQ(inst.panning_envelope->point_count, static_cast<uint8_t>(2));
    CHECK_EQ(inst.panning_envelope->points[0].tick, static_cast<uint16_t>(0));
    CHECK_EQ(inst.panning_envelope->points[0].value, static_cast<int16_t>(20));
    CHECK_EQ(inst.panning_envelope->points[1].tick, static_cast<uint16_t>(15));
    CHECK_EQ(inst.panning_envelope->points[1].value, static_cast<int16_t>(50));
    CHECK_EQ(inst.fadeout_rate, 256u << 1);

    CHECK_EQ(song.samples[0].length_samples, 6u);
    CHECK_EQ(song.samples[1].length_samples, 4u);
    CHECK(!song.samples[0].loop_enabled);
    CHECK(song.samples[1].loop_enabled); // flags=0x02 (ping-pong без 0x01) должен включать петлю, см. put_sample_header выше
    CHECK(song.samples[1].loop_bidirectional);
    CHECK_EQ(song.samples[1].loop_start, 1u);
    CHECK_EQ(song.samples[1].loop_end, 3u);

    patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, song.patterns[0].psram_offset), song.patterns[0].row_count,
                                   song.patterns[0].channel_count);
    soundsinth::model::PatternCell cells[2];

    reader.read_row(0, cells);
    CHECK_EQ(cells[0].note, static_cast<uint8_t>(48)); // raw 49 -> canonical 48 (C-4)
    CHECK_EQ(cells[0].instrument, static_cast<uint8_t>(1));
    CHECK(cells[0].volume.type == VolumeColumnType::SetVolume);
    CHECK_EQ(cells[0].volume.param, static_cast<uint8_t>(48));

    reader.read_row(1, cells);
    CHECK(cells[0].effect.type == Effect::PatternBreak);
    CHECK_EQ(cells[0].effect.param, static_cast<uint8_t>(12)); // BCD 0x12 -> 12

    reader.read_row(2, cells);
    CHECK(cells[0].effect.type == Effect::PortaUp);
    CHECK(cells[0].effect.rate == soundsinth::model::SlideRate::Fine);
    CHECK_EQ(cells[0].effect.param, static_cast<uint8_t>(5));
    CHECK(cells[1].volume.type == VolumeColumnType::VibratoSpeed);
    CHECK_EQ(cells[1].volume.param, static_cast<uint8_t>(5));

    reader.read_row(3, cells);
    CHECK_EQ(cells[0].note, soundsinth::model::kNoteOff);

    auto* cache0 = memory::sample_cache_find(mem.sample_cache, 0);
    auto* cache1 = memory::sample_cache_find(mem.sample_cache, 1);
    CHECK(cache0 != nullptr);
    CHECK(cache1 != nullptr);
    // PCM - накопленные дельты.
    if (cache0 && cache1) {
        const auto* p0        = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, cache0->first_page));
        const int8_t want0[6] = {10, 5, 25, 10, 15, 12};
        for (int i = 0; i < 6; ++i)
            CHECK_EQ(p0[i], want0[i]);
        const auto* p1        = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, cache1->first_page));
        const int8_t want1[4] = {50, -50, 30, 10};
        for (int i = 0; i < 4; ++i)
            CHECK_EQ(p1[i], want1[i]);
    }

    memory::track_memory_destroy(mem);
}

// Дельты XM с переносом: 8 бит {100, 100} -> {100, -56}; 16 бит (флаг 0x10)
// {30000, 30000, -1} -> {30000, -5536, -5537}, резидентно Raw16.
void test_pcm_delta_wraps() {
    std::printf("test_xm_pcm_delta_wraps\n");
    constexpr size_t kSample0 = 373, kPcm0 = 373 + 80;
    for (const bool wide : {false, true}) {
        std::vector<uint8_t> f = build_synthetic_xm();
        CHECK_EQ(f[kSample0], static_cast<uint8_t>(6));
        if (wide) {
            f[kSample0 + 14]   = 0x10;
            const uint8_t d[6] = {0x30, 0x75, 0x30, 0x75, 0xFF, 0xFF};
            for (int i = 0; i < 6; ++i)
                f[kPcm0 + i] = d[i];
        } else {
            f[kPcm0]     = 100;
            f[kPcm0 + 1] = 100;
        }
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        CHECK(formats::xm::load(src.as_byte_source(), mem, song, nullptr));
        const memory::SampleCacheEntry* e = memory::sample_cache_find(mem.sample_cache, 0);
        CHECK(e != nullptr);
        if (e && wide) {
            CHECK(song.samples[0].resident_encoding == soundsinth::model::ResidentEncoding::Raw16);
            CHECK_EQ(song.samples[0].length_samples, 3u);
            const uint8_t* p      = memory::psram_page_ptr(mem.psram, e->first_page);
            const int16_t want[3] = {30000, -5536, -5537};
            for (int i = 0; i < 3; ++i)
                CHECK_EQ(static_cast<int16_t>(p[2 * i] | (p[2 * i + 1] << 8)), want[i]);
        } else if (e) {
            const auto* p = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, e->first_page));
            CHECK_EQ(p[0], static_cast<int8_t>(100));
            CHECK_EQ(p[1], static_cast<int8_t>(-56));
        }
        memory::track_memory_destroy(mem);
    }
}

void test_real_small_file_smoke() {
    std::printf("test_xm_loader_real_small_file_smoke\n");

    std::ifstream in("SD/test_music/xm/2djs10.xm", std::ios::binary);
    if (!in) {
        std::printf("  file not found - skipped (not started from the repository root?)\n");
        return;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(file.size() > 80);

    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok     = formats::xm::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);
    if (ok) {
        CHECK(song.channel_count >= 1 && song.channel_count <= 64);
        CHECK(song.pattern_count >= 1);
        CHECK(song.order_count >= 1);
        for (uint16_t p = 0; p < song.pattern_count; ++p) {
            CHECK(song.patterns[p].psram_offset != soundsinth::model::Pattern::kInvalidOffset);
        }
    }

    memory::track_memory_destroy(mem);
}

// Стерео-сэмпл (флаг 0x20): звучит левый канал - первая половина данных,
// длина и границы петли пополам; петля, пустая после деления, выключена.
void test_stereo_sample_left_channel() {
    std::printf("test_xm_stereo_sample_left_channel\n");
    constexpr size_t kSample0 = 373; // заголовок сэмпла 0 в синтетике
    struct Case {
        uint8_t loop_start, loop_len;
        bool enabled;
        uint32_t start, end;
    };
    const Case cases[] = {{0, 2, true, 0, 1}, {2, 1, false, 1, 1}};
    for (const Case& c : cases) {
        std::vector<uint8_t> f = build_synthetic_xm();
        CHECK_EQ(f[kSample0], static_cast<uint8_t>(6));
        f[kSample0 + 4]  = c.loop_start;
        f[kSample0 + 8]  = c.loop_len;
        f[kSample0 + 14] = 0x21;
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        CHECK(formats::xm::load(src.as_byte_source(), mem, song, nullptr));
        const soundsinth::model::SampleDescriptor& sd = song.samples[0];
        CHECK_EQ(sd.channels, static_cast<uint8_t>(1));
        CHECK_EQ(sd.length_samples, 3u);
        CHECK_EQ(sd.loop_enabled, c.enabled);
        CHECK_EQ(sd.loop_start, c.start);
        CHECK_EQ(sd.loop_end, c.end);
        const memory::SampleCacheEntry* e = memory::sample_cache_find(mem.sample_cache, 0);
        CHECK(e != nullptr);
        if (e) {
            const auto* p = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, e->first_page));
            CHECK_EQ(p[0], static_cast<int8_t>(10));
            CHECK_EQ(p[1], static_cast<int8_t>(5));
            CHECK_EQ(p[2], static_cast<int8_t>(25));
        }
        memory::track_memory_destroy(mem);
    }
}

// Огибающие XM как у OpenMPT: включённая без точек - как выключенная
// (nullptr), конец петли за точками прижат к последней точке, петля
// остаётся.
void test_envelope_empty_and_loop_clamp() {
    std::printf("test_xm_envelope_empty_and_loop_clamp\n");
    constexpr size_t kVolPoints = 335, kPanLoopStart = 341, kPanLoopEnd = 342, kPanFlags = 344;
    std::vector<uint8_t> f = build_synthetic_xm();
    CHECK_EQ(f[kVolPoints], static_cast<uint8_t>(2));
    f[kVolPoints]    = 0;
    f[kPanLoopStart] = 0;
    f[kPanLoopEnd]   = 5;
    f[kPanFlags]     = 0x01 | 0x04;
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
    CHECK(formats::xm::load(src.as_byte_source(), mem, song, nullptr));
    CHECK(song.instruments[0].volume_envelope == nullptr);
    const soundsinth::model::Envelope* pan = song.instruments[0].panning_envelope;
    CHECK(pan != nullptr);
    if (pan) {
        CHECK(pan->loop_enabled);
        CHECK_EQ(pan->loop_end, static_cast<uint8_t>(1));
    }
    memory::track_memory_destroy(mem);
}

// Finetune сэмпла: FT2 отбрасывает младшие три бита (5 -> 0, -3 -> -8),
// OpenMPT до 1.22.07.19 - нет, как у OpenMPT.
void test_finetune_precision() {
    std::printf("test_xm_finetune_precision\n");
    constexpr size_t kTrackerName = 38, kFinetune0 = 373 + 13, kFinetune1 = 413 + 13;
    struct Case {
        const char* tracker;
        int8_t want0, want1;
    };
    const Case cases[] = {{"FastTracker v2.00", 0, -8}, {"OpenMPT 1.28.00.00", 0, -8}, {"OpenMPT 1.22.07.18", 5, -3}};
    for (const Case& c : cases) {
        std::vector<uint8_t> f = build_synthetic_xm();
        std::memset(&f[kTrackerName], 0, 20);
        std::memcpy(&f[kTrackerName], c.tracker, std::strlen(c.tracker));
        f[kFinetune0] = 5;
        f[kFinetune1] = static_cast<uint8_t>(-3);
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        CHECK(formats::xm::load(src.as_byte_source(), mem, song, nullptr));
        CHECK_EQ(song.samples[0].finetune, c.want0);
        CHECK_EQ(song.samples[1].finetune, c.want1);
        memory::track_memory_destroy(mem);
    }
}

// Правила заголовка XM, как у OpenMPT: закон панорамы по имени трекера;
// огибающая не больше 12 точек, удержание за таблицей и петля с концом
// раньше начала выключены; restartPos за order - 0; сэмпл ModPlug-ADPCM
// (8 бит, reserved 0xAD, данных 16 + (length + 1) / 2) пропущен, следующий
// разобран; у 16 бит тот же reserved - обычный PCM.
void test_header_rules() {
    std::printf("test_xm_header_rules\n");
    auto load = [](std::vector<uint8_t>& f, memory::TrackMemory& mem, Song& song) {
        memory::track_memory_create(mem);
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        return formats::xm::load(src.as_byte_source(), mem, song, nullptr);
    };
    {
        struct Case {
            const char* tracker;
            bool ft2_pan;
            bool ft2_quirks;
        };
        const Case cases[] = {{"FastTracker v2.00", true, true},   {"MilkyTracker 1.02.00", true, false}, {"MilkyTracker        ", false, false},
                              {"OpenMPT 1.22.07.19", true, false}, {"OpenMPT 1.22.07.18", false, false},  {"OpenMPT 1.17RC2", false, false},
                              {"ModPlug Tracker", false, false}};
        for (const Case& c : cases) {
            std::vector<uint8_t> f = build_synthetic_xm();
            std::memset(&f[38], 0, 20);
            std::memcpy(&f[38], c.tracker, std::strlen(c.tracker));
            memory::TrackMemory mem;
            Song song;
            CHECK(load(f, mem, song));
            CHECK(song.pan_law == (c.ft2_pan ? Song::PanLaw::Ft2Sqrt : Song::PanLaw::Linear));
            CHECK_EQ((song.quirks & soundsinth::model::kQuirkXmFt2ArpeggioTable) != 0, c.ft2_quirks);
            memory::track_memory_destroy(mem);
        }
    }
    {
        constexpr size_t kVolPoints = 335, kVolSustain = 337, kVolLoopStart = 338, kVolLoopEnd = 339, kVolFlags = 343;
        std::vector<uint8_t> f = build_synthetic_xm();
        f[kVolPoints]          = 20;
        f[kVolSustain]         = 12;
        f[kVolLoopStart]       = 3;
        f[kVolLoopEnd]         = 1;
        f[kVolFlags]           = 0x01 | 0x02 | 0x04;
        memory::TrackMemory mem;
        Song song;
        CHECK(load(f, mem, song));
        const soundsinth::model::Envelope* e = song.instruments[0].volume_envelope;
        CHECK(e != nullptr);
        if (e) {
            CHECK_EQ(e->point_count, static_cast<uint8_t>(12));
            CHECK(!e->sustain_enabled);
            CHECK(!e->loop_enabled);
        }
        memory::track_memory_destroy(mem);
    }
    for (const uint16_t restart : {uint16_t(1), uint16_t(5)}) {
        std::vector<uint8_t> f = build_synthetic_xm();
        f[60]                  = 22;                            // headerSize: таблица order на 2 позиции
        f[64]                  = 2;                             // orders
        f[66]                  = static_cast<uint8_t>(restart); // restartPos
        f.insert(f.begin() + 81, 0);                            // order[1] = паттерн 0
        memory::TrackMemory mem;
        Song song;
        CHECK(load(f, mem, song));
        CHECK_EQ(song.order_count, static_cast<uint16_t>(2));
        CHECK_EQ(song.restart_position, static_cast<uint16_t>(restart == 1 ? 1 : 0));
        memory::track_memory_destroy(mem);
    }
    constexpr size_t kSample0 = 373, kPcm0 = 373 + 80;
    for (const bool wide : {false, true}) {
        std::vector<uint8_t> f = build_synthetic_xm();
        f[kSample0 + 17]       = 0xAD;
        if (wide) {
            f[kSample0 + 14] = 0x10;
        } else {
            f[kSample0] = 10;                                // length 10 -> данных 21
            f.insert(f.begin() + kPcm0 + 6, 15, uint8_t(0)); // 6 -> 21 байт
        }
        memory::TrackMemory mem;
        Song song;
        CHECK(load(f, mem, song));
        CHECK_EQ(song.samples[0].unsupported_codec, !wide);
        CHECK_EQ(memory::sample_cache_find(mem.sample_cache, 0) != nullptr, wide);
        const memory::SampleCacheEntry* e1 = memory::sample_cache_find(mem.sample_cache, 1);
        CHECK(e1 != nullptr);
        if (e1) {
            const auto* p        = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, e1->first_page));
            const int8_t want[4] = {50, -50, 30, 10};
            for (int i = 0; i < 4; ++i)
                CHECK_EQ(p[i], want[i]);
        }
        memory::track_memory_destroy(mem);
    }
}

using LoadFn = bool (*)(formats::ByteSource, memory::TrackMemory&, Song&, const char**, bool);

// Опубликованных сэмплов после полной загрузки первых size байт файла.
uint32_t published_samples(LoadFn load, const std::vector<uint8_t>& file, uint32_t size) {
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    formats::MemoryByteSource src(file.data(), size);
    uint32_t n = 0;
    if (load(src.as_byte_source(), mem, song, nullptr, false)) {
        for (uint16_t i = 0; i < song.sample_count; ++i) {
            if (memory::sample_cache_find(mem.sample_cache, i) != nullptr) ++n;
        }
    }
    memory::track_memory_destroy(mem);
    return n;
}

// Обрезанный файл при полной загрузке теряет только оборванный сэмпл, а не
// все следующие за первым неудачным чтением. У IT оборванный сжатый сэмпл
// не теряется вовсе: расшифрованное остаётся, хвост - нули, как у OpenMPT.
void test_truncated_file_keeps_samples() {
    std::printf("test_truncated_file_keeps_samples\n");
    struct Case {
        const char* path;
        LoadFn load;
        uint32_t lost;
    };
    const Case cases[] = {
        {"SD/test_music/xm/001.xm", formats::xm::load, 1},
        {"SD/test_music/it/00009.it", formats::it::load, 0},
        {"SD/test_music/s3m/2nd_pm.s3m", formats::s3m::load, 1},
    };
    for (const Case& c : cases) {
        std::ifstream in(c.path, std::ios::binary);
        if (!in) {
            std::printf("  SKIP (no file): %s\n", c.path);
            continue;
        }
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const uint32_t whole = published_samples(c.load, file, static_cast<uint32_t>(file.size()));
        const uint32_t cut   = published_samples(c.load, file, static_cast<uint32_t>(file.size()) - 64);
        CHECK(whole > 1);
        CHECK_EQ(cut + c.lost, whole);
    }
}

// Синтетика с другим паттерном: rows строк, packed - упакованные данные
// двух каналов.
std::vector<uint8_t> xm_with_pattern(const std::vector<uint8_t>& packed, uint16_t rows) {
    std::vector<uint8_t> f = build_synthetic_xm();
    constexpr size_t kRows = 86, kSize = 88, kData = 90;
    const size_t old = f[kSize] | (f[kSize + 1] << 8);
    f[kRows]         = static_cast<uint8_t>(rows & 0xFF);
    f[kRows + 1]     = static_cast<uint8_t>(rows >> 8);
    f[kSize]         = static_cast<uint8_t>(packed.size() & 0xFF);
    f[kSize + 1]     = static_cast<uint8_t>(packed.size() >> 8);
    f.erase(f.begin() + kData, f.begin() + kData + old);
    f.insert(f.begin() + kData, packed.begin(), packed.end());
    return f;
}

// Колонка громкости XM - как у OpenMPT (Load_xm.cpp), независимо от загрузчика.
soundsinth::model::VolumeColumnCommand xm_volume_ref(uint8_t v) {
    using T = VolumeColumnType;
    if (v >= 0x10 && v <= 0x50) return {T::SetVolume, static_cast<uint8_t>(v - 0x10)};
    if (v < 0x60) return {T::None, 0};
    static const T kTrans[10] = {T::SlideDown,    T::SlideUp,    T::FineSlideDown, T::FineSlideUp,   T::VibratoSpeed,
                                 T::VibratoDepth, T::SetPanning, T::PanSlideLeft,  T::PanSlideRight, T::TonePorta};
    const T t                 = kTrans[(v - 0x60) >> 4];
    const uint8_t p           = v & 0x0F;
    return {t, static_cast<uint8_t>(t == T::SetPanning ? p * 4 : p)};
}

// Эффекты XM -> Effect, как ConvertModCommand OpenMPT: Dxx в BCD, Fxx до
// 0x20 - скорость; Gxx x2 с потолком 128 (у OpenMPT удвоение при
// воспроизведении), Cxx до 64. 18, 19, 22-24, 26, 28, 30-32 и 36+ не
// разбираются (у OpenMPT 32 - пустышка, 36/37 - SmoothMidi); у X только X1x
// и X2x. Колонка громкости - все 256 байт.
void test_effect_letters() {
    std::printf("test_xm_effect_letters\n");
    using soundsinth::model::SlideRate;
    struct Row {
        uint8_t cmd, param;
        Effect type;
        uint8_t want;
        SlideRate rate;
    };
    const SlideRate T = SlideRate::PerTick, F = SlideRate::Fine, X = SlideRate::ExtraFine;
    const Row table[] = {
        {0, 0x37, Effect::Arpeggio, 0x37, T},
        {1, 0x12, Effect::PortaUp, 0x12, T},
        {2, 0x12, Effect::PortaDown, 0x12, T},
        {3, 0x20, Effect::TonePorta, 0x20, T},
        {4, 0x44, Effect::Vibrato, 0x44, T},
        {5, 0x02, Effect::TonePortaVolSlide, 0x02, T},
        {6, 0x20, Effect::VibratoVolSlide, 0x20, T},
        {7, 0x44, Effect::Tremolo, 0x44, T},
        {8, 0x80, Effect::SetPanning, 0x80, T},
        {9, 0x10, Effect::SampleOffset, 0x10, T},
        {10, 0x0F, Effect::VolumeSlide, 0x0F, T},
        {11, 0x03, Effect::PositionJump, 3, T},
        {12, 0x30, Effect::SetVolume, 0x30, T},
        {12, 0x50, Effect::SetVolume, 64, T},
        {13, 0x15, Effect::PatternBreak, 15, T},
        {15, 0x1F, Effect::SetSpeed, 0x1F, T},
        {15, 0x20, Effect::SetTempo, 0x20, T},
        {16, 0x20, Effect::SetGlobalVolume, 0x40, T},
        {16, 0x41, Effect::SetGlobalVolume, 128, T},
        {17, 0x21, Effect::GlobalVolumeSlide, 0x21, T},
        {20, 0x05, Effect::KeyOff, 5, T},
        {21, 0x10, Effect::SetEnvelopePosition, 0x10, T},
        {25, 0x02, Effect::PanningSlide, 0x02, T},
        {27, 0x13, Effect::RetriggerXm, 0x13, T},
        {29, 0x21, Effect::Tremor, 0x21, T},
        {33, 0x15, Effect::PortaUp, 5, X},
        {33, 0x25, Effect::PortaDown, 5, X},
        {33, 0x55, Effect::None, 0, T},
        {34, 0x44, Effect::Panbrello, 0x44, T},
        {35, 0x50, Effect::SetMidiMacro, 0x50, T},
        {14, 0x01, Effect::SetFilter, 1, T},
        {14, 0x12, Effect::PortaUp, 2, F},
        {14, 0x23, Effect::PortaDown, 3, F},
        {14, 0x31, Effect::GlissandoControl, 1, T},
        {14, 0x42, Effect::SetVibratoWaveform, 2, T},
        {14, 0x55, Effect::SetFinetune, 5, T},
        {14, 0x63, Effect::PatternLoop, 3, T},
        {14, 0x72, Effect::SetTremoloWaveform, 2, T},
        {14, 0x8C, Effect::SetPanning4Bit, 0xC, T},
        {14, 0x93, Effect::Retrigger, 3, T},
        {14, 0xA4, Effect::VolumeSlide, 0x40, F},
        {14, 0xB4, Effect::VolumeSlide, 0x04, F},
        {14, 0xC5, Effect::NoteCut, 5, T},
        {14, 0xD6, Effect::NoteDelay, 6, T},
        {14, 0xE7, Effect::PatternDelay, 7, T},
        {14, 0xF1, Effect::None, 0, T},
        {18, 0x10, Effect::None, 0, T},
        {19, 0x10, Effect::None, 0, T},
        {22, 0x10, Effect::None, 0, T},
        {24, 0x10, Effect::None, 0, T},
        {26, 0x10, Effect::None, 0, T},
        {28, 0x10, Effect::None, 0, T},
        {32, 0x10, Effect::None, 0, T},
        {36, 0x10, Effect::None, 0, T},
    };
    constexpr size_t kRows = sizeof(table) / sizeof(table[0]);
    std::vector<uint8_t> packed;
    for (size_t row = 0; row < 256; ++row) {
        if (row < kRows) {
            packed.insert(packed.end(), {0x9C, static_cast<uint8_t>(row), table[row].cmd, table[row].param});
        } else {
            packed.insert(packed.end(), {0x84, static_cast<uint8_t>(row)});
        }
        packed.push_back(0x80); // канал 1 пуст
    }
    const std::vector<uint8_t> f = xm_with_pattern(packed, 256);
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
    const bool ok = formats::xm::load(src.as_byte_source(), mem, song, nullptr);
    CHECK(ok);
    if (ok) {
        const soundsinth::model::Pattern& pat = song.patterns[0];
        CHECK_EQ(pat.row_count, static_cast<uint16_t>(256));
        patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, pat.psram_offset), pat.row_count, pat.channel_count);
        uint32_t bad = 0;
        for (size_t row = 0; row < 256; ++row) {
            soundsinth::model::PatternCell cells[2];
            reader.read_row(static_cast<uint16_t>(row), cells);
            const soundsinth::model::VolumeColumnCommand want_vol = xm_volume_ref(static_cast<uint8_t>(row));
            if (cells[0].volume.type != want_vol.type || cells[0].volume.param != want_vol.param) {
                if (++bad <= 5) {
                    std::printf("  volume %02X: type %d param %u, expected %d %u\n", static_cast<unsigned>(row), static_cast<int>(cells[0].volume.type),
                                cells[0].volume.param, static_cast<int>(want_vol.type), want_vol.param);
                }
            }
            if (row >= kRows) continue;
            const soundsinth::model::EffectCommand& e = cells[0].effect;
            const Row& w                              = table[row];
            const bool none                           = w.type == Effect::None;
            if (e.type != w.type || (!none && (e.param != w.want || e.rate != w.rate))) {
                if (++bad <= 5) {
                    std::printf("  effect %u param %02X: type %d param %02X, expected %d %02X\n", w.cmd, w.param, static_cast<int>(e.type), e.param,
                                static_cast<int>(w.type), w.want);
                }
            }
        }
        CHECK_EQ(bad, 0u);
    }
    memory::track_memory_destroy(mem);
}

// Паттерн без данных (packedDataSize 0) - заданное число пустых строк;
// номер паттерна в order за pattern_count - пропуск позиции.
void test_empty_pattern_and_order_skip() {
    std::printf("test_xm_empty_pattern_and_order_skip\n");
    std::vector<uint8_t> f = xm_with_pattern({}, 16);
    f[60]                  = 22; // headerSize: таблица order на 2 позиции
    f[64]                  = 2;  // orders
    f.insert(f.begin() + 81, 5); // order[1] = паттерн 5 при одном паттерне
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
    const bool ok = formats::xm::load(src.as_byte_source(), mem, song, nullptr);
    CHECK(ok);
    if (ok) {
        CHECK_EQ(song.order_count, static_cast<uint16_t>(2));
        CHECK_EQ(song.order[0], static_cast<uint16_t>(0));
        CHECK_EQ(song.order[1], soundsinth::model::kOrderSkip);
        const soundsinth::model::Pattern& pat = song.patterns[0];
        CHECK_EQ(pat.row_count, static_cast<uint16_t>(16));
        patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, pat.psram_offset), pat.row_count, pat.channel_count);
        bool empty = true;
        for (uint16_t row = 0; row < 16; ++row) {
            soundsinth::model::PatternCell cells[2];
            reader.read_row(row, cells);
            for (const soundsinth::model::PatternCell& c : cells) {
                if (c.note != soundsinth::model::kNoteNone || c.instrument != 0 || c.effect.type != Effect::None || c.volume.type != VolumeColumnType::None) {
                    empty = false;
                }
            }
        }
        CHECK(empty);
    }
    memory::track_memory_destroy(mem);
}

// Автовибрато инструмента не воспроизводится - считается при глубине не 0.
void test_autovibrato_counted() {
    std::printf("test_xm_autovibrato_counted\n");
    constexpr size_t kVibDepth = 347;
    for (const uint8_t depth : {uint8_t(0), uint8_t(4)}) {
        std::vector<uint8_t> f = build_synthetic_xm();
        f[kVibDepth]           = depth;
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        CHECK(formats::xm::load(src.as_byte_source(), mem, song, nullptr));
        CHECK_EQ(soundsinth::model::g_tracker_load_stats.ignored_autovibrato, static_cast<uint16_t>(depth ? 1 : 0));
        CHECK_EQ(soundsinth::model::g_tracker_load_stats.samples_failed, static_cast<uint16_t>(0));
        memory::track_memory_destroy(mem);
    }
}

} // namespace

void run_xm_loader_tests() {
    test_autovibrato_counted();
    test_effect_letters();
    test_empty_pattern_and_order_skip();
    test_synthetic_exact();
    test_real_small_file_smoke();
    test_truncated_file_keeps_samples();
    test_stereo_sample_left_channel();
    test_envelope_empty_and_loop_clamp();
    test_finetune_precision();
    test_pcm_delta_wraps();
    test_header_rules();
}
