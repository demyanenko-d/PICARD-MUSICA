#include "testing.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/model/amiga_period.h"
#include "core/formats/load_stats.h"
#include "core/model/song.h"
#include "core/formats/mod.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_reader.h"

using namespace soundsinth;
using soundsinth::model::Effect;
using soundsinth::model::SlideRate;
using soundsinth::model::Song;

namespace {

// Собирает минимальный синтетический MOD-файл вручную, байт за байтом
// (для отладки нужен контроль над каждым байтом): сигнатура M.K.
// (4 канала), 31 заголовок сэмпла (непустой только сэмпл 1, 4 слова =
// 8 сэмплов, без петли), 1 паттерн (64 строки, фиксировано форматом),
// затем сырые PCM-данные сэмпла 1.
std::vector<uint8_t> build_synthetic_mod() {
    std::vector<uint8_t> f(1084 + 1024 + 8, 0);

    std::memcpy(f.data(), "TESTSONG", 8); // title, остальное - нули

    // Сэмпл 1 (индекс 0 при счёте с нуля): length_words=4 (BE), finetune=0,
    // volume=64, без петли.
    const uint32_t s0 = 20;
    f[s0 + 22] = 0x00; f[s0 + 23] = 0x04; // length_words = 4 (BE) -> 8 сэмплов
    f[s0 + 24] = 0x00;                     // finetune
    f[s0 + 25] = 64;                        // volume
    f[s0 + 26] = 0x00; f[s0 + 27] = 0x00;  // loop_start_words
    f[s0 + 28] = 0x00; f[s0 + 29] = 0x00;  // loop_size_words (0 -> без петли)
    // Остальные 30 сэмплов уже нули (пустые "инструменты-таблички").

    f[950] = 1; // song_length
    f[951] = 0; // restart
    f[952] = 0; // order[0] = паттерн 0 (остальные 127 байт таблицы - нули, не читаются при song_length=1)

    std::memcpy(&f[1080], "M.K.", 4);

    auto cell = [&](uint32_t row, uint32_t ch, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
        const uint32_t pos = 1084 + (row * 4 + ch) * 4;
        f[pos + 0] = b0; f[pos + 1] = b1; f[pos + 2] = b2; f[pos + 3] = b3;
    };
    // row0/ch0: период 856 (C-1, tuning 0) + инструмент 1, без эффекта.
    // b0 = (sampleHi<<4)|periodHi; period=856=0x358 -> periodHi=3;
    // sampleHi(инстр.1)=0.
    cell(0, 0, 0x03, 0x58, 0x10, 0x00);
    // row1/ch0: SetVolume (0xC), param=48.
    cell(1, 0, 0x00, 0x00, 0x0C, 48);
    // row2/ch1: PatternBreak (0xD), BCD-параметр 0x23 -> строка 23.
    cell(2, 1, 0x00, 0x00, 0x0D, 0x23);
    // row3/ch2: E1x (fine porta up), под-параметр 5.
    cell(3, 2, 0x00, 0x00, 0x0E, 0x15);
    // row4/ch3: F02 -> SetSpeed(2) (0x02 < 0x20, M.K. - не VBlank).
    cell(4, 3, 0x00, 0x00, 0x0F, 0x02);

    // Сэмпл 1 PCM: 8 байт signed 8-bit, произвольный узнаваемый паттерн.
    const uint32_t sample_data_start = 1084 + 1024;
    const int8_t pcm[8] = {10, -10, 20, -20, 30, -30, 40, -40};
    for (int i = 0; i < 8; ++i) f[sample_data_start + i] = static_cast<uint8_t>(pcm[i]);

    return f;
}

// Эвристики панорамы Load_mod.cpp: в файле ProTracker (M.K., ноты в трёх
// октавах, без петель длиной 0) 8xx/E8x до 0x2F - метки синхронизации,
// панорама игнорируется; 8xx в шкале 0..0x80 - удваивается. E8x - это
// панорама 4 битами, как S8x.
void test_panning_heuristics() {
    std::printf("test_mod_loader_panning_heuristics\n");
    using namespace soundsinth::model;
    struct Case {
        uint8_t loop_words;     // 1 - петли нет, как пишет ProTracker; 0 - файл не для Amiga
        uint8_t fx1, param1, fx2, param2;
        QuirkFlags expected;    // среди kQuirkModIgnorePanning | kQuirkMod7BitPanning | kQuirkGlissandoPtMode
    };
    const Case cases[] = {
        {1, 0x8, 0x10, 0x0, 0x00, kQuirkModIgnorePanning | kQuirkGlissandoPtMode},
        {1, 0xE, 0x82, 0x8, 0x20, kQuirkModIgnorePanning | kQuirkGlissandoPtMode}, // E82 -> 0x20, всё ещё метки
        {1, 0x8, 0x00, 0x8, 0x80, kQuirkMod7BitPanning | kQuirkGlissandoPtMode},
        {1, 0x8, 0x00, 0x8, 0xC0, kQuirkGlissandoPtMode},                          // байтовая шкала
        {0, 0x8, 0x10, 0x0, 0x00, 0},                                              // петля длиной 0: не ProTracker
    };
    constexpr QuirkFlags kMask = kQuirkModIgnorePanning | kQuirkMod7BitPanning | kQuirkGlissandoPtMode;
    for (const Case& c : cases) {
        std::vector<uint8_t> file = build_synthetic_mod();
        file[20 + 29] = c.loop_words;
        file[1084 + (5 * 4 + 0) * 4 + 2] = c.fx1;
        file[1084 + (5 * 4 + 0) * 4 + 3] = c.param1;
        file[1084 + (6 * 4 + 0) * 4 + 2] = c.fx2;
        file[1084 + (6 * 4 + 0) * 4 + 3] = c.param2;
        formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        const char* error = nullptr;
        CHECK(formats::mod::load(mbs.as_byte_source(), mem, song, &error));
        CHECK_EQ(song.quirks & kMask, c.expected);
        if (c.fx1 == 0xE) {
            patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, song.patterns[0].psram_offset),
                                            song.patterns[0].row_count, song.patterns[0].channel_count);
            PatternCell cells[4];
            reader.read_row(5, cells);
            CHECK(cells[0].effect.type == Effect::SetPanning4Bit);
            CHECK_EQ(cells[0].effect.param, static_cast<uint8_t>(2));
        }
        memory::track_memory_destroy(mem);
    }
}

