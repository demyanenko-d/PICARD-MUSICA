#include "testing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/formats/load_stats.h"
#include "core/model/song.h"
#include "core/formats/it.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_reader.h"

using namespace soundsinth;
using soundsinth::model::Effect;
using soundsinth::model::Song;
using soundsinth::model::VolumeColumnType;

namespace {

// Собирает минимальный синтетический IT вручную, поле за полем. В отличие
// от S3M, указатели в IT абсолютные (см. formats/it.cpp), а не
// парапоинтеры в 16-байтных параграфах, поэтому backpatch не нужен:
// данные пишутся последовательно, уже известные смещения (текущий
// f.size()) подставляются по ходу.
// with_signed_pan_pitch_env: при true panenv включена с отрицательным
// сырым значением точки (так хранит .it, см. formats/it.cpp value_offset
// и сверку с OpenMPT soundlib/ITTools.cpp ITEnvelope::ConvertToMPT,
// envOffset=32 для pan/pitch), а pitchenv включена с битом envFilter
// (0x80) и должна быть отфильтрована загрузчиком (это огибающая фильтра,
// а не питча, см. .cpp). При false (по умолчанию) поведение прежнее: обе
// выключены, test_synthetic_exact остаётся тем же.
// extra_samples - столько пустых слотов сэмплов (указатель 0) после
// единственного настоящего; ext_magic ("XTPM"/"MPTX") - метка расширения на
// смещении 550 заголовка инструмента, за заголовком - keyboard_hi (120
// старших байтов номеров сэмплов), если он задан. pattern - свои упакованные
// данные паттерна на rows строк вместо стандартных четырёх.
std::vector<uint8_t> build_synthetic_it(bool with_signed_pan_pitch_env = false, uint16_t extra_samples = 0,
                                        const char* ext_magic = nullptr, const uint8_t* keyboard_hi = nullptr,
                                        const std::vector<uint8_t>* pattern = nullptr, uint16_t rows = 4) {
    std::vector<uint8_t> f;
    auto put8 = [&](uint8_t v) { f.push_back(v); };
    auto put16 = [&](uint16_t v) { f.push_back(static_cast<uint8_t>(v & 0xFF)); f.push_back(static_cast<uint8_t>(v >> 8)); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); };
    auto put_str = [&](const char* s, size_t n) {
        size_t len = std::strlen(s);
        for (size_t i = 0; i < n; ++i) f.push_back(i < len ? static_cast<uint8_t>(s[i]) : 0);
    };

    put_str("IMPM", 4);
    put_str("ITTEST", 26);
    put8(4); put8(16); // highlight minor/major
    put16(1);  // ordnum
    put16(1);  // insnum
    put16(static_cast<uint16_t>(1 + extra_samples));  // smpnum
    put16(1);  // patnum
    put16(0x0214); // cwtv
    put16(0x0214); // cmwt
    put16(0x04);   // flags: instrumentMode
    put16(0);      // special
    put8(128);     // globalvol
    put8(48);      // mv
    put8(6);       // speed
    put8(125);     // tempo
    put8(0);       // sep
    put8(0);       // pwd
    put16(0);      // msglength
    put32(0);      // msgoffset
    put32(0);      // reserved
    for (int i = 0; i < 64; ++i) put8(32); // chnpan
    for (int i = 0; i < 64; ++i) put8(64); // chnvol
    CHECK_EQ(f.size(), static_cast<size_t>(192));

    put8(0); // order[0] = паттерн 0

    const size_t inst_ptr_pos = f.size();
    put32(0);
    const size_t smp_ptr_pos = f.size();
    put32(0);
    for (uint16_t i = 0; i < extra_samples; ++i) put32(0);
    const size_t pat_ptr_pos = f.size();
    put32(0);

    const uint32_t inst_offset = static_cast<uint32_t>(f.size());
    put_str("IMPI", 4);
    for (int i = 0; i < 13; ++i) put8(0); // filename
    put8(1);  // nna = Continue
    put8(2);  // dct = Sample
    put8(1);  // dca = Off
    put16(100); // fadeout
    put8(0);  // pps
    put8(0);  // ppc
    put8(100); // gbv (global volume)
    put8(0);  // dfp
    put8(0);  // rv
    put8(0);  // rp
    put16(0); // trkvers
    put8(1);  // nos
    put8(0);  // reserved1
    for (int i = 0; i < 26; ++i) put8(0); // name
    put8(0); put8(0); put8(0); put8(0);   // ifc/ifr/mch/mpr
    put16(0); // mbank
    // keyboard[240]: все ноты -> сэмпл 1 (единственный), питч тождественный
    // (note[n]==n), без переразметки клавиш, см. instrument.h
    // KeymapRange::note_offset. Раньше здесь для всех клавиш стояла константа
    // put8(60); с тех пор как учитывается note-байт, а не только sample, это
    // потребовало бы полной keymap-таблицы, а тест проверяет другое
    // ("единственный сэмпл -> без keymap").
    for (int n = 0; n < 120; ++n) { put8(static_cast<uint8_t>(n)); put8(1); }
    CHECK_EQ(f.size() - inst_offset, static_cast<size_t>(64 + 240));

    // volenv: enabled(0x01), 2 точки, sustain/loop не используются.
    put8(0x01); // flags
    put8(2);    // num
    put8(0); put8(0); // lpb/lpe
    put8(0); put8(0); // slb/sle
    // data[25] узлов по 3 байта (value, tick LE) - используются первые 2.
    put8(static_cast<uint8_t>(static_cast<int8_t>(64))); put16(0);  // point0: value=64, tick=0
    put8(static_cast<uint8_t>(static_cast<int8_t>(0)));  put16(20); // point1: value=0, tick=20
    for (int i = 2; i < 25; ++i) { put8(0); put16(0); }
    put8(0); // reserved
    CHECK_EQ(f.size() - inst_offset, static_cast<size_t>(64 + 240 + 82));

