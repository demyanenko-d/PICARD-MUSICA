#include "testing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/model/song.h"
#include "core/formats/s3m.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_reader.h"

using namespace soundsinth;
using soundsinth::model::Effect;
using soundsinth::model::Song;
using soundsinth::model::VolumeColumnType;

namespace {

// Собирает минимальный синтетический S3M вручную, парапоинтеры и
// dataPointer дописываются постфактум (backpatch): по формату они
// ссылаются на данные, чьё смещение известно только после укладки этих
// данных (парапоинтеры в единицах по 16 байт). Смещения полей заголовка
// сверены с OpenMPT soundlib/S3MTools.h и libxmp src/loaders/s3m_load.c
// (см. formats/s3m.cpp), а не со старым JS-разборщиком, у которого они на
// 2 байта не совпадали с форматом.
// rows - упакованное тело паттерна вместо стандартного (nullptr - стандартное).
std::vector<uint8_t> build_synthetic_s3m(const std::vector<uint8_t>* rows = nullptr) {
    std::vector<uint8_t> f;
    auto put8 = [&](uint8_t v) { f.push_back(v); };
    auto put16 = [&](uint16_t v) { f.push_back(static_cast<uint8_t>(v & 0xFF)); f.push_back(static_cast<uint8_t>(v >> 8)); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); };
    auto align16 = [&]() { while (f.size() % 16 != 0) f.push_back(0); };

    // --- Заголовок, 96 байт, поле за полем ---
    const char* name = "S3MTEST";
    for (int i = 0; i < 28; ++i) put8(i < 7 ? static_cast<uint8_t>(name[i]) : 0);
    put8(0x1A);          // dosEof
    put8(0x10);          // fileType
    put16(0);            // reserved1 (2 байта, как одно u16 - байты те же)
    put16(1);            // ordNum
    put16(1);            // smpNum
    put16(1);            // patNum
    put16(0);            // flags
    put16(0x1320);       // cwtv (ST3.20)
    put16(2);            // ffi = 2 (unsigned-сэмплы - проверяем конвертацию)
    f.push_back('S'); f.push_back('C'); f.push_back('R'); f.push_back('M');
    put8(64);            // globalVol
    put8(6);             // speed
    put8(125);           // tempo
    put8(0x30);          // masterVolume
    put8(0);             // ultraClicks
    put8(0);             // usePanningTable (не 0xFC - без расширенной панорамы)
    put16(0);            // reserved2
    put32(0);            // reserved3
    put16(0);            // reserved4
    put16(0);            // special
    put8(0); put8(1);    // channels[0..1] - 2 активных канала
    for (int i = 2; i < 32; ++i) put8(0xFF);
    CHECK_EQ(f.size(), static_cast<size_t>(0x60));

    put8(0); // order[0] = паттерн 0

    const size_t sample_pp_pos = f.size();
    put16(0); // сэмпл-парапоинтер - заполним ниже (backpatch)
    const size_t pattern_pp_pos = f.size();
    put16(0); // паттерн-парапоинтер - заполним ниже (backpatch)

    align16();
    const size_t sample_header_offset = f.size();
    const size_t data_pointer_pos = sample_header_offset + 13;
    put8(1);                                   // sampleType = PCM
    for (int i = 0; i < 12; ++i) put8(0);      // filename
    put8(0); put8(0); put8(0);                 // dataPointer[3] - backpatch ниже
    put32(6);                                   // length = 6 сэмплов
    put32(0);                                   // loopStart
    put32(0);                                   // loopEnd
    put8(64);                                   // defaultVolume
    put8(0);                                     // reserved1
    put8(0);                                     // pack = 0 (unpacked)
    put8(0);                                     // flags = 0 (mono, 8-bit, без петли)
    put32(8363);                                 // c5speed
    for (int i = 0; i < 4; ++i) put8(0);         // reserved2
    put16(0); put16(0); put32(0);                // gusAddress/sb512/lastUsedPos
    for (int i = 0; i < 28; ++i) put8(0);        // name
    f.push_back('S'); f.push_back('C'); f.push_back('R'); f.push_back('S'); // magic (не проверяется загрузчиком, для реализма)
    CHECK_EQ(f.size(), sample_header_offset + 80);