// Период -> нота по семи октавам ProTracker, как ReadMODPatternEntry в
// OpenMPT: расширенные октавы (xCHN/xxCH) больше не сворачиваются в ноты 36
// и 71, на середине между соседями - более высокая нота. Ограничение
// 113..856 - только при kQuirkAmigaLimits.
void test_amiga_period_extended_octaves() {
    std::printf("test_amiga_period_extended_octaves\n");
    using soundsinth::model::amiga_period_to_note;
    CHECK_EQ(amiga_period_to_note(856), static_cast<uint8_t>(36));
    CHECK_EQ(amiga_period_to_note(113), static_cast<uint8_t>(71));
    CHECK_EQ(amiga_period_to_note(1712), static_cast<uint8_t>(24));
    CHECK_EQ(amiga_period_to_note(3424), static_cast<uint8_t>(12));
    CHECK_EQ(amiga_period_to_note(4000), static_cast<uint8_t>(12)); // длиннее таблицы - самая низкая
    CHECK_EQ(amiga_period_to_note(107), static_cast<uint8_t>(72));
    CHECK_EQ(amiga_period_to_note(57), static_cast<uint8_t>(83));  // между 60 и 56 ближе 56
    CHECK_EQ(amiga_period_to_note(28), static_cast<uint8_t>(95));
    CHECK_EQ(amiga_period_to_note(20), static_cast<uint8_t>(95));  // короче таблицы - самая высокая
    CHECK_EQ(amiga_period_to_note(832), static_cast<uint8_t>(37)); // середина 856 и 808 - более высокая нота
    CHECK_EQ(amiga_period_to_note(0), soundsinth::model::kNoteNone);
    CHECK_EQ(amiga_period_to_note(0xFFF), soundsinth::model::kNoteNone);

    using soundsinth::model::clamp_amiga_period;
    CHECK_EQ(clamp_amiga_period(57, true), static_cast<uint16_t>(113));
    CHECK_EQ(clamp_amiga_period(1712, true), static_cast<uint16_t>(856));
    CHECK_EQ(clamp_amiga_period(57, false), static_cast<uint16_t>(57));
    CHECK_EQ(clamp_amiga_period(-5, false), static_cast<uint16_t>(1));
    CHECK_EQ(clamp_amiga_period(70000, false), static_cast<uint16_t>(0xFFFF));
}