    if (with_signed_pan_pitch_env) {
        // panenv: enabled(0x01), 1 точка, сырое значение -20 (файловая шкала IT,
        // знаковая -32..32); на выходе загрузчика ожидается 32+(-20)=12 (см.
        // formats/it.cpp value_offset).
        put8(0x01); put8(1); put8(0); put8(0); put8(0); put8(0);
        put8(static_cast<uint8_t>(static_cast<int8_t>(-20))); put16(0);
        for (int i = 1; i < 25; ++i) { put8(0); put16(0); }
        put8(0);
        // pitchenv: enabled(0x01) | envFilter(0x80) - это огибающая фильтра, а не
        // питча: загрузчик должен вернуть nullptr для Instrument::pitch_envelope
        // (см. .cpp), несмотря на envEnabled.
        put8(0x01 | 0x80); put8(1); put8(0); put8(0); put8(0); put8(0);
        put8(static_cast<uint8_t>(static_cast<int8_t>(10))); put16(0);
        for (int i = 1; i < 25; ++i) { put8(0); put16(0); }
        put8(0);
    } else {
        // panenv/pitchenv: выключены (flags=0), остальное нулями.
        for (int e = 0; e < 2; ++e) {
            put8(0); put8(0); put8(0); put8(0); put8(0); put8(0);
            for (int i = 0; i < 25; ++i) { put8(0); put16(0); }
            put8(0);
        }
    }
    if (ext_magic != nullptr) {
        put_str(ext_magic, 4);
    } else {
        put8(0); put8(0); put8(0); put8(0); // dummy[4]
    }
    CHECK_EQ(f.size() - inst_offset, static_cast<size_t>(554));
    if (keyboard_hi != nullptr) {
        for (int n = 0; n < 120; ++n) put8(keyboard_hi[n]);
    }

    const uint32_t smp_offset = static_cast<uint32_t>(f.size());
    put_str("IMPS", 4);
    for (int i = 0; i < 13; ++i) put8(0); // filename
    put8(64);  // gvl
    put8(0x01 | 0x10); // flags: dataPresent + loop
    put8(48);  // vol
    for (int i = 0; i < 26; ++i) put8(0); // name
    put8(0x01); // cvt: signed PCM
    put8(0);    // dfp
    put32(4);   // length
    put32(0);   // loopbegin
    put32(4);   // loopend
    put32(8363); // C5Speed
    put32(0);   // susloopbegin
    put32(0);   // susloopend
    const size_t sample_pointer_pos = f.size();
    put32(0);   // samplepointer - заполним ниже
    put8(0); put8(0); put8(0); put8(0); // vis/vid/vir/vit
    CHECK_EQ(f.size() - smp_offset, static_cast<size_t>(80));

    const uint32_t pat_offset = static_cast<uint32_t>(f.size());
    std::vector<uint8_t> packed;
    auto pb = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) packed.push_back(b); };
    // Row0: ch0 note=48+instr=1, новая маска 0x03.
    pb({0x81, 0x03, 48, 1});
    pb({0x00}); // конец строки 0
    // Row1: ch0 повтор note+instr, новый эффект PatternBreak(3,0x12). У IT
    // параметр не в BCD (в отличие от MOD/S3M/XM): ожидается param=18, а не
    // BCD-декодированные 12.
    pb({0x81, 0x38, 3, 0x12});
    pb({0x00});
    // Row2: ch1 volume=200 -> TonePorta(7).
    pb({0x82, 0x04, 200});
    pb({0x00});
    // Row3: ch0, маска не меняется (переиспользуется 0x38 из row1), новый
    // эффект SetSpeed(1,10).
    pb({0x01, 1, 10});
    pb({0x00});
    if (pattern != nullptr) packed = *pattern;

    put16(static_cast<uint16_t>(packed.size())); // packed length
    put16(rows);                                  // numRows
    put32(0);                                     // reserved
    for (uint8_t b : packed) put8(b);

    const uint32_t sample_data_offset = static_cast<uint32_t>(f.size());
    const int8_t pcm[4] = {10, -10, 20, -20};
    for (int8_t v : pcm) put8(static_cast<uint8_t>(v));

    // --- Заполнить указатели ---
    auto patch32 = [&](size_t pos, uint32_t v) {
        f[pos] = static_cast<uint8_t>(v & 0xFF);
        f[pos + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        f[pos + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        f[pos + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    patch32(inst_ptr_pos, inst_offset);
    patch32(smp_ptr_pos, smp_offset);
    patch32(pat_ptr_pos, pat_offset);
    patch32(sample_pointer_pos, sample_data_offset);

    return f;
}

void test_synthetic_exact() {
    std::printf("test_it_loader_synthetic_exact\n");

    const std::vector<uint8_t> file = build_synthetic_it();
    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));

    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok = formats::it::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);
    if (!ok) { memory::track_memory_destroy(mem); return; }

    CHECK_EQ(song.pattern_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.sample_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.instrument_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.order_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.order[0], static_cast<uint16_t>(0));
    CHECK_EQ(song.channel_count, static_cast<uint8_t>(2)); // максимальный использованный канал в паттерне
    CHECK_EQ(song.default_global_volume, static_cast<uint8_t>(128));
    CHECK_EQ(song.sample_preamp, static_cast<uint8_t>(48)); // "mv" в синтетике - см. build_synthetic_it
    // Без линейных слайдов c5_speed звучит на C-5 (60), сетка периодов - от 48.
    CHECK(song.frequency_model == soundsinth::model::FrequencyModel::Amiga);
    CHECK_EQ(song.samples[0].relative_note, static_cast<int8_t>(-12));

    const auto& inst = song.instruments[0];
    CHECK(inst.nna == soundsinth::model::NewNoteAction::Continue);
    CHECK(inst.dct == soundsinth::model::DuplicateCheckType::Sample);
    CHECK(inst.dca == soundsinth::model::DuplicateCheckAction::Off);
    CHECK_EQ(inst.fadeout_rate, 100u << 6);
    CHECK_EQ(inst.global_volume, static_cast<uint8_t>(100));
    CHECK(inst.note_to_sample_ranges == nullptr); // единственный сэмпл -> без keymap
    CHECK_EQ(inst.default_sample_index, static_cast<uint16_t>(0));
    CHECK(inst.volume_envelope != nullptr);
    CHECK(inst.volume_envelope->enabled);
    CHECK_EQ(inst.volume_envelope->point_count, static_cast<uint8_t>(2));
    CHECK_EQ(inst.volume_envelope->points[0].value, static_cast<int16_t>(64));
    CHECK_EQ(inst.volume_envelope->points[1].tick, static_cast<uint16_t>(20));
    CHECK(inst.panning_envelope == nullptr);

    CHECK_EQ(song.samples[0].length_samples, 4u);
    CHECK(song.samples[0].loop_enabled);

    // Паттерн всегда упакован с channel_count=kMaxChannels(64), см.
    // formats/it.cpp (упрощение: нет отдельного прохода ради точного числа
    // активных каналов на паттерн).
    patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, song.patterns[0].psram_offset),
                                    song.patterns[0].row_count, song.patterns[0].channel_count);
    std::vector<soundsinth::model::PatternCell> cells(song.patterns[0].channel_count);

    reader.read_row(0, cells.data());
    CHECK_EQ(cells[0].note, static_cast<uint8_t>(48));
    CHECK_EQ(cells[0].instrument, static_cast<uint8_t>(1));

    reader.read_row(1, cells.data());
    CHECK_EQ(cells[0].note, static_cast<uint8_t>(48));      // повтор через mask&0x10
    CHECK_EQ(cells[0].instrument, static_cast<uint8_t>(1)); // повтор через mask&0x20
    CHECK(cells[0].effect.type == Effect::PatternBreak);
    CHECK_EQ(cells[0].effect.param, static_cast<uint8_t>(18)); // param=0x12=18 напрямую, а не BCD-декодированные 12

    reader.read_row(2, cells.data());
    CHECK(cells[1].volume.type == VolumeColumnType::TonePorta);
    CHECK_EQ(cells[1].volume.param, static_cast<uint8_t>(7));

    reader.read_row(3, cells.data());
    CHECK_EQ(cells[0].note, static_cast<uint8_t>(48));
    CHECK(cells[0].effect.type == Effect::SetSpeed);
    CHECK_EQ(cells[0].effect.param, static_cast<uint8_t>(10));

    auto* cache_entry = memory::sample_cache_find(mem.sample_cache, 0);
    CHECK(cache_entry != nullptr);
    if (cache_entry) {
        const auto* p = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, cache_entry->first_page));
        const int8_t want[4] = {10, -10, 20, -20};
        for (int i = 0; i < 4; ++i) CHECK_EQ(p[i], want[i]);
    }

    memory::track_memory_destroy(mem);
}

