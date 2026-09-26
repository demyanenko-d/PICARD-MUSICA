#include "testing.h"

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/bank/bank_reader.h"
#include "core/model/song.h"
#include "core/formats/midi.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_reader.h"

using namespace soundsinth;

namespace {

constexpr const char* kBankPath = "release/banks/GeneralUser-GS.ssb";

// SMF формата 0 из одной дорожки: заголовок, MTrk и байты событий как есть.
std::vector<uint8_t> make_smf(uint16_t division, std::initializer_list<uint8_t> events) {
    std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1,
                              static_cast<uint8_t>(division >> 8), static_cast<uint8_t>(division)};
    const uint32_t len = static_cast<uint32_t>(events.size());
    f.insert(f.end(), {'M', 'T', 'r', 'k', static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                       static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)});
    f.insert(f.end(), events.begin(), events.end());
    return f;
}

// Строк в паттерне у .mid - как у OpenMPT при импорте.
constexpr uint32_t kMidiRowsPerPattern = 128;

// Замена PatternReader для .mid: строк в зоне паттернов у него нет,
// конвертер делает их из файла. Интерфейс тот же, чтобы проверки остались
// как были.
struct MidiRows {
    uint8_t channels;
    uint32_t base; // номер первой строки этого паттерна от начала трека

    void read_row(uint16_t row, soundsinth::model::PatternCell* out) const {
        for (uint8_t c = 0; c < channels; ++c) out[c] = soundsinth::model::PatternCell{};
        formats::midi::replay_row_at(base + row, out, channels);
    }
};

struct NoteRows {
    int notes = 0;     // ячеек с настоящей нотой
    int offs = 0;      // ячеек kNoteOff
    int first_on = -1; // абсолютная строка первой ноты
    int first_off = -1;
};

// Проход по всем строкам песни в порядке order: где стоят ноты и снятия.
// Строки берутся у источника, а не из зоны паттернов: у .mid их там нет -
// конвертер делает их из файла по ходу игры.
NoteRows scan_rows(const soundsinth::model::Song& song, memory::TrackMemory& /*mem*/) {
    NoteRows r;
    int abs_row = 0;
    formats::midi::replay_rewind();
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        for (uint16_t row = 0; row < pat.row_count; ++row, ++abs_row) {
            const soundsinth::model::PatternCell* cells = formats::midi::replay_next_row();
            if (cells == nullptr) return r;
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (soundsinth::model::is_real_note(cells[c].note)) {
                    ++r.notes;
                    if (r.first_on < 0) r.first_on = abs_row;
                } else if (cells[c].note == soundsinth::model::kNoteOff) {
                    ++r.offs;
                    if (r.first_off < 0) r.first_off = abs_row;
                }
            }
        }
    }
    return r;
}

bool load_smf(const std::vector<uint8_t>& file, const bank::Bank& bnk, memory::TrackMemory& mem,
              soundsinth::model::Song& song) {
    memory::track_memory_reset_for_new_track(mem);
    formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
    const char* err = nullptr;
    const bool ok = formats::midi::load(src.as_byte_source(), static_cast<uint32_t>(file.size()), mem, bnk, song,
                                        &err, true);
    if (!ok) std::printf("  load: %s\n", err ? err : "?");
    return ok;
}