void test_synthetic_exact() {
    std::printf("test_mod_loader_synthetic_exact\n");

    const std::vector<uint8_t> file = build_synthetic_mod();
    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));

    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok = formats::mod::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);

    CHECK_EQ(song.channel_count, static_cast<uint8_t>(4));
    CHECK_EQ(song.sample_preamp, static_cast<uint8_t>(64)); // clamp(256/4, 32, 128) = 64
    CHECK_EQ(song.sample_count, static_cast<uint16_t>(31));
    CHECK_EQ(song.instrument_count, static_cast<uint16_t>(31));
    CHECK_EQ(song.order_count, static_cast<uint16_t>(1));
    CHECK_EQ(song.order[0], static_cast<uint16_t>(0));
    CHECK_EQ(song.pattern_count, static_cast<uint16_t>(1));
    CHECK(song.frequency_model == soundsinth::model::FrequencyModel::Amiga);

    // Сэмпл 0 (файловый "сэмпл 1")
    const auto& sd = song.samples[0];
    CHECK(sd.encoding == soundsinth::model::SampleEncoding::Pcm8);
    CHECK_EQ(sd.length_samples, 8u);
    CHECK(!sd.loop_enabled);
    CHECK_EQ(sd.default_volume, static_cast<uint8_t>(64));
    CHECK_EQ(sd.c5_speed, 8363u);
    // Остальные 30 сэмплов - пустые "таблички" (так решено: пусть будут
    // пустые).
    for (int i = 1; i < 31; ++i) {
        CHECK_EQ(song.samples[i].length_samples, 0u);
    }
    CHECK_EQ(song.instruments[0].default_sample_index, static_cast<uint16_t>(0));
    CHECK(song.instruments[0].note_to_sample_ranges == nullptr);

    // Паттерн: распаковать и сверить те ячейки, что были заданы.
    CHECK(song.patterns[0].psram_offset != soundsinth::model::Pattern::kInvalidOffset);
    patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, song.patterns[0].psram_offset),
                                    song.patterns[0].row_count, song.patterns[0].channel_count);

    soundsinth::model::PatternCell row_cells[4];
    reader.read_row(0, row_cells);
    CHECK_EQ(row_cells[0].note, static_cast<uint8_t>(36)); // C-1 (tuning 0, индекс 0) -> 36 + 0
    CHECK_EQ(row_cells[0].instrument, static_cast<uint8_t>(1));

    reader.read_row(1, row_cells);
    CHECK(row_cells[0].effect.type == Effect::SetVolume);
    CHECK_EQ(row_cells[0].effect.param, static_cast<uint8_t>(48));

    reader.read_row(2, row_cells);
    CHECK(row_cells[1].effect.type == Effect::PatternBreak);
    CHECK_EQ(row_cells[1].effect.param, static_cast<uint8_t>(23)); // BCD 0x23 -> 23, не 0x23=35

    reader.read_row(3, row_cells);
    CHECK(row_cells[2].effect.type == Effect::PortaUp);
    CHECK(row_cells[2].effect.rate == SlideRate::Fine);
    CHECK_EQ(row_cells[2].effect.param, static_cast<uint8_t>(5));

    reader.read_row(4, row_cells);
    CHECK(row_cells[3].effect.type == Effect::SetSpeed);
    CHECK_EQ(row_cells[3].effect.param, static_cast<uint8_t>(2));

    // Все остальные строки/каналы - пустые (проверяем выборочно строку 10).
    reader.read_row(10, row_cells);
    for (int ch = 0; ch < 4; ++ch) {
        CHECK_EQ(row_cells[ch].note, soundsinth::model::kNoteNone);
        CHECK_EQ(row_cells[ch].instrument, static_cast<uint8_t>(0));
    }

    // Сэмпл попал в кэш (перепакован в резидентные Raw8-страницы).
    auto* cache_entry = memory::sample_cache_find(mem.sample_cache, 0);
    CHECK(cache_entry != nullptr);
    if (cache_entry) {
        CHECK(cache_entry->first_page != memory::kPageChainEnd);
        const int8_t want[8] = {10, -10, 20, -20, 30, -30, 40, -40};
        const auto* got = reinterpret_cast<const int8_t*>(memory::psram_page_ptr(mem.psram, cache_entry->first_page));
        for (int i = 0; i < 8; ++i) CHECK_EQ(got[i], want[i]);
    }

    memory::track_memory_destroy(mem);
}