// PCM IT: беззнаковый (cvt без бита 0) {138, 118, 148, 108} -> {10, -10,
// 20, -20}; 16 бит знаковый и беззнаковый; стерео (флаг 0x04) - левый канал.
void test_pcm_variants() {
    std::printf("test_it_pcm_variants\n");
    struct Case { uint8_t flags_add; uint8_t cvt; std::vector<uint8_t> data; bool wide; };
    const Case cases[] = {
        {0x00, 0x00, {138, 118, 148, 108}, false},
        {0x02, 0x01, {0xE8, 0x03, 0x18, 0xFC, 0xD0, 0x07, 0x30, 0xF8}, true},  // 1000, -1000, 2000, -2000
        {0x02, 0x00, {0xE8, 0x83, 0x18, 0x7C, 0xD0, 0x87, 0x30, 0x78}, true},  // то же + 32768
        {0x04, 0x01, {10, 246, 20, 236, 9, 9, 9, 9}, false},                   // левый {10, -10, 20, -20}
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> file = build_synthetic_it();
        const char kImps[4] = {'I', 'M', 'P', 'S'};
        auto it = std::search(file.begin(), file.end(), kImps, kImps + 4);
        CHECK(it != file.end());
        if (it == file.end()) return;
        const size_t smp = static_cast<size_t>(it - file.begin());
        file[smp + 18] = static_cast<uint8_t>(file[smp + 18] | c.flags_add);
        file[smp + 46] = c.cvt;
        file.resize(file.size() - 4); // PCM синтетики - последние 4 байта
        file.insert(file.end(), c.data.begin(), c.data.end());
        formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        CHECK(formats::it::load(src.as_byte_source(), mem, song, nullptr));
        CHECK_EQ(song.samples[0].length_samples, 4u);
        const memory::SampleCacheEntry* e = memory::sample_cache_find(mem.sample_cache, 0);
        CHECK(e != nullptr);
        if (e && c.wide) {
            CHECK(song.samples[0].resident_encoding == soundsinth::model::ResidentEncoding::Raw16);
            const uint8_t* p = memory::psram_page_ptr(mem.psram, e->first_page);
            const int16_t want[4] = {1000, -1000, 2000, -2000};
            for (int i = 0; i < 4; ++i) CHECK_EQ(static_cast<int16_t>(p[2 * i] | (p[2 * i + 1] << 8)), want[i]);
        } else if (e) {
            const auto* p = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, e->first_page));
            const int8_t want[4] = {10, -10, 20, -20};
            for (int i = 0; i < 4; ++i) CHECK_EQ(p[i], want[i]);
        }
        memory::track_memory_destroy(mem);
    }
}

void test_real_small_file_smoke() {
    std::printf("test_it_loader_real_small_file_smoke\n");

    std::ifstream in("SD/test_music/it/00009.it", std::ios::binary);
    if (!in) {
        std::printf("  файл не найден — ПРОПУСК (запуск не из корня репозитория?)\n");
        return;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(file.size() > 192);

    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok = formats::it::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);
    if (ok) {
        CHECK(song.pattern_count >= 1);
        CHECK(song.order_count >= 1);
        for (uint16_t p = 0; p < song.pattern_count; ++p) {
            CHECK(song.patterns[p].psram_offset != soundsinth::model::Pattern::kInvalidOffset);
        }
    }

    memory::track_memory_destroy(mem);
}

