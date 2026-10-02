// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cstdio>
#include <vector>

#include "core/model/song.h"
#include "core/memory/psram_store.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_cell_codec.h"
#include "core/codec/pattern_packer.h"
#include "core/codec/pattern_reader.h"

using namespace soundsinth;
using soundsinth::model::Effect;
using soundsinth::model::PatternCell;
using soundsinth::model::VolumeColumnType;

namespace {

PatternCell make_cell(uint8_t note, uint8_t instrument, Effect fx = Effect::None, uint8_t fx_param = 0) {
    PatternCell c;
    c.note         = note;
    c.instrument   = instrument;
    c.effect.type  = fx;
    c.effect.param = fx_param;
    return c;
}

void test_roundtrip_exact() {
    std::printf("test_pattern_packer_roundtrip_exact\n");

    constexpr uint16_t kRowCount    = 6;
    constexpr uint8_t kChannelCount = 4;

    // Строится вручную, включая повторяющиеся ячейки (проверка
    // дедупликации словаря) и полностью пустые строки/каналы (проверка
    // битовой маски).
    std::vector<std::vector<PatternCell>> rows(kRowCount, std::vector<PatternCell>(kChannelCount));
    rows[0][0] = make_cell(36, 1); // C-3, инструмент 1
    rows[0][2] = make_cell(48, 2, Effect::SetVolume, 40);
    // rows[1] - полностью пустая строка
    rows[2][0] = make_cell(36, 1); // та же ячейка, что rows[0][0] - должна переиспользовать словарную запись
    rows[2][1] = make_cell(40, 1, Effect::PortaUp, 5);
    rows[3][3] = make_cell(soundsinth::model::kNoteNone, 0, Effect::NoteCut, 0); // без ноты, только эффект - всё равно активная ячейка
    rows[4][0] = make_cell(36, 1);                                               // третье использование той же ячейки
    rows[5][1] = make_cell(60, 3, Effect::Vibrato, 0x84);

    uint8_t buffer[soundsinth::memory::kPatternPackBufferBytes] = {};
    patterns::PatternPacker packer(buffer, sizeof(buffer), kRowCount, kChannelCount);
    for (uint16_t r = 0; r < kRowCount; ++r) {
        CHECK(packer.add_row(rows[r].data()));
    }
    CHECK(packer.ok());

    memory::PsramStore psram;
    memory::psram_create(psram);
    const uint32_t offset = packer.finish(psram);
    CHECK(offset != memory::kPatternAllocFailed);

    patterns::PatternReader reader(memory::psram_pattern_ptr(psram, offset), kRowCount, kChannelCount);
    for (uint16_t r = 0; r < kRowCount; ++r) {
        std::vector<PatternCell> decoded(kChannelCount);
        reader.read_row(r, decoded.data());
        for (uint8_t ch = 0; ch < kChannelCount; ++ch) {
            const PatternCell& expected = rows[r][ch];
            const PatternCell& got      = decoded[ch];
            CHECK_EQ(got.note, expected.note);
            CHECK_EQ(got.instrument, expected.instrument);
            CHECK(got.effect.type == expected.effect.type);
            CHECK_EQ(got.effect.param, expected.effect.param);
            CHECK(got.volume.type == expected.volume.type);
            CHECK_EQ(got.volume.param, expected.volume.param);
        }
    }

    memory::psram_destroy(psram);
}

// Пустые строки паттерна делят одно тело (таблица смещений указывает в одно
// место), непустые - свои. 40 каналов: активные ячейки и в старшем слове
// маски (каналы 32-39), читатель разбирает её двумя словами.
void test_empty_rows_share_one_body_and_high_channels_roundtrip() {
    std::printf("test_pattern_packer_empty_rows_share_one_body\n");

    constexpr uint16_t kRowCount    = 64;
    constexpr uint8_t kChannelCount = 40;
    std::vector<std::vector<PatternCell>> rows(kRowCount, std::vector<PatternCell>(kChannelCount));
    for (uint16_t r = 0; r < kRowCount; r += 4) {
        rows[r][r % kChannelCount] = make_cell(static_cast<uint8_t>(24 + r), 1);
        rows[r][39]                = make_cell(60, 2, Effect::SetVolume, static_cast<uint8_t>(r));
        rows[r][32]                = make_cell(soundsinth::model::kNoteNone, 0, Effect::NoteCut, 1);
    }

    uint8_t buffer[soundsinth::memory::kPatternPackBufferBytes] = {};
    patterns::PatternPacker packer(buffer, sizeof(buffer), kRowCount, kChannelCount);
    for (uint16_t r = 0; r < kRowCount; ++r)
        CHECK(packer.add_row(rows[r].data()));

    memory::PsramStore psram;
    memory::psram_create(psram);
    const uint32_t offset = packer.finish(psram);
    CHECK(offset != memory::kPatternAllocFailed);
    const uint8_t* block = memory::psram_pattern_ptr(psram, offset);

    auto row_offset           = [&](uint16_t r) { return static_cast<uint16_t>(block[2 + r * 2] | (block[3 + r * 2] << 8)); };
    const uint16_t empty_body = row_offset(1);
    for (uint16_t r = 0; r < kRowCount; ++r) {
        if (r % 4 == 0) {
            CHECK(row_offset(r) != empty_body);
        } else {
            CHECK_EQ(row_offset(r), empty_body);
        }
    }

    patterns::PatternReader reader(block, kRowCount, kChannelCount);
    std::vector<PatternCell> decoded(kChannelCount);
    for (uint16_t r = 0; r < kRowCount; ++r) {
        reader.read_row(r, decoded.data());
        for (uint8_t ch = 0; ch < kChannelCount; ++ch) {
            CHECK_EQ(decoded[ch].note, rows[r][ch].note);
            CHECK_EQ(decoded[ch].instrument, rows[r][ch].instrument);
            CHECK(decoded[ch].effect.type == rows[r][ch].effect.type);
            CHECK_EQ(decoded[ch].effect.param, rows[r][ch].effect.param);
            CHECK(decoded[ch].volume.type == rows[r][ch].volume.type);
        }
    }

    memory::psram_destroy(psram);
}

} // namespace