void test_real_small_file_smoke() {
    std::printf("test_mod_loader_real_small_file_smoke\n");

    // SD/test_music/mod/ptiswap.mod - часть тестового корпуса в репозитории
    // (scripts/build_sd.bat собирает из SD/ образ build/sd.img), а не внешней
    // библиотеки: тест воспроизводим на любой машине с репозиторием.
    std::ifstream in("SD/test_music/mod/ptiswap.mod", std::ios::binary);
    if (!in) {
        std::printf("  файл не найден (test_music/mod/ptiswap.mod) — ПРОПУСК (запуск не из корня репозитория?)\n");
        return;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(file.size() > 1084);

    formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Song song;
    const char* error = nullptr;
    const bool ok = formats::mod::load(mbs.as_byte_source(), mem, song, &error);
    if (!ok) std::printf("  load() failed: %s\n", error ? error : "(no message)");
    CHECK(ok);
    if (ok) {
        CHECK(song.channel_count >= 1 && song.channel_count <= 32);
        CHECK(song.pattern_count >= 1);
        CHECK(song.order_count >= 1);
        CHECK_EQ(song.sample_count, static_cast<uint16_t>(31));
        for (uint16_t p = 0; p < song.pattern_count; ++p) {
            CHECK(song.patterns[p].psram_offset != soundsinth::model::Pattern::kInvalidOffset);
        }
    }

    memory::track_memory_destroy(mem);
}

} // namespace

// MOD на channels каналов с сигнатурой sig: строка 0 канала 0 - нота с
// периодом period и инструментом 1, строка 1 - F7D. Порядок - order (не
// больше 128 байт), restart - байт 951.
std::vector<uint8_t> build_mod(const char* sig, uint32_t channels, uint16_t period,
                               const std::vector<uint8_t>& order = {0}, uint8_t restart = 0) {
    const uint32_t pattern_bytes = 64u * channels * 4u;
    std::vector<uint8_t> f(1084 + pattern_bytes + 8, 0);
    std::memcpy(f.data(), "TESTSONG", 8);
    f[20 + 23] = 0x04; // сэмпл 1: 4 слова
    f[20 + 25] = 64;
    f[20 + 29] = 1;    // петли нет, как пишет ProTracker (длина 0 - файл не для Amiga)
    f[950] = static_cast<uint8_t>(order.size());
    f[951] = restart;
    for (size_t i = 0; i < order.size(); ++i) f[952 + i] = order[i];
    std::memcpy(&f[1080], sig, 4);
    auto cell = [&](uint32_t row, uint32_t ch, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
        const uint32_t pos = 1084 + (row * channels + ch) * 4;
        f[pos] = b0; f[pos + 1] = b1; f[pos + 2] = b2; f[pos + 3] = b3;
    };
    cell(0, 0, static_cast<uint8_t>(period >> 8), static_cast<uint8_t>(period & 0xFF), 0x10, 0x00);
    cell(1, 0, 0x00, 0x00, 0x0F, 0x7D);
    return f;
}

// Старый 15-сэмпловый формат: заголовок 600 байт без сигнатуры, 4 канала.
std::vector<uint8_t> build_old_mod() {
    std::vector<uint8_t> f(600 + 1024 + 8, 0);
    std::memcpy(f.data(), "OLDSONG", 7);
    f[20 + 23] = 0x04;
    f[20 + 25] = 64;
    f[470] = 1; // song_length
    f[472] = 0; // order[0]
    const uint32_t pos = 600;
    f[pos] = 0x03; f[pos + 1] = 0x58; f[pos + 2] = 0x10;
    return f;
}