    align16();
    const size_t pattern_offset = f.size();
    put16(0); // packed length - загрузчик ему не доверяет (сканирует по границам файла), значение не важно
    if (rows) {
        f.insert(f.end(), rows->begin(), rows->end());
    } else {
    // Строка 0, канал 0: нота (октава4/note0 -> канонический 48) +
    // инструмент 1 + PatternBreak (BCD 0x15 -> строка 15)
    put8(0x20 | 0x80);      // presence: note+instr, effect
    put8(0x40);             // note_raw: октава4, note0
    put8(1);                // instrument
    put8(3);                // command = 'C' = PatternBreak
    put8(0x15);              // BCD-параметр -> строка 15
    put8(0x00);              // конец строки 0

    // Строка 1, канал 1: только volume=32
    put8(0x41);
    put8(32);
    put8(0x00);

    // Строки 2..63 - пустые.
    for (int row = 2; row < 64; ++row) put8(0x00);
    }

    align16();
    const size_t sample_data_offset = f.size();
    const uint8_t unsigned_pcm[6] = {138, 118, 148, 108, 158, 88}; // signed: 10,-10,20,-20,30,-40 (ffi=2, XOR/-128)
    for (uint8_t b : unsigned_pcm) put8(b);

    // --- Backpatch ---
    const uint16_t sample_pp = static_cast<uint16_t>(sample_header_offset / 16);
    const uint16_t pattern_pp = static_cast<uint16_t>(pattern_offset / 16);
    f[sample_pp_pos] = static_cast<uint8_t>(sample_pp & 0xFF);
    f[sample_pp_pos + 1] = static_cast<uint8_t>(sample_pp >> 8);
    f[pattern_pp_pos] = static_cast<uint8_t>(pattern_pp & 0xFF);
    f[pattern_pp_pos + 1] = static_cast<uint8_t>(pattern_pp >> 8);

    const uint32_t data_paragraph = static_cast<uint32_t>(sample_data_offset / 16);
    f[data_pointer_pos] = static_cast<uint8_t>((data_paragraph >> 16) & 0xFF);      // dataPointer[0] = P_high
    f[data_pointer_pos + 1] = static_cast<uint8_t>(data_paragraph & 0xFF);          // dataPointer[1] = P_low
    f[data_pointer_pos + 2] = static_cast<uint8_t>((data_paragraph >> 8) & 0xFF);   // dataPointer[2] = P_mid

    return f;
}