// Найденный баг: pan/pitch-огибающие IT хранят в файле знаковые -32..32
// (0 = нейтрально), а загрузчик раньше писал их в общую шкалу движка
// 0..64 без сдвига +32 (см. formats/it.cpp value_offset, сверено с
// OpenMPT soundlib/ITTools.cpp ITEnvelope::ConvertToMPT envOffset=32), и
// отрицательное сырое значение уходило в движок мусором от
// переполнения uint8_t. Кроме того, pitch-огибающая с битом envFilter
// (0x80) - это огибающая фильтра (IT использует ту же структуру для среза
// частоты): загрузчик должен вернуть nullptr, а не подставлять данные
// фильтра как питч-бенд.
void test_pan_pitch_envelope_offset_and_filter_flag() {
    std::printf("test_pan_pitch_envelope_offset_and_filter_flag\n");

    const std::vector<uint8_t> file = build_synthetic_it(/*with_signed_pan_pitch_env=*/true);
    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));

    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok = formats::it::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);
    if (ok) {
        const auto& inst = song.instruments[0];
        CHECK(inst.panning_envelope != nullptr);
        CHECK_EQ(inst.panning_envelope->point_count, static_cast<uint8_t>(1));
        CHECK_EQ(inst.panning_envelope->points[0].value, static_cast<int16_t>(12)); // 32+(-20)=12, а не мусор от переполнения
        CHECK(inst.pitch_envelope == nullptr); // envFilter - фильтр, а не питч, не подключаем
    }

    memory::track_memory_destroy(mem);
}

// ChnPan: бит 0x80 - канал выключен (Song::channel_muted), младшие 7 бит -
// панорама, как Load_it.cpp в OpenMPT.
void test_chnpan_mute_bit() {
    std::printf("test_it_chnpan_mute_bit\n");

    std::vector<uint8_t> file = build_synthetic_it();
    file[0x40] = 0x80 | 16;
    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));

    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok = formats::it::load(mbs.as_byte_source(), mem, song, &error);
    CHECK(ok);
    if (ok) {
        CHECK(song.channel_muted == 1);
        CHECK_EQ(song.channel_pan[0], 16);
        CHECK(song.channel_surround == 0);
    }
    memory::track_memory_destroy(mem);

    // ChnPan 100 - surround из центра, вместе с битом выключения тоже.
    for (uint8_t raw : {uint8_t(100), uint8_t(0x80 | 100)}) {
        file[0x40] = raw;
        formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
        memory::track_memory_create(mem);
        Song s;
        CHECK(formats::it::load(src.as_byte_source(), mem, s, &error));
        CHECK(s.channel_surround == 1);
        CHECK_EQ(s.channel_pan[0], 32);
        CHECK(s.channel_muted == ((raw & 0x80) ? 1u : 0u));
        memory::track_memory_destroy(mem);
    }
}

// Сведение ModPlug (MixLevels::Original) по подписи заголовка, как в
// Load_it.cpp: метка "OMPT" - только при cwtv 0x5xxx.
void test_modplug_signature() {
    std::printf("test_it_modplug_signature\n");
    using MixLevels = Song::MixLevels;
    struct Case {
        uint16_t cwtv, cmwt;
        uint32_t reserved;
        MixLevels expected;
    };
    const Case cases[] = {
        {0x0217, 0x0200, 0, MixLevels::Original},
        {0x0214, 0x0214, 0x54504D4Fu, MixLevels::Compatible}, // "OMPT" без cwtv 0x5xxx
        {0x5117, 0x0214, 0x54504D4Fu, MixLevels::Original},   // OpenMPT, не режим совместимости
        {0x5117, 0x0214, 0, MixLevels::Compatible},           // OpenMPT в режиме совместимости
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> file = build_synthetic_it();
        file[0x28] = static_cast<uint8_t>(c.cwtv);
        file[0x29] = static_cast<uint8_t>(c.cwtv >> 8);
        file[0x2A] = static_cast<uint8_t>(c.cmwt);
        file[0x2B] = static_cast<uint8_t>(c.cmwt >> 8);
        for (int i = 0; i < 4; ++i) file[0x3C + i] = static_cast<uint8_t>(c.reserved >> (8 * i));
        formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        const char* error = nullptr;
        CHECK(formats::it::load(mbs.as_byte_source(), mem, song, &error));
        CHECK(song.mix_levels == c.expected);
        memory::track_memory_destroy(mem);
    }
}

} // namespace

// Несжатый 8-битный IT: бит дельты в cvt не применяется (данные - PCM), cvt
// 0xFF - ModPlug-ADPCM, сэмпл не публикуется.
void test_uncompressed_delta_and_adpcm() {
    std::printf("test_it_uncompressed_delta_and_adpcm\n");
    struct Case { uint8_t cvt; bool published; };
    const Case cases[] = {{0x05, true}, {0xFF, false}};
    for (const Case& c : cases) {
        std::vector<uint8_t> file = build_synthetic_it();
        const char kImps[4] = {'I', 'M', 'P', 'S'};
        auto it = std::search(file.begin(), file.end(), kImps, kImps + 4);
        CHECK(it != file.end());
        if (it == file.end()) return;
        *(it + 46) = c.cvt;
        formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        CHECK(formats::it::load(src.as_byte_source(), mem, song, nullptr));
        const memory::SampleCacheEntry* e = memory::sample_cache_find(mem.sample_cache, 0);
        CHECK_EQ(e != nullptr, c.published);
        if (e) {
            const auto* p = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, e->first_page));
            const int8_t want[4] = {10, -10, 20, -20};
            for (int i = 0; i < 4; ++i) CHECK_EQ(p[i], want[i]);
        }
        memory::track_memory_destroy(mem);
    }
}

// Огибающая громкости с потерянным старшим байтом тика: точки (0,64)
// (200,32) (20,0) - третья чинится до 276, как у OpenMPT; значения и номера
// петли за точками прижаты.
void test_envelope_sanitized() {
    std::printf("test_it_envelope_sanitized\n");
    std::vector<uint8_t> file = build_synthetic_it();
    const char kImpi[4] = {'I', 'M', 'P', 'I'};
    auto it = std::search(file.begin(), file.end(), kImpi, kImpi + 4);
    CHECK(it != file.end());
    if (it == file.end()) return;
    const size_t env = static_cast<size_t>(it - file.begin()) + 304;
    file[env + 0] = 0x01 | 0x02; // включена, петля
    file[env + 1] = 3;
    file[env + 2] = 1;
    file[env + 3] = 9; // конец петли за точками
    const uint8_t nodes[9] = {64, 0, 0, 32, 200, 0, 0, 20, 0};
    for (int i = 0; i < 9; ++i) file[env + 6 + i] = nodes[i];
    formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    CHECK(formats::it::load(src.as_byte_source(), mem, song, nullptr));
    const soundsinth::model::Envelope* e = song.instruments[0].volume_envelope;
    CHECK(e != nullptr);
    if (e) {
        CHECK_EQ(e->points[1].tick, static_cast<uint16_t>(200));
        CHECK_EQ(e->points[2].tick, static_cast<uint16_t>(276));
        CHECK(e->loop_enabled);
        CHECK_EQ(e->loop_end, static_cast<uint8_t>(2));
    }
    memory::track_memory_destroy(mem);
}