// Ветки отказа: строка или словарь не влезают в буфер, лишняя строка
// сверх row_count, finish после отказа. Границу буфера тест находит сам -
// наименьший, куда входят две одинаковые строки; на нём вторая строка с
// новой ячейкой обязана не влезть, а зона паттернов - остаться нетронутой.
void test_packer_failure_branches() {
    std::printf("test_pattern_packer_failure_branches\n");
    const PatternCell a = make_cell(36, 1, Effect::SetVolume, 40);
    const PatternCell b = make_cell(48, 2, Effect::PortaUp, 7);
    std::vector<uint8_t> buf(4096);
    auto fits = [&](uint32_t size, const PatternCell& second) {
        patterns::PatternPacker p(buf.data(), size, 2, 1);
        return p.add_row(&a) && p.add_row(&second);
    };
    uint32_t size = 1;
    while (size < buf.size() && !fits(size, a))
        ++size;
    CHECK(size < buf.size());
    std::printf("  buffer boundary %u bytes\n", size);
    CHECK(!fits(size - 1, a)); // на байт меньше - уже нет

    memory::PsramStore psram;
    memory::psram_create(psram);
    {
        patterns::PatternPacker p(buf.data(), size, 2, 1);
        CHECK(p.add_row(&a));
        CHECK(!p.add_row(&b)); // новая ячейка словаря не влезает
        CHECK(!p.ok());
        CHECK_EQ(p.finish(psram), memory::kPatternAllocFailed);
    }
    {
        // Зона паттернов не тронута отказом: удачная упаковка встаёт с нуля.
        patterns::PatternPacker p(buf.data(), static_cast<uint32_t>(buf.size()), 2, 1);
        CHECK(p.add_row(&a));
        CHECK(p.add_row(&a));
        CHECK(!p.add_row(&a)); // третья строка при row_count 2
        CHECK(!p.ok());
    }
    {
        patterns::PatternPacker p(buf.data(), static_cast<uint32_t>(buf.size()), 2, 1);
        CHECK(p.add_row(&a));
        CHECK(p.add_row(&b));
        CHECK_EQ(p.finish(psram), 0u);
    }
    memory::psram_destroy(psram);
}

// Тонкий и сверхтонкий слайд (SlideRate в двух битах рядом с типом) и
// последний тип эффекта переживают упаковку; 64 канала, активен только 63-й.
void test_packer_rate_and_last_channel_roundtrip() {
    std::printf("test_pattern_packer_rate_and_last_channel_roundtrip\n");
    constexpr uint8_t kCh = 64;
    std::vector<PatternCell> row0(kCh), row1(kCh), row2(kCh);
    row0[63]                                = make_cell(40, 1, Effect::PortaUp, 3);
    row0[63].effect.rate                    = soundsinth::model::SlideRate::Fine;
    row1[63]                                = make_cell(41, 1, Effect::PortaDown, 5);
    row1[63].effect.rate                    = soundsinth::model::SlideRate::ExtraFine;
    row2[63]                                = make_cell(42, 2, static_cast<Effect>(static_cast<uint8_t>(Effect::Count) - 1), 9);
    const std::vector<PatternCell>* rows[3] = {&row0, &row1, &row2};

    std::vector<uint8_t> buf(soundsinth::memory::kPatternPackBufferBytes);
    patterns::PatternPacker p(buf.data(), static_cast<uint32_t>(buf.size()), 3, kCh);
    for (const auto* r : rows)
        CHECK(p.add_row(r->data()));
    memory::PsramStore psram;
    memory::psram_create(psram);
    const uint32_t off = p.finish(psram);
    CHECK(off != memory::kPatternAllocFailed);
    patterns::PatternReader reader(memory::psram_pattern_ptr(psram, off), 3, kCh);
    for (uint16_t r = 0; r < 3; ++r) {
        std::vector<PatternCell> got(kCh);
        reader.read_row(r, got.data());
        const PatternCell& want = (*rows[r])[63];
        CHECK_EQ(got[63].note, want.note);
        CHECK(got[63].effect.type == want.effect.type);
        CHECK(got[63].effect.rate == want.effect.rate);
        CHECK_EQ(got[63].effect.param, want.effect.param);
        CHECK_EQ(got[62].note, soundsinth::model::kNoteNone);
    }
    memory::psram_destroy(psram);
}