void test_synthetic_exact() {
    std::printf("test_s3m_loader_synthetic_exact\n");

    const std::vector<uint8_t> file = build_synthetic_s3m();
    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));

    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok = formats::s3m::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);
    if (!ok) { memory::track_memory_destroy(mem); return; }

    CHECK_EQ(song.channel_count, static_cast<uint8_t>(2));
    CHECK_EQ(song.sample_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.pattern_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.order_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.order[0], static_cast<uint16_t>(0));
    CHECK_EQ(song.default_speed, static_cast<uint16_t>(6));
    CHECK_EQ(song.default_tempo, static_cast<uint16_t>(125));
    CHECK_EQ(song.default_global_volume, static_cast<uint8_t>(128)); // globalVol=64 x2 -> 128
    CHECK_EQ(song.sample_preamp, static_cast<uint8_t>(35)); // masterVolume=0x30: 48, моно - 48 * 8 / 11 = 35

    const auto& sd = song.samples[0];
    CHECK_EQ(sd.length_samples, 6u);
    CHECK(!sd.loop_enabled);
    CHECK_EQ(sd.c5_speed, 8363u);
    CHECK_EQ(sd.default_volume, static_cast<uint8_t>(64));

    patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, song.patterns[0].psram_offset),
                                    song.patterns[0].row_count, song.patterns[0].channel_count);
    soundsinth::model::PatternCell cells[2];

    reader.read_row(0, cells);
    CHECK_EQ(cells[0].note, static_cast<uint8_t>(48)); // октава4/note0 -> 4*12+0
    CHECK_EQ(cells[0].instrument, static_cast<uint8_t>(1));
    CHECK(cells[0].effect.type == Effect::PatternBreak);
    CHECK_EQ(cells[0].effect.param, static_cast<uint8_t>(15)); // BCD 0x15 -> 15, не 0x15=21
    CHECK_EQ(cells[1].note, soundsinth::model::kNoteNone);

    reader.read_row(1, cells);
    CHECK(cells[1].volume.type == VolumeColumnType::SetVolume);
    CHECK_EQ(cells[1].volume.param, static_cast<uint8_t>(32));
    CHECK_EQ(cells[0].note, soundsinth::model::kNoteNone);

    reader.read_row(10, cells);
    for (int ch = 0; ch < 2; ++ch) CHECK_EQ(cells[ch].note, soundsinth::model::kNoteNone);

    // Сэмпл резидентен (Raw8-repack прошёл, применена конвертация ffi=2
    // unsigned -> signed).
    auto* cache_entry = memory::sample_cache_find(mem.sample_cache, 0);
    CHECK(cache_entry != nullptr);
    if (cache_entry) {
        // Сами байты: без перевода беззнаковых в знаковые здесь было бы 138, 118...
        const int8_t want[6] = {10, -10, 20, -20, 30, -40};
        const auto* got = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, cache_entry->first_page));
        for (int i = 0; i < 6; ++i) CHECK_EQ(got[i], want[i]);
    }

    memory::track_memory_destroy(mem);
}

void test_real_small_file_smoke() {
    std::printf("test_s3m_loader_real_small_file_smoke\n");

    std::ifstream in("SD/test_music/s3m/41096877.s3m", std::ios::binary);
    if (!in) {
        std::printf("  файл не найден — ПРОПУСК (запуск не из корня репозитория?)\n");
        return;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(file.size() > 0x60);

    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok = formats::s3m::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);
    if (ok) {
        CHECK(song.channel_count >= 1 && song.channel_count <= 32);
        CHECK(song.pattern_count >= 1);
        CHECK(song.order_count >= 1);
        for (uint16_t p = 0; p < song.pattern_count; ++p) {
            CHECK(song.patterns[p].psram_offset != soundsinth::model::Pattern::kInvalidOffset);
        }
    }

    memory::track_memory_destroy(mem);
}

// Выключенный канал (бит 0x80 байта канала). ST3 не обрабатывает его
// вовсе, даже PatternBreak - ячейки не сохраняются. Файл другого трекера
// (cwtv 0x3xxx - Impulse Tracker) - как IT: ячейки на месте, канал в
// Song::channel_muted.
void test_muted_channel() {
    std::printf("test_s3m_loader_muted_channel\n");

    for (const bool st3 : {true, false}) {
        std::vector<uint8_t> file = build_synthetic_s3m();
        file[0x40] = 0x80;        // канал 0 выключен
        if (!st3) file[0x29] = 0x32; // cwtv 0x3220
        formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);

        Song song;
        const char* error = nullptr;
        const bool ok = formats::s3m::load(mbs.as_byte_source(), mem, song, &error);
        CHECK(ok);
        if (ok) {
            patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, song.patterns[0].psram_offset),
                                            song.patterns[0].row_count, song.patterns[0].channel_count);
            soundsinth::model::PatternCell cells[2];
            reader.read_row(0, cells);
            if (st3) {
                CHECK(song.channel_muted == 0);
                CHECK_EQ(cells[0].note, soundsinth::model::kNoteNone);
                CHECK(cells[0].effect.type == Effect::None);
            } else {
                CHECK(song.channel_muted == 1);
                CHECK_EQ(cells[0].note, static_cast<uint8_t>(48));
                CHECK(cells[0].effect.type == Effect::PatternBreak);
            }
            reader.read_row(1, cells);
            CHECK(cells[1].volume.type == VolumeColumnType::SetVolume); // канал 1 не задет
        }
        memory::track_memory_destroy(mem);
    }
}