// Keymap: диапазон держит смещение или постоянную целевую ноту, режим - по
// первым двум клавишам. Клавиши 0..59 - нота n + 12; 60, 61 - n; 62 - нота 61
// (цель предыдущей, но смещение другое - новый диапазон); 63..99 - n; 100 -
// пробел; 101..119 - все на ноту 72 (ударные).
void test_keymap_ranges() {
    std::printf("test_it_keymap_ranges\n");
    std::vector<uint8_t> file = build_synthetic_it();
    const char kImpi[4] = {'I', 'M', 'P', 'I'};
    auto it = std::search(file.begin(), file.end(), kImpi, kImpi + 4);
    CHECK(it != file.end());
    if (it == file.end()) return;
    const size_t kb = static_cast<size_t>(it - file.begin()) + 64;
    for (int n = 0; n < 120; ++n) {
        int note = n;
        if (n < 60) note = n + 12;
        if (n == 62) note = 61;
        if (n > 100) note = 72;
        file[kb + n * 2] = static_cast<uint8_t>(note);
        file[kb + n * 2 + 1] = n == 100 ? 0 : 1;
    }
    formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    CHECK(formats::it::load(src.as_byte_source(), mem, song, nullptr));
    const soundsinth::model::Instrument& ins = song.instruments[0];
    CHECK_EQ(ins.note_to_sample_range_count, static_cast<uint8_t>(6));
    if (ins.note_to_sample_ranges != nullptr && ins.note_to_sample_range_count == 6) {
        const uint8_t starts[6] = {0, 60, 62, 63, 100, 101 | soundsinth::model::kKeymapFixedNote};
        for (int r = 0; r < 6; ++r) CHECK_EQ(ins.note_to_sample_ranges[r].start_note, starts[r]);
        CHECK_EQ(ins.note_to_sample_ranges[4].sample_index, soundsinth::model::kNoSample);
    }
    struct Case { uint8_t key; bool ok; uint8_t note; };
    const Case cases[] = {{5, true, 17}, {61, true, 61}, {62, true, 61}, {99, true, 99},
                          {100, false, 0}, {101, true, 72}, {119, true, 72}};
    for (const Case& c : cases) {
        uint16_t idx = 0xFFFF;
        uint8_t note = 0;
        CHECK_EQ(soundsinth::model::resolve_sample_index(song, 1, c.key, &idx, &note), c.ok);
        if (c.ok) {
            CHECK_EQ(idx, static_cast<uint16_t>(0));
            CHECK_EQ(note, c.note);
        }
    }
    memory::track_memory_destroy(mem);
}

// Старшие байты номеров сэмплов keymap (XTPM, у OpenMPT 1.20-1.22 - MPTX):
// клавиша 5 с младшим 1 и старшим 1 - сэмпл 257 (индекс 256), клавиша 6 со
// старшим 2 - за песней, пробел; без метки старшие байты не читаются.
void test_keymap_high_bytes() {
    std::printf("test_it_keymap_high_bytes\n");
    uint8_t hi[120] = {};
    hi[5] = 1;
    hi[6] = 2;
    struct Case { const char* magic; bool high; };
    const Case cases[] = {{"XTPM", true}, {"MPTX", true}, {nullptr, false}};
    for (const Case& c : cases) {
        std::vector<uint8_t> file = build_synthetic_it(false, 299, c.magic, hi);
        formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        const char* error = nullptr;
        const bool ok = formats::it::load(src.as_byte_source(), mem, song, &error);
        if (!ok) std::printf("  load: %s\n", error ? error : "?");
        CHECK(ok);
        CHECK_EQ(song.sample_count, static_cast<uint16_t>(300));
        uint16_t idx = 0xFFFF;
        uint8_t note = 0;
        bool unmapped = false;
        CHECK(soundsinth::model::resolve_sample_index(song, 1, 5, &idx, &note));
        CHECK_EQ(idx, static_cast<uint16_t>(c.high ? 256 : 0));
        CHECK_EQ(soundsinth::model::resolve_sample_index(song, 1, 6, &idx, &note, &unmapped), !c.high);
        CHECK_EQ(unmapped, c.high);
        CHECK(soundsinth::model::resolve_sample_index(song, 1, 7, &idx, &note));
        CHECK_EQ(idx, static_cast<uint16_t>(0));
        memory::track_memory_destroy(mem);
    }
}