struct ModLoaded {
    memory::TrackMemory mem;
    Song song;
    bool ok = false;
    explicit ModLoaded(const std::vector<uint8_t>& f) {
        memory::track_memory_create(mem);
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()));
        const char* err = nullptr;
        ok = formats::mod::load(src.as_byte_source(), mem, song, &err);
    }
    ~ModLoaded() { memory::track_memory_destroy(mem); }
    soundsinth::model::PatternCell cell(uint16_t row, uint8_t ch) {
        const soundsinth::model::Pattern& pat = song.patterns[0];
        patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, pat.psram_offset), pat.row_count,
                                       pat.channel_count);
        soundsinth::model::PatternCell cells[64];
        reader.read_row(row, cells);
        return cells[ch];
    }
};

// Ветки MOD по сигнатуре и версии: число каналов, режим ProTracker, VBlank и
// тип Fxx, отказ по незнакомому тегу, старый формат, order и restart.
void test_signatures_and_header() {
    std::printf("test_mod_signatures_and_header\n");
    const auto pt = soundsinth::model::kQuirkGlissandoPtMode | soundsinth::model::kQuirkAmigaLimits;
    {
        ModLoaded m(build_mod("M.K.", 4, 856));
        CHECK(m.ok);
        CHECK_EQ(m.song.channel_count, static_cast<uint8_t>(4));
        CHECK((m.song.quirks & pt) == pt);
        CHECK((m.song.quirks & soundsinth::model::kQuirkModVBlankTiming) == 0);
        CHECK(m.cell(1, 0).effect.type == Effect::SetTempo);
        CHECK((m.song.quirks & soundsinth::model::kQuirkModTempoOnSecondTick) != 0); // темп со второго тика - режим ProTracker
    }
    {
        ModLoaded m(build_mod("M&K!", 4, 856));
        CHECK(m.ok);
        CHECK((m.song.quirks & soundsinth::model::kQuirkModVBlankTiming) != 0);
        CHECK((m.song.quirks & pt) == 0);
        CHECK(m.cell(1, 0).effect.type == Effect::SetSpeed);
    }
    {
        ModLoaded m(build_mod("6CHN", 6, 856));
        CHECK(m.ok);
        CHECK_EQ(m.song.channel_count, static_cast<uint8_t>(6));
        CHECK((m.song.quirks & pt) == 0);
        CHECK((m.song.quirks & soundsinth::model::kQuirkModTempoOnSecondTick) == 0); // вне ProTracker - темп с тика 0
    }
    {
        ModLoaded m(build_mod("28CH", 28, 856));
        CHECK(m.ok);
        CHECK_EQ(m.song.channel_count, static_cast<uint8_t>(28));
    }
    {
        ModLoaded m(build_mod("FLT4", 4, 856)); // незнакомый ASCII-тег - отказ, а не старый формат
        CHECK(!m.ok);
    }
    {
        ModLoaded m(build_mod("M.K.", 4, 1712)); // нота вне трёх октав Amiga - не режим ProTracker
        CHECK(m.ok);
        CHECK((m.song.quirks & pt) == 0);
        CHECK((m.song.quirks & soundsinth::model::kQuirkModTempoOnSecondTick) == 0);
    }
    {
        ModLoaded m(build_mod("M.K.", 4, 856, {0, 0x80, 0})); // список order обрывается на байте > 0x7F
        CHECK(m.ok);
        CHECK_EQ(m.song.order_count, static_cast<uint16_t>(1));
    }
    for (uint8_t restart : {uint8_t(0x78), uint8_t(0x7F), uint8_t(1)}) {
        ModLoaded m(build_mod("M.K.", 4, 856, {0}, restart)); // 0x78, >= 0x7F и за order_count - с начала
        CHECK(m.ok);
        CHECK_EQ(m.song.restart_position, static_cast<uint16_t>(0));
    }
    {
        ModLoaded m(build_mod("M.K.", 4, 856, {0, 0}, 1));
        CHECK_EQ(m.song.restart_position, static_cast<uint16_t>(1));
    }
    {
        ModLoaded m(build_old_mod());
        CHECK(m.ok);
        CHECK_EQ(m.song.sample_count, static_cast<uint16_t>(15));
        CHECK_EQ(m.song.channel_count, static_cast<uint8_t>(4));
        CHECK((m.song.quirks & soundsinth::model::kQuirkModVBlankTiming) != 0);
    }
}