// S8x - панорама 4 битами, как у IT (раньше терялась: 26.7% S3M архива).
void test_s8x_panning() {
    std::printf("test_s3m_loader_s8x_panning\n");

    std::vector<uint8_t> file = build_synthetic_s3m();
    // Ячейка строки 0 канала 0: нота 0x40, инструмент 1, эффект C15 - на S8C.
    const uint8_t pattern_cell[] = {0x40, 1, 3, 0x15};
    auto it = std::search(file.begin(), file.end(), std::begin(pattern_cell), std::end(pattern_cell));
    CHECK(it != file.end());
    if (it == file.end()) return;
    it[2] = 19;   // 'S'
    it[3] = 0x8C;
    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    const char* error = nullptr;
    CHECK(formats::s3m::load(mbs.as_byte_source(), mem, song, &error));
    patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, song.patterns[0].psram_offset),
                                    song.patterns[0].row_count, song.patterns[0].channel_count);
    soundsinth::model::PatternCell cells[2];
    reader.read_row(0, cells);
    CHECK(cells[0].effect.type == Effect::SetPanning4Bit);
    CHECK_EQ(cells[0].effect.param, static_cast<uint8_t>(0xC));
    memory::track_memory_destroy(mem);
}

} // namespace

// Поля заголовка и ячейки S3M на синтетике с правкой байтов: amigaLimits
// (бит 0x10 flags), разводка L/R по стерео-биту masterVolume и биту 0x08
// типа канала, подстановки speed 0 -> 6 и tempo < 32 -> 125, нота 0xFE ->
// kNoteOff. Смещения - заголовок S3M: flags 0x26, speed 0x31, tempo 0x32,
// masterVolume 0x33, типы каналов с 0x40.
struct S3mLoaded {
    memory::TrackMemory mem;
    Song song;
    bool ok = false;
    explicit S3mLoaded(const std::vector<uint8_t>& f) {
        memory::track_memory_create(mem);
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        const char* err = nullptr;
        ok = formats::s3m::load(src.as_byte_source(), mem, song, &err);
    }
    ~S3mLoaded() { memory::track_memory_destroy(mem); }
};