// Заголовок IT: ChnPan/ChnVol на пяти каналах (0xFF - запись не читается,
// громкость больше 64 режется), флаги 0x08/0x10/0x20/0x1000, пороги cwtv
// для flow_mode, speed 0 и tempo меньше 32 - умолчания.
void test_header_table() {
    std::printf("test_it_header_table\n");
    auto patch = [](std::vector<uint8_t>& f) {
        const uint8_t row2[3] = {0x82, 0x04, 200}; // строка 2: канал 2 -> канал 5
        auto it = std::search(f.begin(), f.end(), row2, row2 + 3);
        CHECK(it != f.end());
        if (it != f.end()) *it = 0x85;
    };
    {
        std::vector<uint8_t> f = build_synthetic_it();
        patch(f);
        const uint8_t pan[5] = {0x80 | 16, 0xFF, 70, 100, 20};
        const uint8_t vol[5] = {20, 5, 100, 0, 64};
        for (int ch = 0; ch < 5; ++ch) {
            f[0x40 + ch] = pan[ch];
            f[0x80 + ch] = vol[ch];
        }
        f[0x32] = 0;  // speed
        f[0x33] = 20; // tempo
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        CHECK(formats::it::load(src.as_byte_source(), mem, song, nullptr));
        CHECK_EQ(song.channel_count, static_cast<uint8_t>(5));
        const uint8_t want_pan[5] = {16, 32, 32, 32, 20};
        const uint8_t want_vol[5] = {20, 64, 64, 0, 64};
        for (int ch = 0; ch < 5; ++ch) {
            CHECK_EQ(song.channel_pan[ch], want_pan[ch]);
            CHECK_EQ(song.channel_volume[ch], want_vol[ch]);
        }
        CHECK(song.channel_muted == 1);
        CHECK(song.channel_surround == (uint64_t(1) << 3));
        CHECK_EQ(song.default_speed, static_cast<uint16_t>(6));
        CHECK_EQ(song.default_tempo, static_cast<uint16_t>(125));
        memory::track_memory_destroy(mem);
    }
    {
        std::vector<uint8_t> f = build_synthetic_it();
        f[0x2C] = 0x04 | 0x08 | 0x10 | 0x20;
        f[0x2D] = 0x10; // 0x1000
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        CHECK(formats::it::load(src.as_byte_source(), mem, song, nullptr));
        CHECK(song.frequency_model == soundsinth::model::FrequencyModel::Linear);
        CHECK((song.quirks & soundsinth::model::kQuirkItLinearC5Reference) != 0);
        CHECK_EQ(song.samples[0].relative_note, static_cast<int8_t>(0));
        CHECK((song.quirks & soundsinth::model::kQuirkItOldEffects) != 0);
        CHECK((song.quirks & soundsinth::model::kQuirkItCompatGxx) != 0);
        CHECK((song.quirks & soundsinth::model::kQuirkGxxSharesPortaMemory) == 0);
        CHECK((song.quirks & soundsinth::model::kQuirkItVolColumnPortaTable) != 0);
        CHECK_EQ(song.filter_units_per_octave, static_cast<uint8_t>(20));
        memory::track_memory_destroy(mem);
    }
    struct Flow { uint16_t cwtv; uint32_t want; };
    const Flow flows[] = {
        {0x0103, soundsinth::model::kFlowLoopGlobalTarget},
        {0x0200, soundsinth::model::kFlowLoopDelaysSameRowBreak},
        {0x0210, soundsinth::model::kFlowLoopDelaysSameRowBreak | soundsinth::model::kFlowLoopEndAdvancesRow},
    };
    for (const Flow& c : flows) {
        std::vector<uint8_t> f = build_synthetic_it();
        f[0x28] = static_cast<uint8_t>(c.cwtv & 0xFF);
        f[0x29] = static_cast<uint8_t>(c.cwtv >> 8);
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        CHECK(formats::it::load(src.as_byte_source(), mem, song, nullptr));
        CHECK_EQ(static_cast<uint32_t>(song.flow_mode), c.want);
        memory::track_memory_destroy(mem);
    }
}

namespace {

// Колонка громкости IT - таблица Load_it.cpp OpenMPT, независимо от загрузчика.
soundsinth::model::VolumeColumnCommand it_volume_ref(uint8_t v) {
    using T = VolumeColumnType;
    auto cmd = [](T t, int p) { return soundsinth::model::VolumeColumnCommand{t, static_cast<uint8_t>(p)}; };
    if (v <= 64) return cmd(T::SetVolume, v);
    if (v >= 128 && v <= 192) return cmd(T::SetPanning, v - 128);
    if (v < 75) return cmd(T::FineSlideUp, v - 65);
    if (v < 85) return cmd(T::FineSlideDown, v - 75);
    if (v < 95) return cmd(T::SlideUp, v - 85);
    if (v < 105) return cmd(T::SlideDown, v - 95);
    if (v < 115) return cmd(T::PortamentoDown, v - 105);
    if (v < 125) return cmd(T::PortamentoUp, v - 115);
    if (v >= 193 && v <= 202) return cmd(T::TonePorta, v - 193);
    if (v >= 203 && v <= 212) return cmd(T::VibratoDepth, v - 203);
    if (v >= 223 && v <= 232) return cmd(T::Offset, v - 223);
    return cmd(T::None, 0);
}

// Буквы IT -> Effect, как S3MConvert OpenMPT для IT: Cxx не в BCD, Vxx как
// есть, 28 ('\') - SmoothMidiMacro; 27 и 29-31 (XParam, DelayCut, Finetune)
// не разбираются. S0x и S7x у OpenMPT - память параметра и NNA, у нас None
// (расхождение, пачка поведения). Колонка громкости - все 256 байт.
void test_effect_letters() {
    std::printf("test_it_effect_letters\n");
    struct Row { uint8_t cmd, param; Effect type; uint8_t want; };
    const Row table[] = {
        {1, 6, Effect::SetSpeed, 6},          {2, 3, Effect::PositionJump, 3},
        {3, 0x15, Effect::PatternBreak, 0x15}, {4, 0xF1, Effect::VolumeSlide, 0xF1},
        {5, 0x12, Effect::PortaDown, 0x12},   {6, 0xE2, Effect::PortaUp, 0xE2},
        {7, 0x20, Effect::TonePorta, 0x20},   {8, 0x44, Effect::Vibrato, 0x44},
        {9, 0x21, Effect::Tremor, 0x21},      {10, 0x37, Effect::Arpeggio, 0x37},
        {11, 0x02, Effect::VibratoVolSlide, 0x02}, {12, 0x20, Effect::TonePortaVolSlide, 0x20},
        {13, 0x30, Effect::SetChannelVolume, 0x30}, {14, 0xF1, Effect::ChannelVolumeSlide, 0xF1},
        {15, 0x10, Effect::SampleOffset, 0x10}, {16, 0x02, Effect::PanningSlide, 0x02},
        {17, 0x13, Effect::Retrigger, 0x13},  {18, 0x44, Effect::Tremolo, 0x44},
        {20, 0x96, Effect::SetTempo, 0x96},   {20, 0x05, Effect::SetTempo, 0x05},
        {21, 0x44, Effect::FineVibrato, 0x44}, {22, 0x40, Effect::SetGlobalVolume, 0x40},
        {22, 0x90, Effect::SetGlobalVolume, 0x90}, {23, 0x21, Effect::GlobalVolumeSlide, 0x21},
        {24, 0x80, Effect::SetPanning, 0x80}, {25, 0x44, Effect::Panbrello, 0x44},
        {26, 0x50, Effect::SetMidiMacro, 0x50}, {28, 0x51, Effect::SmoothMidiMacro, 0x51},
        {19, 0x11, Effect::GlissandoControl, 1}, {19, 0x22, Effect::SetFinetune, 2},
        {19, 0x31, Effect::SetVibratoWaveform, 1}, {19, 0x42, Effect::SetTremoloWaveform, 2},
        {19, 0x53, Effect::SetPanbrelloWaveform, 3}, {19, 0x62, Effect::FinePatternDelay, 2},
        {19, 0x8C, Effect::SetPanning4Bit, 0xC}, {19, 0x91, Effect::SoundControl, 1},
        {19, 0xA2, Effect::HighOffset, 2},    {19, 0xB3, Effect::PatternLoop, 3},
        {19, 0xC4, Effect::NoteCut, 4},       {19, 0xD5, Effect::NoteDelay, 5},
        {19, 0xE6, Effect::PatternDelay, 6},  {19, 0xF7, Effect::SetActiveMidiMacro, 7},
        {19, 0x05, Effect::None, 0},          {19, 0x75, Effect::None, 0},
        {0, 0x10, Effect::None, 0},           {27, 0x10, Effect::None, 0},
        {29, 0x12, Effect::None, 0},          {30, 0x40, Effect::None, 0},
        {31, 0x40, Effect::None, 0},
    };
    constexpr size_t kRows = sizeof(table) / sizeof(table[0]);
    // Канал 0 на каждой из 256 строк: байт громкости - номер строки, эффект - строка таблицы.
    std::vector<uint8_t> body;
    for (size_t row = 0; row < 256; ++row) {
        const bool fx = row < kRows;
        body.push_back(0x81);
        body.push_back(fx ? 0x0C : 0x04);
        body.push_back(static_cast<uint8_t>(row));
        if (fx) {
            body.push_back(table[row].cmd);
            body.push_back(table[row].param);
        }
        body.push_back(0x00);
    }
    std::vector<uint8_t> file = build_synthetic_it(false, 0, nullptr, nullptr, &body, 256);
    formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    const bool ok = formats::it::load(src.as_byte_source(), mem, song, nullptr);
    CHECK(ok);
    if (ok) {
        CHECK_EQ(song.channel_count, static_cast<uint8_t>(1));
        const soundsinth::model::Pattern& pat = song.patterns[0];
        CHECK_EQ(pat.row_count, static_cast<uint16_t>(256));
        patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, pat.psram_offset), pat.row_count,
                                       pat.channel_count);
        uint32_t bad = 0;
        for (size_t row = 0; row < 256; ++row) {
            soundsinth::model::PatternCell cells[1];
            reader.read_row(static_cast<uint16_t>(row), cells);
            const soundsinth::model::VolumeColumnCommand want_vol = it_volume_ref(static_cast<uint8_t>(row));
            if (cells[0].volume.type != want_vol.type || cells[0].volume.param != want_vol.param) {
                if (++bad <= 5) {
                    std::printf("  громкость %u: тип %d param %u, ждали %d %u\n", static_cast<unsigned>(row),
                                static_cast<int>(cells[0].volume.type), cells[0].volume.param,
                                static_cast<int>(want_vol.type), want_vol.param);
                }
            }
            if (row >= kRows) continue;
            const soundsinth::model::EffectCommand& e = cells[0].effect;
            const bool none = table[row].type == Effect::None;
            if (e.type != table[row].type || (!none && e.param != table[row].want) ||
                e.rate != soundsinth::model::SlideRate::PerTick) {
                if (++bad <= 5) {
                    std::printf("  буква %u param %02X: тип %d param %02X, ждали %d %02X\n", table[row].cmd,
                                table[row].param, static_cast<int>(e.type), e.param, static_cast<int>(table[row].type),
                                table[row].want);
                }
            }
        }
        CHECK_EQ(bad, 0u);
    }
    memory::track_memory_destroy(mem);
}