// Уровень MOD по числу каналов (Clamp(256 / каналов, 32, 128), как OpenMPT):
// 1 канал - 128, 4 - 64, 8 и больше - 32. И круг нота -> период -> нота в
// октавах 1..7 (12..95) замыкается.
void test_preamp_and_period_round_trip() {
    std::printf("test_mod_preamp_and_period_round_trip\n");
    struct Case { const char* sig; uint32_t channels; uint8_t preamp; };
    const Case cases[] = {{"TDZ1", 1, 128}, {"M.K.", 4, 64}, {"8CHN", 8, 32}, {"16CH", 16, 32}};
    for (const Case& c : cases) {
        ModLoaded m(build_mod(c.sig, c.channels, 856));
        CHECK(m.ok);
        CHECK_EQ(m.song.sample_preamp, c.preamp);
    }
    uint32_t bad = 0;
    for (uint8_t note = 12; note <= 95; ++note) {
        if (soundsinth::model::amiga_period_to_note(soundsinth::model::amiga_note_to_period(note)) != note) ++bad;
    }
    CHECK_EQ(bad, 0u);
}

// MOD с сэмплом 1 длиной len_words и петлёй (начало, длина в словах), PCM -
// нули нужной длины.
std::vector<uint8_t> build_mod_with_loop(const char* sig, uint32_t channels, uint16_t len_words, uint16_t start_words,
                                         uint16_t size_words) {
    std::vector<uint8_t> f = build_mod(sig, channels, 856);
    f.resize(1084 + 64u * channels * 4u + len_words * 2u, 0);
    f[20 + 22] = static_cast<uint8_t>(len_words >> 8);
    f[20 + 23] = static_cast<uint8_t>(len_words);
    f[20 + 26] = static_cast<uint8_t>(start_words >> 8);
    f[20 + 27] = static_cast<uint8_t>(start_words);
    f[20 + 28] = static_cast<uint8_t>(size_words >> 8);
    f[20 + 29] = static_cast<uint8_t>(size_words);
    return f;
}

// Петли 31-сэмплового MOD в порядке OpenMPT: начало в байтах, если в словах
// петля за концом, а в байтах влезает; петля до 8 отсчётов в начале длинного
// сэмпла у 4 каналов выключена, у 6 - звучит; начало за концом - последний
// отсчёт; длина 1 слово - петли нет.
void test_sample_loops() {
    std::printf("test_mod_sample_loops\n");
    struct Case {
        const char* sig;
        uint32_t channels;
        uint16_t len_words, start_words, size_words;
        bool enabled;
        uint32_t start, end;
    };
    const Case cases[] = {
        {"M.K.", 4, 100, 10, 20, true, 20, 60},
        {"M.K.", 4, 100, 80, 30, true, 80, 140},
        {"M.K.", 4, 100, 0, 4, false, 0, 0},
        {"6CHN", 6, 100, 0, 4, true, 0, 8},
        {"M.K.", 4, 4, 0, 4, true, 0, 8},
        {"M.K.", 4, 10, 15, 4, true, 19, 20},
        {"M.K.", 4, 100, 10, 1, false, 0, 0},
    };
    for (const Case& c : cases) {
        ModLoaded m(build_mod_with_loop(c.sig, c.channels, c.len_words, c.start_words, c.size_words));
        CHECK(m.ok);
        const soundsinth::model::SampleDescriptor& sd = m.song.samples[0];
        CHECK_EQ(sd.loop_enabled, c.enabled);
        CHECK_EQ(sd.loop_start, c.start);
        CHECK_EQ(sd.loop_end, c.end);
        CHECK_EQ(sd.length_samples, c.len_words * 2u);
    }
}