void test_header_fields() {
    std::printf("test_s3m_header_fields\n");
    {
        std::vector<uint8_t> f = build_synthetic_s3m();
        f[0x26] = 0x10; // flags: amigaLimits
        S3mLoaded a(f);
        CHECK(a.ok);
        CHECK((a.song.quirks & soundsinth::model::kQuirkAmigaLimits) != 0);
        S3mLoaded b(build_synthetic_s3m());
        CHECK((b.song.quirks & soundsinth::model::kQuirkAmigaLimits) == 0);
    }
    {
        std::vector<uint8_t> f = build_synthetic_s3m();
        f[0x33] = 0xB0; // стерео
        f[0x40] = 0x00; // L1
        f[0x41] = 0x08; // R1
        S3mLoaded a(f);
        CHECK(a.ok);
        CHECK_EQ(a.song.channel_pan[0], static_cast<uint8_t>(13));
        CHECK_EQ(a.song.channel_pan[1], static_cast<uint8_t>(51));
        f[0x33] = 0x30; // моно - в центр
        S3mLoaded b(f);
        CHECK_EQ(b.song.channel_pan[0], static_cast<uint8_t>(32));
        CHECK_EQ(b.song.channel_pan[1], static_cast<uint8_t>(32));
    }
    {
        std::vector<uint8_t> f = build_synthetic_s3m();
        f[0x31] = 0;  // speed
        f[0x32] = 20; // tempo
        S3mLoaded a(f);
        CHECK_EQ(a.song.default_speed, static_cast<uint16_t>(6));
        CHECK_EQ(a.song.default_tempo, static_cast<uint16_t>(125));
        f[0x31] = 3;
        f[0x32] = 150;
        S3mLoaded b(f);
        CHECK_EQ(b.song.default_speed, static_cast<uint16_t>(3));
        CHECK_EQ(b.song.default_tempo, static_cast<uint16_t>(150));
    }
    {
        // Нота 0xFE - Note-Off: ячейка строки 0 канала 0 (note+instr, эффект C).
        std::vector<uint8_t> f = build_synthetic_s3m();
        const uint8_t cell[5] = {0xA0, 0x40, 0x01, 0x03, 0x15};
        auto it = std::search(f.begin(), f.end(), std::begin(cell), std::end(cell));
        CHECK(it != f.end());
        if (it != f.end()) *(it + 1) = 0xFE;
        S3mLoaded a(f);
        CHECK(a.ok);
        const soundsinth::model::Pattern& pat = a.song.patterns[0];
        patterns::PatternReader reader(memory::psram_pattern_ptr(a.mem.psram, pat.psram_offset), pat.row_count,
                                       pat.channel_count);
        soundsinth::model::PatternCell cells[32];
        reader.read_row(0, cells);
        CHECK_EQ(cells[0].note, soundsinth::model::kNoteOff);
    }
}

