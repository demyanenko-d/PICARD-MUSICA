// Повторная выдача строк .mid (midi::replay_*).
//
// У .mid упакованных паттернов нет: строки делает конвертер из файла по ходу
// игры. Секвенсор просит их по порядку, а назад ходит только прогоном с
// начала - на этом держится и перемотка, и повтор трека. Значит повтор
// обязан быть воспроизводимым: второй прогон даёт те же ячейки, что первый,
// до единого поля.
//
// Проверяется заодно нумерация инструментов: номер в ячейке - это индекс в
// Song::instruments, и сдвиг там означал бы чужой тембр. Сброс состояния
// перед повтором её не трогает намеренно.

#include "testing.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/bank/bank_reader.h"
#include "core/formats/midi.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"

namespace {

using namespace soundsinth;
using soundsinth::model::PatternCell;

std::vector<uint8_t> read_whole_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// По полям, а не memcmp: в PatternCell есть байт выравнивания, а у
// EffectCommand свой конструктор - значит дополнение не обнуляется, и
// побайтное сравнение ловило бы мусор.
bool same_cell(const PatternCell& a, const PatternCell& b) {
    return a.instrument == b.instrument && a.note == b.note && a.volume.type == b.volume.type &&
           a.volume.param == b.volume.param && a.effect.type == b.effect.type && a.effect.rate == b.effect.rate &&
           a.effect.param == b.effect.param;
}

void check_replay_is_repeatable(const bank::Bank& bank, const char* path) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  ПРОПУСК (файл не найден): %s\n", path);
        return;
    }

    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const char* err = nullptr;
    if (!formats::midi::load(src.as_byte_source(), static_cast<uint32_t>(bytes.size()), mem, bank, song, &err)) {
        std::printf("  ПРОПУСК (не загрузился: %s): %s\n", err ? err : "без причины", path);
        memory::track_memory_destroy(mem);
        return;
    }

    // Строк в треке столько же, сколько их в дескрипторах паттернов: данных
    // за ними нет, но позицию секвенсор ведёт по ним.
    uint32_t rows_in_patterns = 0;
    for (uint16_t p = 0; p < song.pattern_count; ++p) {
        rows_in_patterns += song.patterns[p].row_count;
        CHECK_EQ(song.patterns[p].psram_offset, soundsinth::model::Pattern::kInvalidOffset);
    }
    CHECK_EQ(formats::midi::replay_row_count(), rows_in_patterns);
    CHECK(formats::midi::last_total_frames() > 0);
    CHECK(song.row_fetch != nullptr);

    const uint8_t channels = song.patterns[0].channel_count;

    // Первый прогон - в память, второй сверяется с ним.
    std::vector<PatternCell> first;
    first.reserve(static_cast<size_t>(rows_in_patterns) * channels);
    formats::midi::replay_rewind();
    for (uint32_t r = 0; r < rows_in_patterns; ++r) {
        const PatternCell* cells = formats::midi::replay_next_row();
        if (cells == nullptr) {
            CHECK(false); // строки кончились раньше времени
            break;
        }
        for (uint8_t c = 0; c < channels; ++c) first.push_back(cells[c]);
    }
    CHECK(formats::midi::replay_next_row() == nullptr);

    uint32_t bad_cells = 0, bad_rows = 0, first_bad_row = 0;
    formats::midi::replay_rewind();
    for (uint32_t r = 0; r < rows_in_patterns; ++r) {
        const PatternCell* cells = formats::midi::replay_next_row();
        if (cells == nullptr) {
            CHECK(false);
            break;
        }
        bool row_ok = true;
        for (uint8_t c = 0; c < channels; ++c) {
            if (!same_cell(first[static_cast<size_t>(r) * channels + c], cells[c])) {
                ++bad_cells;
                row_ok = false;
            }
        }
        if (!row_ok) {
            if (bad_rows == 0) first_bad_row = r;
            ++bad_rows;
        }
    }

    CHECK_EQ(bad_rows, 0u);
    CHECK_EQ(bad_cells, 0u);
    std::printf("  %s: строк %u, каналов %u, длительность %u с, расхождений %u%s\n", path, rows_in_patterns, channels,
                formats::midi::last_total_frames() / 44100u, bad_cells, bad_rows ? "" : " - повтор совпал");
    if (bad_rows != 0) std::printf("    первая разошедшаяся строка: %u\n", first_bad_row);

    memory::track_memory_destroy(mem);
}

void test_replay_is_repeatable() {
    std::printf("test_replay_is_repeatable\n");
    const std::vector<uint8_t> blob = read_whole_file("release/banks/GeneralUser-GS.ssb");
    if (blob.empty()) {
        std::printf("  ПРОПУСК: банк release/banks/GeneralUser-GS.ssb не найден\n");
        return;
    }
    bank::Bank bank;
    const char* berr = nullptr;
    if (!bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bank, &berr)) {
        std::printf("  ПРОПУСК: банк не принят (%s)\n", berr ? berr : "без причины");
        return;
    }

    check_replay_is_repeatable(bank, "SD/test_music/midi/Bond.mid");
    check_replay_is_repeatable(bank, "SD/test_music/midi/Dance.mid");
    // Плотный файл: 64 канала, кражи каналов, слои по силе удара.
    check_replay_is_repeatable(bank, "music/src/midi/test/RHAPBLUE.MID");
}

} // namespace

void run_midi_replay_tests() {
    test_replay_is_repeatable();
}