// Второй сэмпл в синтетике: копия заголовка первого с громкостью vol и
// четырьмя байтами своих данных в конце файла.
void add_second_sample(std::vector<uint8_t>& f, uint8_t vol) {
    const char kImps[4] = {'I', 'M', 'P', 'S'};
    auto it = std::search(f.begin(), f.end(), kImps, kImps + 4);
    CHECK(it != f.end());
    if (it == f.end()) return;
    std::vector<uint8_t> hdr(it, it + 80);
    hdr[19] = vol;
    const uint32_t hdr_offset = static_cast<uint32_t>(f.size());
    const uint32_t data_offset = hdr_offset + 80;
    for (int i = 0; i < 4; ++i) hdr[72 + i] = static_cast<uint8_t>(data_offset >> (8 * i));
    f.insert(f.end(), hdr.begin(), hdr.end());
    const uint8_t pcm[4] = {1, 2, 3, 4};
    f.insert(f.end(), pcm, pcm + 4);
    const size_t second_ptr = 192 + 1 + 4 + 4; // за order, указателем инструмента и первого сэмпла
    for (int i = 0; i < 4; ++i) f[second_ptr + i] = static_cast<uint8_t>(hdr_offset >> (8 * i));
}

// Режим сэмплов (флаг instrumentMode снят): инструмент на сэмпл, keymap и
// огибающих нет, NNA - Cut; громкость по умолчанию - у каждого сэмпла своя.
void test_sample_mode() {
    std::printf("test_it_sample_mode\n");
    std::vector<uint8_t> f = build_synthetic_it(false, 1);
    f[0x2C] = 0;
    add_second_sample(f, 20);
    formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    const bool ok = formats::it::load(src.as_byte_source(), mem, song, nullptr);
    CHECK(ok);
    if (ok) {
        CHECK_EQ(song.instrument_count, static_cast<uint16_t>(2));
        const uint8_t want_volume[2] = {48, 20};
        for (uint16_t i = 0; i < 2 && i < song.instrument_count; ++i) {
            const soundsinth::model::Instrument& ins = song.instruments[i];
            CHECK_EQ(ins.default_sample_index, i);
            CHECK_EQ(song.samples[i].default_volume, want_volume[i]);
            CHECK(ins.note_to_sample_ranges == nullptr);
            CHECK(ins.nna == soundsinth::model::NewNoteAction::Cut);
            CHECK(ins.volume_envelope == nullptr && ins.panning_envelope == nullptr && ins.pitch_envelope == nullptr);
        }
        uint16_t idx = 0xFFFF;
        uint8_t note = 0;
        CHECK(soundsinth::model::resolve_sample_index(song, 2, 60, &idx, &note));
        CHECK_EQ(idx, static_cast<uint16_t>(1));
        CHECK_EQ(note, static_cast<uint8_t>(60));
    }
    memory::track_memory_destroy(mem);
}