// Петля за концом сэмпла у M.K. с нотами Amiga: сэмпл удлиняется до конца
// петли данными дальше по файлу, но не за конец файла; у файла с пустым
// слотом громкости 64 и у 6CHN - нет. Сэмпл в одно слово пустой.
void test_loop_past_end() {
    std::printf("test_mod_loop_past_end\n");
    struct Case {
        const char* sig;
        uint32_t channels;
        uint32_t extra;
        bool empty_slot_volume;
        uint32_t length;
    };
    const Case cases[] = {
        {"M.K.", 4, 100, false, 280}, {"M.K.", 4, 40, false, 240}, {"M.K.", 4, 100, true, 200}, {"6CHN", 6, 100, false, 200},
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> f = build_mod_with_loop(c.sig, c.channels, 100, 70, 70);
        f.resize(f.size() + c.extra, 0);
        if (c.empty_slot_volume) f[50 + 25] = 64;
        ModLoaded m(f);
        CHECK(m.ok);
        const soundsinth::model::SampleDescriptor& sd = m.song.samples[0];
        CHECK(sd.loop_enabled);
        CHECK_EQ(sd.length_samples, c.length);
        CHECK_EQ(sd.source_length_samples, c.length);
        CHECK_EQ(sd.loop_start, 140u);
        CHECK_EQ(sd.loop_end, c.length);
    }
    ModLoaded one(build_mod_with_loop("M.K.", 4, 1, 0, 1));
    CHECK(one.ok);
    CHECK_EQ(one.song.samples[0].length_samples, 0u);
}

// Старый формат: начало петли в байтах, данные до него отрезаются (длина и
// смещение в файле сдвигаются, следующий сэмпл читается с прежнего места),
// finetune 0; начало за концом сэмпла - петли нет, ничего не отрезано.
void test_old_format_loops() {
    std::printf("test_mod_old_format_loops\n");
    std::vector<uint8_t> f = build_old_mod();
    f.resize(600 + 1024 + 200 + 20, 0);
    f[20 + 22] = 0;
    f[20 + 23] = 100; // сэмпл 1: 200 байт
    f[20 + 24] = 0x05;
    f[20 + 27] = 56;  // начало петли - 56 байт
    f[20 + 29] = 20;  // петля 40 байт
    f[50 + 23] = 10;  // сэмпл 2: 20 байт
    f[50 + 25] = 64;
    f[50 + 26] = 0x01; // начало петли 256 байт - за концом
    f[50 + 29] = 4;
    ModLoaded m(f);
    CHECK(m.ok);
    const soundsinth::model::SampleDescriptor& s1 = m.song.samples[0];
    CHECK(s1.loop_enabled);
    CHECK_EQ(s1.loop_start, 0u);
    CHECK_EQ(s1.loop_end, 40u);
    CHECK_EQ(s1.length_samples, 144u);
    CHECK_EQ(s1.source_length_samples, 144u);
    CHECK_EQ(s1.file_offset, 600u + 1024u + 56u);
    CHECK_EQ(s1.finetune, static_cast<int8_t>(0));
    const soundsinth::model::SampleDescriptor& s2 = m.song.samples[1];
    CHECK(!s2.loop_enabled);
    CHECK_EQ(s2.length_samples, 20u);
    CHECK_EQ(s2.file_offset, 600u + 1024u + 200u);
}