// Буквы S3M -> Effect, как S3MConvert OpenMPT: по строке на букву A..Z и на
// подкоманду S0..SF, эффект в канале 0. Vxx - шкала 0..64 умножается на 2 с
// потолком 128; Wxy - по ниблам. W с ниблом 8 и больше (у OpenMPT без
// потолка 15) здесь не закрепляется.
void test_effect_letters() {
    std::printf("test_s3m_effect_letters\n");
    struct Row { uint8_t cmd, param; Effect type; uint8_t want; };
    const Row table[] = {
        {1, 6, Effect::SetSpeed, 6},          {2, 3, Effect::PositionJump, 3},
        {3, 0x15, Effect::PatternBreak, 15},  {4, 0x0F, Effect::VolumeSlide, 0x0F},
        {5, 0x12, Effect::PortaDown, 0x12},   {6, 0x12, Effect::PortaUp, 0x12},
        {7, 0x20, Effect::TonePorta, 0x20},   {8, 0x44, Effect::Vibrato, 0x44},
        {9, 0x21, Effect::Tremor, 0x21},      {10, 0x37, Effect::Arpeggio, 0x37},
        {11, 0x02, Effect::VibratoVolSlide, 0x02}, {12, 0x20, Effect::TonePortaVolSlide, 0x20},
        {13, 0x30, Effect::SetChannelVolume, 0x30}, {14, 0xF1, Effect::ChannelVolumeSlide, 0xF1},
        {15, 0x10, Effect::SampleOffset, 0x10}, {16, 0x02, Effect::PanningSlide, 0x02},
        {17, 0x13, Effect::Retrigger, 0x13},  {18, 0x44, Effect::Tremolo, 0x44},
        {20, 0x96, Effect::SetTempo, 0x96},   {21, 0x44, Effect::FineVibrato, 0x44},
        {22, 0x20, Effect::SetGlobalVolume, 0x40}, {22, 0x40, Effect::SetGlobalVolume, 0x80},
        {22, 0x41, Effect::SetGlobalVolume, 0x80}, {23, 0x21, Effect::GlobalVolumeSlide, 0x21},
        {24, 0x80, Effect::SetPanning, 0x80}, {25, 0x44, Effect::Panbrello, 0x44},
        {26, 0x50, Effect::SetMidiMacro, 0x50},
        {19, 0x11, Effect::GlissandoControl, 1}, {19, 0x22, Effect::SetFinetune, 2},
        {19, 0x31, Effect::SetVibratoWaveform, 1}, {19, 0x42, Effect::SetTremoloWaveform, 2},
        {19, 0x53, Effect::SetPanbrelloWaveform, 3}, {19, 0x62, Effect::FinePatternDelay, 2},
        {19, 0x8C, Effect::SetPanning4Bit, 0xC}, {19, 0x91, Effect::SoundControl, 1},
        {19, 0xA2, Effect::HighOffset, 2},    {19, 0xB3, Effect::PatternLoop, 3},
        {19, 0xC4, Effect::NoteCut, 4},       {19, 0xD5, Effect::NoteDelay, 5},
        {19, 0xE6, Effect::PatternDelay, 6},  {19, 0xF7, Effect::SetActiveMidiMacro, 7},
        {19, 0x05, Effect::None, 0},          {19, 0x75, Effect::None, 0},
    };
    constexpr size_t kRows = sizeof(table) / sizeof(table[0]);
    static_assert(kRows <= 64, "по строке на случай");
    std::vector<uint8_t> body;
    for (const Row& r : table) {
        body.push_back(0x80); // канал 0, только эффект
        body.push_back(r.cmd);
        body.push_back(r.param);
        body.push_back(0x00);
    }
    for (size_t row = kRows; row < 64; ++row) body.push_back(0x00);
    const std::vector<uint8_t> f = build_synthetic_s3m(&body);
    S3mLoaded m(f);
    CHECK(m.ok);
    if (!m.ok) return;
    const soundsinth::model::Pattern& pat = m.song.patterns[0];
    patterns::PatternReader reader(memory::psram_pattern_ptr(m.mem.psram, pat.psram_offset), pat.row_count,
                                   pat.channel_count);
    uint32_t bad = 0;
    for (size_t row = 0; row < kRows; ++row) {
        soundsinth::model::PatternCell cells[32];
        reader.read_row(static_cast<uint16_t>(row), cells);
        const soundsinth::model::EffectCommand& e = cells[0].effect;
        const bool none = table[row].type == Effect::None;
        if (e.type != table[row].type || (!none && e.param != table[row].want) ||
            e.rate != soundsinth::model::SlideRate::PerTick) {
            if (++bad <= 5) {
                std::printf("  буква %u param %02X: тип %d param %02X, ждали %d %02X\n", table[row].cmd, table[row].param,
                            static_cast<int>(e.type), e.param, static_cast<int>(table[row].type), table[row].want);
            }
        }
    }
    CHECK_EQ(bad, 0u);
}

// Синтетика с другим сэмплом: flags сэмпла (0x02 стерео, 0x04 16 бит), ffi
// заголовка (1 - знаковые, 2 - беззнаковые) и сырые данные вместо шести байт
// стандартного сэмпла. Данные - последние в файле, указатель на них тот же.
std::vector<uint8_t> s3m_with_sample(uint8_t sample_flags, uint16_t ffi, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> f = build_synthetic_s3m();
    f[0x2A] = static_cast<uint8_t>(ffi & 0xFF);
    f[0x2B] = static_cast<uint8_t>(ffi >> 8);
    const size_t header = (f[0x61] | (f[0x62] << 8)) * 16u; // парапоинтер сэмпла - сразу за order
    f[header + 31] = sample_flags;
    f.resize(f.size() - 6);
    f.insert(f.end(), data.begin(), data.end());
    return f;
}