// Старый формат инструмента (cmwt < 2.00): NNA и DNC на своих местах,
// fadeout x128, одна огибающая узлами (тик, значение) до тика 0xFF, флаги
// 0x01 включена, 0x02 петля, 0x04 удержание, carry нет.
void test_old_instrument() {
    std::printf("test_it_old_instrument\n");
    std::vector<uint8_t> f = build_synthetic_it();
    f[0x2A] = 0x00; // cmwt 0x0100
    f[0x2B] = 0x01;
    const char kImpi[4] = {'I', 'M', 'P', 'I'};
    auto it = std::search(f.begin(), f.end(), kImpi, kImpi + 4);
    CHECK(it != f.end());
    if (it == f.end()) return;
    const size_t h = static_cast<size_t>(it - f.begin());
    f[h + 17] = 0x01 | 0x04; // флаги огибающей
    f[h + 18] = 0;           // петля
    f[h + 19] = 0;
    f[h + 20] = 1;           // удержание
    f[h + 21] = 1;
    f[h + 24] = 100;         // fadeout
    f[h + 25] = 0;
    f[h + 26] = 2;           // NNA Off
    f[h + 27] = 1;           // DNC Note
    const uint8_t nodes[8] = {0, 64, 10, 32, 20, 0, 0xFF, 0};
    for (int i = 0; i < 8; ++i) f[h + 504 + i] = nodes[i];
    formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    const bool ok = formats::it::load(src.as_byte_source(), mem, song, nullptr);
    CHECK(ok);
    if (ok) {
        const soundsinth::model::Instrument& ins = song.instruments[0];
        CHECK(ins.nna == soundsinth::model::NewNoteAction::Off);
        CHECK(ins.dct == soundsinth::model::DuplicateCheckType::Note);
        CHECK_EQ(ins.fadeout_rate, 12800u);
        const soundsinth::model::Envelope* e = ins.volume_envelope;
        CHECK(e != nullptr);
        if (e) {
            CHECK_EQ(e->point_count, static_cast<uint8_t>(3));
            CHECK(e->sustain_enabled);
            CHECK(!e->loop_enabled);
            CHECK(!e->carry);
            CHECK_EQ(e->points[1].tick, static_cast<uint16_t>(10));
            CHECK_EQ(e->points[1].value, static_cast<int16_t>(32));
        }
        CHECK(ins.panning_envelope == nullptr && ins.pitch_envelope == nullptr);
    }
    memory::track_memory_destroy(mem);
}

// Включённая огибающая без точек: у громкости остаётся (у OpenMPT она
// включена и не обрабатывается), у панорамы и питча - nullptr.
void test_empty_envelopes() {
    std::printf("test_it_empty_envelopes\n");
    std::vector<uint8_t> f = build_synthetic_it(true);
    const char kImpi[4] = {'I', 'M', 'P', 'I'};
    auto it = std::search(f.begin(), f.end(), kImpi, kImpi + 4);
    CHECK(it != f.end());
    if (it == f.end()) return;
    const size_t h = static_cast<size_t>(it - f.begin());
    for (size_t env : {304u, 386u, 468u}) {
        f[h + env] = 0x01;
        f[h + env + 1] = 0;
    }
    formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    const bool ok = formats::it::load(src.as_byte_source(), mem, song, nullptr);
    CHECK(ok);
    if (ok) {
        const soundsinth::model::Instrument& ins = song.instruments[0];
        CHECK(ins.volume_envelope != nullptr);
        if (ins.volume_envelope) {
            CHECK(ins.volume_envelope->enabled);
            CHECK_EQ(ins.volume_envelope->point_count, static_cast<uint8_t>(0));
        }
        CHECK(ins.panning_envelope == nullptr);
        CHECK(ins.pitch_envelope == nullptr);
        CHECK(ins.filter_envelope == nullptr);
    }
    memory::track_memory_destroy(mem);
}

// Потери разбора считаются: петля удержания и автовибрато сэмпла (не
// воспроизводятся), ячейка S0x (не разбирается), сэмпл, данные которого за
// концом файла (выброшен).
void test_load_stats() {
    std::printf("test_it_load_stats\n");
    std::vector<uint8_t> body = {0x81, 0x08, 19, 0x05, 0x00}; // канал 1: S05
    std::vector<uint8_t> f = build_synthetic_it(false, 0, nullptr, nullptr, &body, 1);
    const char kImps[4] = {'I', 'M', 'P', 'S'};
    auto it = std::search(f.begin(), f.end(), kImps, kImps + 4);
    CHECK(it != f.end());
    if (it == f.end()) return;
    const size_t h = static_cast<size_t>(it - f.begin());
    f[h + 18] |= 0x20; // петля удержания
    f[h + 77] = 5;     // глубина автовибрато
    for (const bool cut : {false, true}) {
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()) - (cut ? 2u : 0u));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        CHECK(formats::it::load(src.as_byte_source(), mem, song, nullptr));
        const soundsinth::model::TrackerLoadStats& ls = soundsinth::model::g_tracker_load_stats;
        CHECK_EQ(ls.effect_cells_dropped, 1u);
        CHECK_EQ(ls.samples_dropped, static_cast<uint16_t>(cut ? 1 : 0));
        CHECK_EQ(ls.ignored_sustain_loops, static_cast<uint16_t>(cut ? 0 : 1));
        CHECK_EQ(ls.ignored_autovibrato, static_cast<uint16_t>(cut ? 0 : 1));
        CHECK_EQ(ls.samples_failed, static_cast<uint16_t>(0));
        memory::track_memory_destroy(mem);
    }
}

} // namespace

void run_it_loader_tests() {
    test_load_stats();
    test_effect_letters();
    test_sample_mode();
    test_old_instrument();
    test_empty_envelopes();
    test_header_table();
    test_keymap_ranges();
    test_keymap_high_bytes();
    test_pcm_variants();
    test_envelope_sanitized();
    test_synthetic_exact();
    test_uncompressed_delta_and_adpcm();
    test_real_small_file_smoke();
    test_pan_pitch_envelope_offset_and_filter_flag();
    test_chnpan_mute_bit();
    test_modplug_signature();
}