namespace {

// Причины отказа сэмпла при загрузке по одному: PSRAM кончилась, каталог
// полон, файл обрезан внутри PCM (с числами), номер вне песни. Свободных
// страниц после отказа столько же, сколько до.
void test_sample_fail_reasons() {
    std::printf("test_mod_sample_fail_reasons\n");
    const std::vector<uint8_t> f = build_synthetic_mod();
    enum class Case { NoPsram, CatalogFull, Truncated, BadIndex };
    for (const Case c : {Case::NoPsram, Case::CatalogFull, Case::Truncated, Case::BadIndex}) {
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        const uint32_t size = static_cast<uint32_t>(f.size()) - (c == Case::Truncated ? 4u : 0u);
        formats::MemoryByteSource src(f.data(), size);
        const bool loaded = formats::mod::load(src.as_byte_source(), mem, song, nullptr, true);
        CHECK(loaded);
        if (!loaded) {
            memory::track_memory_destroy(mem);
            continue;
        }
        if (c == Case::NoPsram) {
            while (memory::psram_alloc_page(mem.psram) != memory::kPageChainEnd) {
            }
        }
        if (c == Case::CatalogFull) {
            for (uint16_t i = 0; i < memory::kSampleCacheCatalogCapacity; ++i) {
                memory::sample_cache_alloc_slot(mem.sample_cache, static_cast<uint16_t>(1000 + i), 0);
            }
        }
        const uint32_t free_before = memory::psram_free_page_count(mem.psram);
        const char* reason = nullptr;
        const uint16_t index = c == Case::BadIndex ? song.sample_count : 0;
        CHECK(!formats::mod::load_sample_pcm(src.as_byte_source(), mem, song, index, &reason));
        CHECK(reason != nullptr);
        if (reason != nullptr) {
            const char* want = c == Case::NoPsram      ? "PSRAM кончилась"
                               : c == Case::CatalogFull ? "в каталоге сэмплов нет места"
                               : c == Case::Truncated   ? "чтение оборвалось: сэмпл с 2108, всего 8 байт"
                                                        : "номер сэмпла вне песни";
            if (std::strncmp(reason, want, std::strlen(want)) != 0) std::printf("  причина: %s\n", reason);
            CHECK(std::strncmp(reason, want, std::strlen(want)) == 0);
        }
        CHECK_EQ(memory::psram_free_page_count(mem.psram), free_before);
        memory::track_memory_destroy(mem);
    }
}

// Полная загрузка файла, оборванного внутри PCM: load() успешна, отказ
// сэмпла посчитан с причиной, его страницы свободны; целый файл - без
// отказов.
void test_full_load_counts_failures() {
    std::printf("test_mod_full_load_counts_failures\n");
    const std::vector<uint8_t> f = build_synthetic_mod();
    for (const uint32_t cut : {0u, 4u}) {
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        formats::MemoryByteSource src(f.data(), static_cast<uint32_t>(f.size()) - cut);
        CHECK(formats::mod::load(src.as_byte_source(), mem, song, nullptr));
        const soundsinth::model::TrackerLoadStats& ls = soundsinth::model::g_tracker_load_stats;
        CHECK_EQ(ls.samples_failed, static_cast<uint16_t>(cut == 0 ? 0 : 1));
        CHECK_EQ(memory::sample_cache_find(mem.sample_cache, 0) == nullptr, cut != 0);
        if (cut != 0) {
            CHECK_EQ(ls.first_failed_sample, static_cast<uint16_t>(0));
            CHECK(ls.first_failure != nullptr);
            CHECK_EQ(memory::psram_free_page_count(mem.psram), mem.psram.sample_page_count);
        }
        memory::track_memory_destroy(mem);
    }
}

// Конец файла по заголовкам: у MOD длины сэмплов точные, поэтому сумма
// смещения и длины последнего сэмпла равна длине файла байт в байт. По этому
// числу поток GS видит недобор, которого не видят ни кольцо приёма, ни
// приёмный буфер: байт, потерянный в очереди записи шины, до автомата не
// доходит и нигде не считается.
void test_source_end_matches_file_size() {
    std::printf("test_mod_source_end_matches_file_size\n");
    const std::vector<uint8_t> f = build_synthetic_mod();
    for (const uint32_t missing : {0u, 1u}) {
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        Song song;
        const uint32_t size = static_cast<uint32_t>(f.size()) - missing;
        formats::MemoryByteSource src(f.data(), size);
        CHECK(formats::mod::load(src.as_byte_source(), mem, song, nullptr, true));
        const uint32_t end = soundsinth::model::g_tracker_load_stats.source_end;
        CHECK_EQ(end - size, missing);
        memory::track_memory_destroy(mem);
    }
}

} // namespace

void run_mod_loader_tests() {
    test_source_end_matches_file_size();
    test_full_load_counts_failures();
    test_sample_fail_reasons();
    test_signatures_and_header();
    test_preamp_and_period_round_trip();
    test_sample_loops();
    test_old_format_loops();
    test_loop_past_end();
    test_synthetic_exact();
    test_real_small_file_smoke();
    test_panning_heuristics();
    test_amiga_period_extended_octaves();
}