// Running status заводят только сообщения канала: байт данных после meta
// или SysEx продолжает прошлое сообщение канала, как у OpenMPT. Раньше
// статусом становился и 0xFF: снятие ноты ниже читалось как ещё одно meta,
// дорожка сбивалась (THEDANCE.MID - 880 note-on вместо 1413).
void test_running_status_survives_meta_and_sysex() {
    std::printf("test_running_status_survives_meta_and_sysex\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    // Нота 60 на тике 0, снятие (note-on с velocity 0 по running status) на
    // тике 96, PPQN 96.
    const std::vector<uint8_t> plain = make_smf(96, {0x00, 0x90, 0x3C, 0x64, 0x60, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    const std::vector<uint8_t> meta =
        make_smf(96, {0x00, 0x90, 0x3C, 0x64, 0x00, 0xFF, 0x01, 0x00, 0x60, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    const std::vector<uint8_t> sysex =
        make_smf(96, {0x00, 0x90, 0x3C, 0x64, 0x00, 0xF0, 0x01, 0xF7, 0x60, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});

    soundsinth::model::Song song;
    CHECK(load_smf(plain, bnk, mem, song));
    const NoteRows ref = scan_rows(song, mem);
    CHECK(ref.notes >= 1);
    CHECK(ref.offs >= 1);
    CHECK(ref.first_off > ref.first_on);
    for (const auto* file : {&meta, &sysex}) {
        soundsinth::model::Song s;
        CHECK(load_smf(*file, bnk, mem, s));
        const NoteRows r = scan_rows(s, mem);
        CHECK_EQ(r.notes, ref.notes);
        CHECK_EQ(r.offs, ref.offs);
        CHECK_EQ(r.first_on, ref.first_on);
        CHECK_EQ(r.first_off, ref.first_off);
    }
    memory::track_memory_destroy(mem);
}

// Каналов 64: 70 нот подряд без снятий заставляют красть самые давние
// каналы. Снятие первой ноты (30, её каналы украдены) и ноты 95 на одной
// строке обязано попасть только в каналы ноты 95 - не в каналы, отданные
// новым нотам (раньше слот held ноты 30 указывал на украденный канал, и
// новая нота глохла).
void test_stolen_channel_keeps_new_note() {
    std::printf("test_stolen_channel_keeps_new_note\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    // Ноты 30..99 через 12 тиков, снятие 30 и 95 через 96 тиков после последней.
    std::vector<uint8_t> ev;
    for (uint8_t i = 0; i < 70; ++i) {
        ev.insert(ev.end(), {static_cast<uint8_t>(i == 0 ? 0x00 : 0x0C), 0x90, static_cast<uint8_t>(30 + i), 0x64});
    }
    ev.insert(ev.end(), {0x60, 0x80, 30, 0x00, 0x00, 0x80, 95, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    const uint32_t len = static_cast<uint32_t>(ev.size());
    std::vector<uint8_t> file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k',
                                 static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                                 static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
    file.insert(file.end(), ev.begin(), ev.end());

    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    // Первая строка со снятием - строка note-off: в ней kNoteOff только у
    // каналов, где сейчас звучит нота 95.
    uint8_t cur[64];
    for (uint8_t& n : cur) n = soundsinth::model::kNoteNone;
    int wrong = 0, offs = 0;
    bool seen_off = false;
    soundsinth::model::PatternCell cells[64];
    for (uint32_t o = 0; o < song.order_count && !seen_off; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count && !seen_off; ++row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (cells[c].note == soundsinth::model::kNoteOff) {
                    seen_off = true;
                    ++offs;
                    if (cur[c] != 95) ++wrong;
                } else if (soundsinth::model::is_real_note(cells[c].note)) {
                    cur[c] = cells[c].note;
                }
            }
        }
    }
    std::printf("  снятий на строке note-off: %d, из них в чужих каналах: %d\n", offs, wrong);
    CHECK(offs >= 1);
    CHECK_EQ(wrong, 0);
    memory::track_memory_destroy(mem);
}

// Две дорожки на одном канале удваивают партию: вторая нота той же клавиши
// на той же строке берёт новые каналы. Раньше прежние выпадали из учёта и
// висели до следующего нажатия; теперь каждый канал с этой нотой получает
// kNoteOff от снятия.
void test_doubled_note_released_everywhere() {
    std::printf("test_doubled_note_released_everywhere\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    // Нота 60 дважды на тике 0, снятия на тиках 48 и 96.
    const std::vector<uint8_t> file = make_smf(96, {0x00, 0x90, 0x3C, 0x64, 0x00, 0x90, 0x3C, 0x64, 0x30, 0x80, 0x3C,
                                                    0x00, 0x30, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    bool has_note[64] = {};
    bool has_off[64] = {};
    soundsinth::model::PatternCell cells[64];
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count; ++row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (cells[c].note == 60) has_note[c] = true;
                if (cells[c].note == soundsinth::model::kNoteOff) has_off[c] = true;
            }
        }
    }
    int notes = 0, hanging = 0;
    for (uint32_t c = 0; c < 64; ++c) {
        if (!has_note[c]) continue;
        ++notes;
        if (!has_off[c]) ++hanging;
    }
    std::printf("  каналов с нотой %d, без снятия %d\n", notes, hanging);
    CHECK(notes >= 2);
    CHECK_EQ(hanging, 0);
    memory::track_memory_destroy(mem);
}

void put_varint(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t buf[5];
    int n = 0;
    do {
        buf[n++] = static_cast<uint8_t>(x & 0x7F);
        x >>= 7;
    } while (x != 0);
    while (n-- > 0) v.push_back(static_cast<uint8_t>(buf[n] | (n ? 0x80 : 0)));
}

// Битый файл: division 1 и событие на тике 2^28 + 50. Число строк за 2^32,
// раньше оно усекалось до проверки длины, трек проходил, и нота на тике
// 89478486 ложилась на десятую строку. Теперь - отказ.
// Нулевой темп - 120 BPM, как явные 500000 мкс на долю (раньше 6e7 / 0 шло
// в приведение к uint32).
void test_broken_timing_rejected_or_defaulted() {
    std::printf("test_broken_timing_rejected_or_defaulted\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    std::vector<uint8_t> ev = {0x00, 0x90, 0x3C, 0x64};
    put_varint(ev, 89478486u);
    ev.insert(ev.end(), {0x90, 0x3E, 0x64});
    put_varint(ev, (1u << 28) + 50u - 89478486u);
    ev.insert(ev.end(), {0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    std::vector<uint8_t> file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 1, 'M', 'T', 'r', 'k', 0, 0, 0,
                                 static_cast<uint8_t>(ev.size())};
    file.insert(file.end(), ev.begin(), ev.end());
    soundsinth::model::Song song;
    CHECK(!load_smf(file, bnk, mem, song));

    const std::vector<uint8_t> t120 = make_smf(96, {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x00, 0x90, 0x3C, 0x64,
                                                    0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    const std::vector<uint8_t> zero = make_smf(96, {0x00, 0xFF, 0x51, 0x03, 0x00, 0x00, 0x00, 0x00, 0x90, 0x3C, 0x64,
                                                    0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song ref;
    CHECK(load_smf(t120, bnk, mem, ref));
    soundsinth::model::Song s;
    CHECK(load_smf(zero, bnk, mem, s));
    std::printf("  темп при 120 BPM %u, при нулевом темпе %u\n", unsigned(ref.default_tempo), unsigned(s.default_tempo));
    CHECK_EQ(s.default_tempo, ref.default_tempo);

    // Без смены темпа - те же 120 BPM, та же сетка. Раньше выигрывала первая
    // сетка 32/6, темп 960 упирался в 255, трек шёл вчетверо медленнее.
    const std::vector<uint8_t> none = make_smf(96, {0x00, 0x90, 0x3C, 0x64, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song n;
    CHECK(load_smf(none, bnk, mem, n));
    std::printf("  без смены темпа: темп %u, скорость %u (при 120 BPM %u, %u)\n", unsigned(n.default_tempo),
                unsigned(n.default_speed), unsigned(ref.default_tempo), unsigned(ref.default_speed));
    CHECK_EQ(n.default_tempo, ref.default_tempo);
    CHECK_EQ(n.default_speed, ref.default_speed);
    memory::track_memory_destroy(mem);
}

// Сколько каналов берёт клавиша key программы prog на канале MIDI 0.
int layers_of(uint8_t prog, uint8_t key, const bank::Bank& bnk, memory::TrackMemory& mem) {
    const std::vector<uint8_t> f =
        make_smf(96, {0x00, 0xC0, prog, 0x00, 0x90, key, 0x64, 0x60, 0x80, key, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song s;
    if (!load_smf(f, bnk, mem, s)) return -1;
    return scan_rows(s, mem).notes;
}

// CC123 снимает ноту, у которой первый слой уже отдан: двуслойная нота
// на канале MIDI 0, 62 однослойные на канале 1 занимают остальные каналы,
// ещё одна крадёт самый давний - первый слой двуслойной. CC123 на канале 0
// обязан снять второй слой. Раньше проверялся только первый слот, и второй
// слой висел до кражи.
void test_all_notes_off_releases_every_layer() {
    std::printf("test_all_notes_off_releases_every_layer\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    // Двуслойная программа на клавише 60 и программа с 63 однослойными
    // клавишами кроме 60 - чтобы кража была ровно одна.
    int two = -1, one = -1;
    std::vector<uint8_t> keys;
    for (int p = 0; p < 128 && two < 0; ++p) {
        if (layers_of(static_cast<uint8_t>(p), 60, bnk, mem) == 2) two = p;
    }
    for (int p = 0; p < 128 && one < 0; ++p) {
        if (p == two) continue;
        keys.clear();
        for (int k = 24; k < 120 && keys.size() < 63; ++k) {
            if (k != 60 && layers_of(static_cast<uint8_t>(p), static_cast<uint8_t>(k), bnk, mem) == 1) {
                keys.push_back(static_cast<uint8_t>(k));
            }
        }
        if (keys.size() == 63) one = p;
    }
    std::printf("  двуслойная программа %d, однослойная %d\n", two, one);
    CHECK(two >= 0 && one >= 0);
    if (two < 0 || one < 0) {
        memory::track_memory_destroy(mem);
        return;
    }

    std::vector<uint8_t> ev = {0x00, 0xC0, static_cast<uint8_t>(two), 0x00, 0xC1, static_cast<uint8_t>(one),
                               0x00, 0x90, 0x3C, 0x64};
    for (const uint8_t key : keys) ev.insert(ev.end(), {0x0C, 0x91, key, 0x64});
    ev.insert(ev.end(), {0x60, 0xB0, 123, 0x00, 0x60, 0xFF, 0x2F, 0x00});
    std::vector<uint8_t> file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0,
                                 static_cast<uint8_t>(ev.size() >> 8), static_cast<uint8_t>(ev.size())};
    file.insert(file.end(), ev.begin(), ev.end());
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));

    // Каналы, где звучит нота 60 к концу трека (не украдены) - каждому
    // нужен kNoteOff после взятия ноты.
    uint8_t cur[64];
    bool off_after[64] = {};
    for (uint8_t& n : cur) n = soundsinth::model::kNoteNone;
    soundsinth::model::PatternCell cells[64];
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count; ++row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (soundsinth::model::is_real_note(cells[c].note)) {
                    cur[c] = cells[c].note;
                    off_after[c] = false;
                } else if (cells[c].note == soundsinth::model::kNoteOff) {
                    off_after[c] = true;
                }
            }
        }
    }
    int alive = 0, hanging = 0;
    for (uint32_t c = 0; c < 64; ++c) {
        if (cur[c] != 60) continue;
        ++alive;
        if (!off_after[c]) ++hanging;
    }
    std::printf("  каналов с нотой 60 к концу %d, без снятия %d\n", alive, hanging);
    CHECK_EQ(alive, 1);
    CHECK_EQ(hanging, 0);
    memory::track_memory_destroy(mem);
}

// Файл: программы first..last по 6 клавиш, затем ударные на клавишах 1 и 2
// (у наборов банка там зон нет). Каждая нота - на своей строке.
std::vector<uint8_t> programs_file(int first, int last, int& note_ons) {
    std::vector<uint8_t> ev;
    note_ons = 0;
    for (int p = first; p <= last; ++p) {
        ev.insert(ev.end(), {0x00, 0xC0, static_cast<uint8_t>(p)});
        for (const uint8_t key : {36, 48, 60, 72, 84, 96}) {
            ev.insert(ev.end(), {0x0C, 0x90, key, 0x64, 0x00, 0x80, key, 0x00});
            ++note_ons;
        }
    }
    for (const uint8_t key : {1, 2}) {
        ev.insert(ev.end(), {0x0C, 0x99, key, 0x64, 0x00, 0x89, key, 0x00});
        ++note_ons;
    }
    ev.insert(ev.end(), {0x60, 0xFF, 0x2F, 0x00});
    const uint32_t len = static_cast<uint32_t>(ev.size());
    std::vector<uint8_t> file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k',
                                 static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                                 static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
    file.insert(file.end(), ev.begin(), ev.end());
    return file;
}

// Ноты без звука считаются. 128 программ по 6 клавиш просят больше 255
// инструментов банка: номер в ячейке 9-битный, всем хватает, нот сверх
// потолка нет; загрузку GeneralUser тут останавливает арена, а не номера.
// На 16 программах строки со звучащей нотой вместе с нотами без зоны дают
// все note-on.
void test_silent_notes_counted() {
    std::printf("test_silent_notes_counted\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    int note_ons = 0;
    {
        const std::vector<uint8_t> big = programs_file(0, 127, note_ons);
        soundsinth::model::Song song;
        CHECK(!load_smf(big, bnk, mem, song));
        const auto& st = formats::midi::last_load_stats();
        std::printf("  128 программ: note-on %d, сверх 511 инструментов %u (инструментов %u)\n", note_ons,
                    unsigned(st.notes_over_cap), unsigned(st.instruments));
        CHECK(st.instruments > 255u);
        CHECK_EQ(st.notes_over_cap, 0u);
    }

    const std::vector<uint8_t> file = programs_file(0, 15, note_ons);
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    const auto& st = formats::midi::last_load_stats();

    int rows_with_note = 0;
    soundsinth::model::PatternCell cells[64];
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count; ++row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (soundsinth::model::is_real_note(cells[c].note)) {
                    ++rows_with_note;
                    break;
                }
            }
        }
    }
    std::printf("  16 программ: note-on %d, звучат %d, сверх 511 инструментов %u, без зоны %u\n", note_ons,
                rows_with_note, unsigned(st.notes_over_cap), unsigned(st.notes_no_zone));
    // При успешной загрузке арена в LoadStats - настоящая, а не оценка.
    std::printf("  арена: LoadStats %u, arena_used %u\n", unsigned(st.arena_bytes),
                unsigned(memory::arena_used(mem.resident)));
    CHECK_EQ(st.arena_bytes, static_cast<uint32_t>(memory::arena_used(mem.resident)));
    CHECK_EQ(st.notes_over_cap, 0u);
    CHECK(st.notes_no_zone >= 2);
    CHECK_EQ(rows_with_note + int(st.notes_no_zone), note_ons);
    memory::track_memory_destroy(mem);
}

// CC7 или CC11 = 0 - тишина, а не 1/64 колонки: нота на канале 0 взята при
// CC7 = 0, нота на канале 1 глушится CC11 = 0 посреди звучания. Минимум 1
// остаётся тихой ноте с ненулевыми контроллерами (velocity 1).
void test_zero_controller_silences() {
    std::printf("test_zero_controller_silences\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    // Канал 0: CC7 0, нота 60. Канал 1: нота 64, через 48 тиков CC11 0.
    // Канал 2: нота 67 с velocity 1.
    const std::vector<uint8_t> file =
        make_smf(96, {0x00, 0xB0, 0x07, 0x00, 0x00, 0x90, 0x3C, 0x64, 0x00, 0x91, 0x40, 0x64, 0x00, 0x92, 0x43,
                      0x01, 0x30, 0xB1, 0x0B, 0x00, 0x30, 0x80, 0x3C, 0x00, 0x00, 0x81, 0x40, 0x00, 0x00, 0x82,
                      0x43, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    int vol60 = -1, vol64_last = -1, vol67 = -1;
    uint8_t cur[64];
    for (uint8_t& n : cur) n = soundsinth::model::kNoteNone;
    soundsinth::model::PatternCell cells[64];
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count; ++row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (soundsinth::model::is_real_note(cells[c].note)) cur[c] = cells[c].note;
                if (cells[c].volume.type != soundsinth::model::VolumeColumnType::SetVolume) continue;
                const int v = cells[c].volume.param;
                if (cur[c] == 60 && vol60 < 0) vol60 = v;
                if (cur[c] == 64) vol64_last = v;
                if (cur[c] == 67 && vol67 < 0) vol67 = v;
            }
        }
    }
    std::printf("  взята при CC7 0: %d, после CC11 0: %d, velocity 1: %d\n", vol60, vol64_last, vol67);
    CHECK_EQ(vol60, 0);
    CHECK_EQ(vol64_last, 0);
    CHECK_EQ(vol67, 1);
    memory::track_memory_destroy(mem);
}

// Панорама CC10 на строке ноты: без задержки - колонкой эффекта той же
// строки (с тика 0), а не колонкой громкости следующей. Бенд на той же
// строке вытесняет её в колонку громкости следующей строки; задержанная
// нота - как раньше, следующей строкой.
void test_note_pan_on_note_row() {
    std::printf("test_note_pan_on_note_row\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    // CC10 0 на каналах 0-2. Канал 0: нота 60 на тике 0. Канал 1: нота 64 на
    // тике 0 и бенд там же. Канал 2: нота 67 на тике 2 (задержка внутри строки).
    const std::vector<uint8_t> file =
        make_smf(96, {0x00, 0xB0, 0x0A, 0x00, 0x00, 0xB1, 0x0A, 0x00, 0x00, 0xB2, 0x0A, 0x00, 0x00, 0x90, 0x3C, 0x64,
                      0x00, 0x91, 0x40, 0x64, 0x00, 0xE1, 0x00, 0x50, 0x02, 0x92, 0x43, 0x64, 0x60, 0x80, 0x3C,
                      0x00, 0x00, 0x81, 0x40, 0x00, 0x00, 0x82, 0x43, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    // Для каждой ноты: строка ноты, панорама эффектом на ней, панорама колонкой
    // громкости на следующей строке.
    struct Seen {
        int row = -1;
        bool fx_pan = false, vol_pan_next = false;
    } seen[128];
    uint8_t cur[64];
    for (uint8_t& n : cur) n = soundsinth::model::kNoteNone;
    soundsinth::model::PatternCell cells[64];
    int abs_row = 0;
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count; ++row, ++abs_row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (soundsinth::model::is_real_note(cells[c].note)) {
                    cur[c] = cells[c].note;
                    if (seen[cur[c]].row < 0) seen[cur[c]].row = abs_row;
                }
                if (cur[c] >= 128) continue;
                Seen& s = seen[cur[c]];
                if (abs_row == s.row && cells[c].effect.type == soundsinth::model::Effect::SetPanning) s.fx_pan = true;
                if (abs_row == s.row + 1 && cells[c].volume.type == soundsinth::model::VolumeColumnType::SetPanning) {
                    s.vol_pan_next = true;
                }
            }
        }
    }
    std::printf("  нота: эффект на строке / громкость следующей - 60: %d/%d, 64 с бендом: %d/%d, 67 задержана: %d/%d\n",
                seen[60].fx_pan, seen[60].vol_pan_next, seen[64].fx_pan, seen[64].vol_pan_next, seen[67].fx_pan,
                seen[67].vol_pan_next);
    CHECK(seen[60].fx_pan && !seen[60].vol_pan_next);
    CHECK(!seen[64].fx_pan && seen[64].vol_pan_next);
    CHECK(!seen[67].fx_pan && seen[67].vol_pan_next);
    memory::track_memory_destroy(mem);
}

// Бенд доходит и до отпущенной ноты, пока звучит её релиз: флейта с бендом
// +2, снятие, сброс бенда через 4 тика. Канал получает SetPitchOffset 0 на
// строке сброса - следующая нота с задержкой (колонка эффекта занята)
// начинается уже без старого бенда, хвост гнётся, как у SF2.
void test_bend_reaches_released_note() {
    std::printf("test_bend_reaches_released_note\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    const std::vector<uint8_t> file = make_smf(96, {0x00, 0xC0, 73, 0x00, 0x90, 69, 0x64, 0x00, 0xE0, 0x7F, 0x7F, 0x60,
                                                    0x80, 69, 0x00, 0x04, 0xE0, 0x00, 0x40, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    bool off_seen = false;
    int reset_after_off = 0;
    uint8_t cur[64];
    for (uint8_t& n : cur) n = soundsinth::model::kNoteNone;
    soundsinth::model::PatternCell cells[64];
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count; ++row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (soundsinth::model::is_real_note(cells[c].note)) cur[c] = cells[c].note;
                if (cur[c] != 69) continue;
                if (cells[c].note == soundsinth::model::kNoteOff) off_seen = true;
                if (off_seen && cells[c].effect.type == soundsinth::model::Effect::SetPitchOffset &&
                    cells[c].effect.param == 128) {
                    ++reset_after_off;
                }
            }
        }
    }
    std::printf("  сбросов бенда у отпущенной ноты: %d\n", reset_after_off);
    CHECK(off_seen);
    CHECK(reset_after_off >= 1);
    memory::track_memory_destroy(mem);
}

// Глубина вибрато по CC1 - ближайший шаг к линейной шкале SF2 (50 центов
// на 127): CC1 10 - без вибрато (раньше глубина 1, вшестеро глубже), 64 -
// 1, 127 - 2. Нота на каждом из трёх каналов MIDI.
// Клавиши 120..127 у .mid доходят до ячейки: шкала движка - 0..127, как у
// MIDI. В архиве такие ноты есть у 0.51% файлов, у одного - четверть всех.
// Где у банка зоны нет, нота считается в "нет зоны", а не теряется молча.
void test_high_notes_reach_cells() {
    std::printf("test_high_notes_reach_cells\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    // Программа 80 (соло-синтезатор) - у неё зоны доходят до верхних клавиш.
    const std::vector<uint8_t> file = make_smf(96, {0x00, 0xC0, 0x50, 0x00, 0x90, 0x7C, 0x64, 0x60, 0x80, 0x7C, 0x00,
                                                    0x00, 0x90, 0x7F, 0x64, 0x60, 0x80, 0x7F, 0x00, 0x00, 0xFF, 0x2F,
                                                    0x00});
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));

    int high = 0, highest = -1;
    soundsinth::model::PatternCell cells[64];
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count; ++row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (!soundsinth::model::is_real_note(cells[c].note)) continue;
                if (cells[c].note < 120) continue;
                ++high;
                if (cells[c].note > highest) highest = cells[c].note;
            }
        }
    }
    std::printf("  нот 120..127 в ячейках: %d, самая высокая %d\n", high, highest);
    CHECK(high >= 2);
    CHECK_EQ(highest, 127);
    memory::track_memory_destroy(mem);
}

void test_modwheel_depth_scale() {
    std::printf("test_modwheel_depth_scale\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    const std::vector<uint8_t> file =
        make_smf(96, {0x00, 0xB0, 0x01, 10, 0x00, 0xB1, 0x01, 64, 0x00, 0xB2, 0x01, 127, 0x00, 0x90, 60, 0x64, 0x00,
                      0x91, 64, 0x64, 0x00, 0x92, 67, 0x64, 0x83, 0x00, 0x80, 60, 0x00, 0x00, 0x81, 64, 0x00, 0x00,
                      0x82, 67, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    // Для ноты: максимальная глубина из колонки громкости и нибла FineVibrato / 4.
    int depth[128];
    for (int& d : depth) d = -1;
    uint8_t cur[64];
    for (uint8_t& n : cur) n = soundsinth::model::kNoteNone;
    soundsinth::model::PatternCell cells[64];
    for (uint32_t o = 0; o < song.order_count; ++o) {
        const soundsinth::model::Pattern& pat = song.patterns[song.order[o]];
        const MidiRows reader{pat.channel_count, static_cast<uint32_t>(o) * kMidiRowsPerPattern};
        for (uint16_t row = 0; row < pat.row_count; ++row) {
            reader.read_row(row, cells);
            for (uint32_t c = 0; c < pat.channel_count; ++c) {
                if (soundsinth::model::is_real_note(cells[c].note)) cur[c] = cells[c].note;
                if (cur[c] >= 128) continue;
                int d = -1;
                if (cells[c].volume.type == soundsinth::model::VolumeColumnType::VibratoDepth) d = cells[c].volume.param;
                if (cells[c].volume.type == soundsinth::model::VolumeColumnType::VibratoSpeed) d = 0;
                if (cells[c].effect.type == soundsinth::model::Effect::FineVibrato) d = (cells[c].effect.param & 0x0F) / 4;
                if (d > depth[cur[c]]) depth[cur[c]] = d;
            }
        }
    }
    std::printf("  глубина: CC1 10 -> %d, 64 -> %d, 127 -> %d (-1 - ни одной ячейки вибрато)\n", depth[60], depth[64],
                depth[67]);
    CHECK_EQ(depth[60], -1);
    CHECK_EQ(depth[64], 1);
    CHECK_EQ(depth[67], 2);
    memory::track_memory_destroy(mem);
}

// Огибающие громкости и фильтра - индексы одной таблицы банка. У
// инструмента, где они совпали, копия в арене одна на оба поля: поиск
// уже скопированной смотрит и огибающую громкости самого инструмента.
// Банк правится в памяти: у слоёв фортепиано фильтру отдаётся огибающая
// громкости.
void test_shared_envelope_copied_once() {
    std::printf("test_shared_envelope_copied_once\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    const bank::BankPreset& piano = bank::bank_preset(bnk, 0, 0);
    int patched = 0;
    for (uint16_t l = 0; l < bank::preset_layer_count(piano); ++l) {
        auto& bi = const_cast<bank::BankInstrument&>(bnk.instruments[bnk.layers[piano.first_layer + l].instrument]);
        if (bi.env_volume == bank::kNoIndex) continue;
        bi.env_filter = bi.env_volume;
        ++patched;
    }
    CHECK(patched > 0);
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    const std::vector<uint8_t> file =
        make_smf(96, {0x00, 0xC0, 0x00, 0x00, 0x90, 0x3C, 0x64, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00});
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    int shared = 0, both = 0;
    for (uint16_t i = 0; i < song.instrument_count; ++i) {
        const auto& ins = song.instruments[i];
        if (!ins.volume_envelope || !ins.filter_envelope) continue;
        ++both;
        if (ins.volume_envelope == ins.filter_envelope) ++shared;
    }
    std::printf("  инструментов с обеими огибающими %d, из них одна копия на оба поля %d\n", both, shared);
    CHECK(both > 0);
    CHECK_EQ(shared, both);
    memory::track_memory_destroy(mem);
}

// Смены темпа до первой ноты в двух дорожках, в обратном порядке времени:
// дорожка 0 - 60 BPM на тике 100 и нота на 200, дорожка 1 - 240 BPM на тике
// 50. Пауза срезается, оба тика прижимаются к нулю, но порядок - по сырым
// тикам: трек начинается с 240 BPM, на строке 0 побеждает последний перед
// нотой - смена на 60 BPM.
void test_lead_trim_keeps_tempo_order() {
    std::printf("test_lead_trim_keeps_tempo_order\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    const std::vector<uint8_t> t0 = {0x64, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40, 0x64, 0x90, 0x3C, 0x64,
                                     0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};
    const std::vector<uint8_t> t1 = {0x32, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90, 0x00, 0xFF, 0x2F, 0x00};
    std::vector<uint8_t> file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0, 96};
    for (const auto* tr : {&t0, &t1}) {
        const uint32_t len = static_cast<uint32_t>(tr->size());
        file.insert(file.end(), {'M', 'T', 'r', 'k', 0, 0, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)});
        file.insert(file.end(), tr->begin(), tr->end());
    }
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    int row0_tempo = -1;
    soundsinth::model::PatternCell cells[64];
    const soundsinth::model::Pattern& pat = song.patterns[song.order[0]];
    const MidiRows reader{pat.channel_count, 0};
    reader.read_row(0, cells);
    for (uint32_t c = 0; c < pat.channel_count; ++c) {
        if (cells[c].effect.type == soundsinth::model::Effect::SetTempo) row0_tempo = cells[c].effect.param;
    }
    std::printf("  начальный темп %u, смена на строке 0 %d\n", unsigned(song.default_tempo), row0_tempo);
    CHECK(row0_tempo > 0);
    CHECK(row0_tempo < int(song.default_tempo));
    memory::track_memory_destroy(mem);
}

// Потери раскладки считаются: 70 удержанных нот при 64 каналах - кражи
// звучащих каналов (не меньше 6); 1100 смен темпа при карте на 1024 -
// 76 отброшенных.
void test_layout_losses_counted() {
    std::printf("test_layout_losses_counted\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    std::vector<uint8_t> ev;
    for (uint32_t i = 0; i < 1100; ++i) {
        const uint32_t us = 400000u + (i % 7) * 20000u;
        ev.insert(ev.end(), {0x00, 0xFF, 0x51, 0x03, static_cast<uint8_t>(us >> 16), static_cast<uint8_t>(us >> 8),
                             static_cast<uint8_t>(us)});
        if (i < 70) ev.insert(ev.end(), {0x00, 0x90, static_cast<uint8_t>(30 + i), 0x64});
        ev.push_back(0x06);   // следующая смена через 6 тиков
        ev.insert(ev.end(), {0xFF, 0x01, 0x00});
    }
    ev.insert(ev.end(), {0x00, 0xFF, 0x2F, 0x00});
    const uint32_t len = static_cast<uint32_t>(ev.size());
    std::vector<uint8_t> file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k',
                                 static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                                 static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
    file.insert(file.end(), ev.begin(), ev.end());
    soundsinth::model::Song song;
    CHECK(load_smf(file, bnk, mem, song));
    const auto& st = formats::midi::last_load_stats();
    std::printf("  кражи %u, сверх карты темпа %u\n", unsigned(st.steals), unsigned(st.tempo_dropped));
    CHECK(st.steals >= 6u);
    CHECK_EQ(st.tempo_dropped, 76u);
    memory::track_memory_destroy(mem);
}

// Загрузка с текстом отказа: пустая строка - загрузилось.
std::string load_error(const std::vector<uint8_t>& file, const bank::Bank& bnk, memory::TrackMemory& mem) {
    memory::track_memory_reset_for_new_track(mem);
    formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
    soundsinth::model::Song song;
    const char* err = nullptr;
    const bool ok = formats::midi::load(src.as_byte_source(), static_cast<uint32_t>(file.size()), mem, bnk, song,
                                        &err, true);
    return ok ? std::string() : std::string(err ? err : "?");
}

// Заголовок с заданными форматом, числом дорожек и делением, дорожки - как есть.
std::vector<uint8_t> make_header(uint16_t format, uint16_t tracks, uint16_t division) {
    return {'M', 'T', 'h', 'd', 0, 0, 0, 6, static_cast<uint8_t>(format >> 8), static_cast<uint8_t>(format),
            static_cast<uint8_t>(tracks >> 8), static_cast<uint8_t>(tracks), static_cast<uint8_t>(division >> 8),
            static_cast<uint8_t>(division)};
}

// Ветки отказа и крайние случаи разбора, которых корпус не исполняет: отказ -
// с понятным текстом, а не молчание и не порча; крайний случай - загрузка.
void test_parse_failures_and_edges() {
    std::printf("test_parse_failures_and_edges\n");
    std::ifstream in(kBankPath, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК: банк не найден\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank bnk;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr));
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);

    const std::vector<uint8_t> note = {0x00, 0x90, 0x3C, 0x64, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};
    auto with_track = [](std::vector<uint8_t> f, const std::vector<uint8_t>& ev, uint32_t claimed_extra = 0) {
        const uint32_t len = static_cast<uint32_t>(ev.size()) + claimed_extra;
        f.insert(f.end(), {'M', 'T', 'r', 'k', static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                           static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)});
        f.insert(f.end(), ev.begin(), ev.end());
        return f;
    };
    struct Case {
        const char* name;
        std::vector<uint8_t> file;
        const char* expect;   // подстрока отказа, nullptr - загрузка
    };
    std::vector<uint8_t> short13 = make_header(0, 1, 96);
    short13.resize(13);
    std::vector<uint8_t> not_smf = with_track(make_header(0, 1, 96), note);
    not_smf[3] = 'x';
    std::vector<uint8_t> tracks65 = make_header(1, 65, 96);
    for (int i = 0; i < 65; ++i) tracks65 = with_track(tracks65, note);
    // Чужой чанк между дорожками: заголовок обещает две, вторая - "XFIH".
    // Отказ, как у OpenMPT: дальше по файлу дорожки не найти.
    std::vector<uint8_t> foreign = with_track(make_header(1, 2, 96), note);
    foreign.insert(foreign.end(), {'X', 'F', 'I', 'H', 0, 0, 0, 2, 0x11, 0x22});
    // Заявлены две дорожки, есть одна: играем то, что есть.
    std::vector<uint8_t> short_tracks = with_track(make_header(1, 2, 96), note);
    const std::vector<Case> cases = {
        {"13 байт", short13, "короче заголовка"},
        {"не MThd", not_smf, "не Standard MIDI File"},
        {"формат 2", with_track(make_header(2, 1, 96), note), "формата 2"},
        {"0 дорожек", make_header(1, 0, 96), "нет дорожек"},
        {"65 дорожек", tracks65, "слишком много"},
        {"только meta", with_track(make_header(0, 1, 96), {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x00, 0xFF, 0x2F, 0x00}),
         "нет событий"},
        {"одна нота после паузы", with_track(make_header(0, 1, 96), {0x64, 0x90, 0x3C, 0x64, 0x00, 0xFF, 0x2F, 0x00}),
         "после первой ноты"},
        {"чужой чанк", foreign, "чужой чанк"},
        {"дорожек меньше заявленных", short_tracks, nullptr},
        {"MTrk длиннее файла", with_track(make_header(0, 1, 96), note, 100), nullptr},
        {"SMPTE 25 x 40", with_track(make_header(0, 1, 0xE728), note), nullptr},
    };
    for (const Case& c : cases) {
        const std::string err = load_error(c.file, bnk, mem);
        const bool good = c.expect ? err.find(c.expect) != std::string::npos : err.empty();
        std::printf("  %-24s %s\n", c.name, err.empty() ? "загружен" : err.c_str());
        CHECK(good);
    }

    // Смена темпа на строке, где у всех 64 каналов колонка эффекта занята
    // задержкой ноты: откладывается на следующую строку и считается. Ноты на
    // тике 4 - внутри строки при любой сетке; срез паузы выключен, иначе они
    // уехали бы на тик 0.
    {
        formats::midi::set_trim_lead_silence(false);
        std::vector<uint8_t> ev = {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x04};
        for (uint8_t i = 0; i < 64; ++i) {
            if (i) ev.push_back(0x00);
            ev.insert(ev.end(), {0x90, static_cast<uint8_t>(30 + i), 0x64});
        }
        ev.insert(ev.end(), {0x00, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90, 0x60, 0xFF, 0x2F, 0x00});
        const std::string err = load_error(with_track(make_header(0, 1, 96), ev), bnk, mem);
        formats::midi::set_trim_lead_silence(true);
        const auto& st = formats::midi::last_load_stats();
        std::printf("  темп при занятых колонках: %s, отложено %u\n", err.empty() ? "загружен" : err.c_str(),
                    unsigned(st.tempo_deferred));
        CHECK(err.empty());
        CHECK(st.tempo_deferred >= 1u);
    }

    // Хранилище меньше зоны паттернов: отказ с текстом про PSRAM, зона
    // паттернов не выше границы хранилища.
    {
        memory::psram_set_track_bytes(mem.psram, 16u * 1024u);
        std::vector<uint8_t> ev;
        for (uint8_t i = 0; i < 120; ++i) ev.insert(ev.end(), {0x10, 0x90, static_cast<uint8_t>(30 + (i % 60)), 0x64});
        ev.insert(ev.end(), {0x00, 0xFF, 0x2F, 0x00});
        const std::string err = load_error(with_track(make_header(0, 1, 96), ev), bnk, mem);
        std::printf("  хранилище 16 КБ: %s, паттерны %u байт\n", err.c_str(), unsigned(mem.psram.pattern_bump_offset));
        CHECK(err.find("PSRAM") != std::string::npos);
        CHECK(mem.psram.pattern_bump_offset <= 16u * 1024u);
        memory::psram_set_track_bytes(mem.psram, memory::kPsramChipBytes);
    }
    memory::track_memory_destroy(mem);
}

} // namespace

void run_midi_loader_tests() {
    test_running_status_survives_meta_and_sysex();
    test_stolen_channel_keeps_new_note();
    test_doubled_note_released_everywhere();
    test_broken_timing_rejected_or_defaulted();
    test_all_notes_off_releases_every_layer();
    test_silent_notes_counted();
    test_zero_controller_silences();
    test_note_pan_on_note_row();
    test_bend_reaches_released_note();
    test_high_notes_reach_cells();
    test_modwheel_depth_scale();
    test_shared_envelope_copied_once();
    test_lead_trim_keeps_tempo_order();
    test_layout_losses_counted();
    test_parse_failures_and_edges();
}