// Поиск по словарю на частых совпадениях младшего слова (нота, инструмент,
// колонка громкости) при разном эффекте: ветка "слово совпало, полуслово
// нет" и хвостовая запись при нечётном размере словаря. Паттерн обязан
// прочитаться ячейка в ячейку.
void test_dict_lookup_word_collisions_roundtrip() {
    std::printf("test_pattern_packer_dict_lookup_word_collisions_roundtrip\n");
    constexpr uint16_t kRows = 64;
    constexpr uint8_t kCh    = 32;
    std::vector<PatternCell> cells(kRows * kCh);
    uint32_t x = 99;
    for (auto& c : cells) {
        x = x * 1664525u + 1013904223u;
        if ((x >> 28) < 3) continue; // часть ячеек пустая
        c = make_cell(static_cast<uint8_t>(40 + ((x >> 8) & 3)), 1, ((x >> 12) & 1) ? Effect::PortaUp : Effect::PortaDown, static_cast<uint8_t>((x >> 16) & 7));
    }
    std::vector<uint8_t> buf(soundsinth::memory::kPatternPackBufferBytes);
    patterns::PatternPacker p(buf.data(), static_cast<uint32_t>(buf.size()), kRows, kCh);
    for (uint16_t r = 0; r < kRows; ++r)
        CHECK(p.add_row(&cells[r * kCh]));
    memory::PsramStore psram;
    memory::psram_create(psram);
    const uint32_t off = p.finish(psram);
    CHECK(off != memory::kPatternAllocFailed);
    patterns::PatternReader reader(memory::psram_pattern_ptr(psram, off), kRows, kCh);
    uint32_t bad = 0;
    std::vector<PatternCell> got(kCh);
    for (uint16_t r = 0; r < kRows; ++r) {
        reader.read_row(r, got.data());
        for (uint8_t ch = 0; ch < kCh; ++ch) {
            const PatternCell& w = cells[r * kCh + ch];
            if (got[ch].note != w.note || got[ch].instrument != w.instrument || got[ch].effect.type != w.effect.type || got[ch].effect.param != w.effect.param)
                ++bad;
        }
    }
    CHECK_EQ(bad, 0u);
    memory::psram_destroy(psram);
}

// Номер инструмента - 9 бит: младший байт и бит 7 байта типа колонки
// громкости. Все номера 0..511 со всеми типами колонки и с параметрами
// эффекта проходят кодек туда и обратно без потерь и не задевают соседей.
void test_cell_codec_instrument_9_bits() {
    std::printf("test_cell_codec_instrument_9_bits\n");
    int bad = 0;
    for (uint32_t inst = 0; inst <= patterns::kMaxCellInstrument; ++inst) {
        for (uint32_t vt = 0; vt <= static_cast<uint32_t>(VolumeColumnType::Offset); ++vt) {
            PatternCell c;
            c.instrument   = static_cast<uint16_t>(inst);
            c.note         = static_cast<uint8_t>(inst % 120);
            c.volume.type  = static_cast<VolumeColumnType>(vt);
            c.volume.param = static_cast<uint8_t>(inst ^ 0x5A);
            c.effect.type  = Effect::SetPanning;
            c.effect.param = static_cast<uint8_t>(inst * 7);
            uint8_t enc[patterns::kEncodedCellBytes];
            patterns::encode_cell(c, enc);
            const PatternCell d = patterns::decode_cell(enc);
            if (d.instrument != c.instrument || d.note != c.note || d.volume.type != c.volume.type || d.volume.param != c.volume.param ||
                d.effect.type != c.effect.type || d.effect.param != c.effect.param) {
                ++bad;
            }
        }
    }
    std::printf("  differences: %d\n", bad);
    CHECK_EQ(bad, 0);
}

void run_pattern_packer_tests() {
    test_cell_codec_instrument_9_bits();
    test_dict_lookup_word_collisions_roundtrip();
    test_packer_failure_branches();
    test_packer_rate_and_last_channel_roundtrip();
    test_roundtrip_exact();
    test_empty_rows_share_one_body_and_high_channels_roundtrip();
}