// 16-битные и стерео-сэмплы S3M (в корпусе таких нет): перевод беззнаковых
// 16 бит, знаковые как есть, от стерео - левый канал.
void test_16bit_and_stereo_samples() {
    std::printf("test_s3m_16bit_and_stereo_samples\n");
    const int16_t v[6] = {1000, -1000, 2000, -2000, 3000, -4000};
    for (uint16_t ffi : {uint16_t(2), uint16_t(1)}) {
        std::vector<uint8_t> data;
        for (int16_t x : v) {
            const uint16_t w = ffi == 2 ? static_cast<uint16_t>(x + 32768) : static_cast<uint16_t>(x);
            data.push_back(static_cast<uint8_t>(w & 0xFF));
            data.push_back(static_cast<uint8_t>(w >> 8));
        }
        S3mLoaded m(s3m_with_sample(0x04, ffi, data));
        CHECK(m.ok);
        CHECK(m.song.samples[0].resident_encoding == soundsinth::model::ResidentEncoding::Raw16);
        auto* e = memory::sample_cache_find(m.mem.sample_cache, 0);
        CHECK(e != nullptr);
        if (e) {
            const uint8_t* p = memory::psram_page_ptr(m.mem.psram, e->first_page);
            for (int i = 0; i < 6; ++i) CHECK_EQ(static_cast<int16_t>(p[2 * i] | (p[2 * i + 1] << 8)), v[i]);
        }
    }
    {
        // Стерео 8 бит, беззнаковые: левый блок, затем правый - звучит левый.
        const int8_t left[6] = {10, -10, 20, -20, 30, -40};
        std::vector<uint8_t> data;
        for (int8_t x : left) data.push_back(static_cast<uint8_t>(x + 128));
        for (int i = 0; i < 6; ++i) data.push_back(static_cast<uint8_t>(128 + 50 + i));
        S3mLoaded m(s3m_with_sample(0x02, 2, data));
        CHECK(m.ok);
        CHECK_EQ(m.song.samples[0].length_samples, 6u);
        auto* e = memory::sample_cache_find(m.mem.sample_cache, 0);
        CHECK(e != nullptr);
        if (e) {
            const auto* p = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(m.mem.psram, e->first_page));
            for (int i = 0; i < 6; ++i) CHECK_EQ(p[i], left[i]);
        }
    }
}

// Таблица панорамы каналов (usePanningTable 0xFC): панорама каналов как у
// OpenMPT (ChnSettings nPan / 4) - поверх аппаратной разводки 13/51.
void test_channel_panning_table() {
    std::printf("test_s3m_channel_panning_table\n");
    struct Case { const char* path; std::vector<uint8_t> pan; };
    const Case cases[] = {
        {"SD/test_music/s3m/starwars.s3m", {30, 30, 30, 30}},
        {"SD/test_music/s3m/2nd_reality.s3m", {26, 4, 13, 21, 43, 17, 47, 30}},
    };
    for (const Case& c : cases) {
        std::ifstream in(c.path, std::ios::binary);
        if (!in) { std::printf("  ПРОПУСК (нет файла): %s\n", c.path); continue; }
        const std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        S3mLoaded m(f);
        CHECK(m.ok);
        for (size_t i = 0; i < c.pan.size(); ++i) CHECK_EQ(m.song.channel_pan[i], c.pan[i]);
    }
}

// Настоящий ST3, как у OpenMPT: отпечаток заголовка и адреса GUS у сэмплов
// (без них - не ST3, кроме 3.00) - Kxy/Lxy без тонкого варианта.
void test_st3_combined_fine_slides_quirk() {
    std::printf("test_s3m_st3_combined_fine_slides_quirk\n");
    struct Case { uint16_t cwtv; uint16_t gus; bool want; };
    const Case cases[] = {{0x1301, 1, true}, {0x1301, 0, false}, {0x1300, 0, true}, {0x1320, 1, false}};
    for (const Case& c : cases) {
        std::vector<uint8_t> f = build_synthetic_s3m();
        f[0x28] = static_cast<uint8_t>(c.cwtv & 0xFF);
        f[0x29] = static_cast<uint8_t>(c.cwtv >> 8);
        const size_t header = (f[0x61] | (f[0x62] << 8)) * 16u;
        f[header + 40] = static_cast<uint8_t>(c.gus & 0xFF);
        f[header + 41] = static_cast<uint8_t>(c.gus >> 8);
        S3mLoaded m(f);
        CHECK(m.ok);
        CHECK_EQ((m.song.quirks & soundsinth::model::kQuirkS3mIgnoreCombinedFineSlides) != 0, c.want);
    }
}

// c5 сэмпла как у OpenMPT: у ST3 (cwtv 0x1301) не выше 65535, у файла с
// отпечатком Velvet Studio (0x1320, флаги 0, без таблицы панорамы) - как есть;
// у всех не ниже 1024, 0 - 8363. Ноты 0xF0..0xFD - не ноты, инструмент
// остаётся.
void test_c5_limits_and_reserved_notes() {
    std::printf("test_s3m_c5_limits_and_reserved_notes\n");
    struct Case { uint16_t cwtv; uint32_t c5; uint32_t want; };
    const Case cases[] = {
        {0x1301, 100000, 65535}, {0x1320, 100000, 100000}, {0x1301, 500, 1024}, {0x1320, 0, 8363},
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> f = build_synthetic_s3m();
        f[0x28] = static_cast<uint8_t>(c.cwtv & 0xFF);
        f[0x29] = static_cast<uint8_t>(c.cwtv >> 8);
        const size_t header = (f[0x61] | (f[0x62] << 8)) * 16u;
        for (int i = 0; i < 4; ++i) f[header + 32 + i] = static_cast<uint8_t>(c.c5 >> (8 * i));
        S3mLoaded m(f);
        CHECK(m.ok);
        CHECK_EQ(m.song.samples[0].c5_speed, c.want);
    }
    std::vector<uint8_t> f = build_synthetic_s3m();
    const uint8_t cell[5] = {0xA0, 0x40, 0x01, 0x03, 0x15};
    auto it = std::search(f.begin(), f.end(), std::begin(cell), std::end(cell));
    CHECK(it != f.end());
    if (it != f.end()) *(it + 1) = 0xF5;
    S3mLoaded m(f);
    CHECK(m.ok);
    const soundsinth::model::Pattern& pat = m.song.patterns[0];
    patterns::PatternReader reader(memory::psram_pattern_ptr(m.mem.psram, pat.psram_offset), pat.row_count,
                                   pat.channel_count);
    soundsinth::model::PatternCell cells[32];
    reader.read_row(0, cells);
    CHECK_EQ(cells[0].note, soundsinth::model::kNoteNone);
    CHECK_EQ(cells[0].instrument, static_cast<uint8_t>(1));
}

namespace {

// Сэмпл S3M со сжатием (pack != 0) не распаковывается: причина названа.
void test_adpcm_sample_reason() {
    std::printf("test_s3m_adpcm_sample_reason\n");
    std::vector<uint8_t> f = build_synthetic_s3m();
    const size_t header = (f[0x61] | (f[0x62] << 8)) * 16u;
    f[header + 30] = 4; // pack
    S3mLoaded m(f);
    CHECK(m.ok);
    if (!m.ok) return;
    CHECK(m.song.samples[0].encoding == soundsinth::model::SampleEncoding::S3mAdpcm4);
    CHECK(memory::sample_cache_find(m.mem.sample_cache, 0) == nullptr);
    formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
    const char* reason = nullptr;
    CHECK(!formats::s3m::load_sample_pcm(src.as_byte_source(), m.mem, m.song, 0, &reason));
    CHECK(reason != nullptr && std::strcmp(reason, "S3M ADPCM не поддержан") == 0);
}

} // namespace

void run_s3m_loader_tests() {
    test_adpcm_sample_reason();
    test_c5_limits_and_reserved_notes();
    test_st3_combined_fine_slides_quirk();
    test_channel_panning_table();
    test_16bit_and_stereo_samples();
    test_effect_letters();
    test_header_fields();
    test_s8x_panning();
    test_synthetic_exact();
    test_real_small_file_smoke();
    test_muted_channel();
}
