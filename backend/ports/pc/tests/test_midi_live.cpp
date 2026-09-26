// Живой вход MIDI (formats/midi_live) против загрузчика .mid: те же события
// тем же конвертером обязаны давать те же строки ячеек и те же инструменты.
// Загрузчик отдаёт трассу (set_convert_trace): события каждой строки и
// готовые ячейки; живой путь получает те же события построчно.
//
// Файлы со сменой темпа или общей громкости пропускаются: их выписывает
// загрузчик, у живого потока их нет. Файлы сверх потолков живой песни
// (инструменты, записи сэмплов, арена) - тоже, со счётом.

#include "testing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/bank/bank_reader.h"
#include "core/model/song.h"
#include "core/formats/midi.h"
#include "core/formats/midi_live.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"

using namespace soundsinth;

namespace {

constexpr const char* kBankPath = "release/banks/GeneralUser-GS.ssb";
constexpr uint32_t kChannels = formats::midi::kMaxChannels;

struct TraceEvent {
    uint8_t status, d1, d2, delay;
};

// Трасса загрузчика: события и ячейки каждой строки.
struct Trace {
    std::vector<std::vector<TraceEvent>> events{1};
    std::vector<soundsinth::model::PatternCell> cells;
    formats::midi::ConvertTrace hook;

    static void on_event(void* user, uint8_t status, uint8_t d1, uint8_t d2, uint8_t delay) {
        static_cast<Trace*>(user)->events.back().push_back(TraceEvent{status, d1, d2, delay});
    }
    static void on_row(void* user, const soundsinth::model::PatternCell* row) {
        auto* t = static_cast<Trace*>(user);
        t->cells.insert(t->cells.end(), row, row + kChannels);
        t->events.emplace_back();
    }
};

bool same_cell(const soundsinth::model::PatternCell& a, const soundsinth::model::PatternCell& b) {
    return a.note == b.note && a.instrument == b.instrument && a.volume.type == b.volume.type &&
           a.volume.param == b.volume.param && a.effect.type == b.effect.type && a.effect.param == b.effect.param &&
           a.effect.rate == b.effect.rate;
}

// По полям: байты выравнивания у двух арен разные.
bool same_envelope(const soundsinth::model::Envelope* a, const soundsinth::model::Envelope* b) {
    if (!a || !b) return a == b;
    if (a->enabled != b->enabled || a->sustain_enabled != b->sustain_enabled || a->loop_enabled != b->loop_enabled ||
        a->carry != b->carry || a->point_count != b->point_count || a->sustain_point != b->sustain_point ||
        a->sustain_end != b->sustain_end || a->loop_start != b->loop_start || a->loop_end != b->loop_end) {
        return false;
    }
    for (uint8_t k = 0; k < a->point_count; ++k) {
        if (a->points[k].tick != b->points[k].tick || a->points[k].value != b->points[k].value) return false;
    }
    return true;
}

// Инструменты равны по полям; сэмпл keymap - по номеру сэмпла банка
// (file_offset): номера записей песни у путей разные, у живого keymap полный.
// lazy - у живого зона без записи допустима: записи заводятся по первой ноте,
// у файла - на все сыгранные за трек.
bool same_instrument(const soundsinth::model::Song& fs, uint16_t fi, const soundsinth::model::Song& ls, uint16_t li,
                     bool lazy = false) {
    const auto& a = fs.instruments[fi];
    const auto& b = ls.instruments[li];
    if (a.fadeout_rate != b.fadeout_rate || a.global_volume != b.global_volume ||
        a.velocity_to_cutoff != b.velocity_to_cutoff || a.filter_cutoff != b.filter_cutoff ||
        a.filter_resonance != b.filter_resonance || a.nna != b.nna || a.dct != b.dct || a.dca != b.dca ||
        a.instrument_panning != b.instrument_panning || a.note_to_sample_range_count != b.note_to_sample_range_count) {
        return false;
    }
    if (!same_envelope(a.volume_envelope, b.volume_envelope) || !same_envelope(a.filter_envelope, b.filter_envelope)) {
        return false;
    }
    for (uint8_t k = 0; k < a.note_to_sample_range_count; ++k) {
        const auto& ra = a.note_to_sample_ranges[k];
        const auto& rb = b.note_to_sample_ranges[k];
        if (ra.start_note != rb.start_note || ra.note_offset != rb.note_offset) return false;
        if (ra.sample_index == soundsinth::model::kNoSample) continue;
        if (rb.sample_index == soundsinth::model::kNoSample) {
            if (lazy) continue;
            return false;
        }
        if (fs.samples[ra.sample_index].file_offset != ls.samples[rb.sample_index].file_offset) return false;
    }
    return true;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Запросы PCM и отдача записей; подтверждение отдачи - сразу, как если бы
// загрузчик на другом ядре уже выбросил PCM.
struct Requests {
    formats::midi::LiveMidi* live = nullptr;
    uint32_t count = 0;
    uint16_t max_index = 0;
    static void on_retire(void* user, uint16_t song_sample) {
        static_cast<Requests*>(user)->live->record_retired(song_sample);
    }
    static void on_request(void* user, uint16_t song_sample) {
        auto* r = static_cast<Requests*>(user);
        ++r->count;
        if (song_sample > r->max_index) r->max_index = song_sample;
    }
};

} // namespace

void test_midi_live_matches_file_loader() {
    std::printf("test_midi_live_matches_file_loader\n");
    std::vector<uint8_t> blob = read_file(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  ПРОПУСК: банка нет\n");
        return;
    }
    std::vector<std::string> files;
    for (const char* dir : {"SD/test_music/midi", "music/src/midi"}) {
        std::error_code ec;
        uint32_t n = 0;
        for (const auto& de : std::filesystem::recursive_directory_iterator(dir, ec)) {
            const std::string ext = de.path().extension().string();
            if (ext != ".mid" && ext != ".MID") continue;
            // Из большого набора - каждый пятый: тест идёт в каждой проверке.
            if (std::string(dir) == "music/src/midi" && (n++ % 5) != 0) continue;
            files.push_back(de.path().string());
        }
    }
    if (files.empty()) {
        std::printf("  ПРОПУСК: .mid нет\n");
        return;
    }
    std::sort(files.begin(), files.end());

    auto file_mem = std::make_unique<memory::TrackMemory>();
    auto live_mem = std::make_unique<memory::TrackMemory>();
    memory::track_memory_create(*file_mem);
    memory::track_memory_create(*live_mem);
    auto file_song = std::make_unique<soundsinth::model::Song>();
    auto live_song = std::make_unique<soundsinth::model::Song>();
    auto live = std::make_unique<formats::midi::LiveMidi>();

    uint32_t checked = 0, skipped_effects = 0, skipped_caps = 0, bad_files = 0, rows = 0, bad_rows = 0,
             bad_instruments = 0, requests = 0;
    uint32_t caps_arena = 0, caps_samples = 0, caps_instruments = 0, max_arena = 0, max_samples = 0,
             max_instruments = 0, evicted = 0, with_eviction = 0;
    for (const std::string& path : files) {
        const std::vector<uint8_t> file = read_file(path);
        Trace trace;
        trace.hook.event = &Trace::on_event;
        trace.hook.row_done = &Trace::on_row;
        trace.hook.user = &trace;
        memory::track_memory_reset_for_new_track(*file_mem);
        formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
        formats::midi::set_convert_trace(&trace.hook);
        const bool ok = formats::midi::load(src.as_byte_source(), static_cast<uint32_t>(file.size()), *file_mem, bnk,
                                            *file_song, nullptr, true);
        formats::midi::set_convert_trace(nullptr);
        if (!ok) continue;
        if (trace.hook.row_effects != 0) {
            ++skipped_effects;
            continue;
        }

        memory::track_memory_reset_for_new_track(*live_mem);
        Requests req;
        req.live = live.get();
        const char* why =
            live->begin(*live_song, *live_mem, bnk, file_song->default_speed,
                        static_cast<uint8_t>(file_song->default_tempo), &Requests::on_request, &Requests::on_retire, &req);
        CHECK(why == nullptr);
        if (why) return;
        for (uint8_t ch = 0; ch < 16; ++ch) live->set_drum_channel(ch, (trace.hook.drum_mask >> ch) & 1u);
        const uint32_t row_count = static_cast<uint32_t>(trace.cells.size() / kChannels);
        uint32_t file_bad_rows = 0;
        for (uint32_t r = 0; r < row_count; ++r) {
            live->begin_row();
            for (const TraceEvent& e : trace.events[r]) live->event(e.status, e.d1, e.d2, e.delay);
            live->finish_row();
            const soundsinth::model::PatternCell* want = &trace.cells[static_cast<size_t>(r) * kChannels];
            for (uint32_t c = 0; c < kChannels; ++c) {
                const soundsinth::model::PatternCell& got = live->row()[c];
                if (same_cell(got, want[c])) continue;
                // После вытеснения номера у путей разные: тот же инструмент
                // по содержимому - та же ячейка.
                soundsinth::model::PatternCell renumbered = got;
                renumbered.instrument = want[c].instrument;
                if (got.instrument != 0 && want[c].instrument != 0 && same_cell(renumbered, want[c]) &&
                    same_instrument(*file_song, static_cast<uint16_t>(want[c].instrument - 1), *live_song,
                                    static_cast<uint16_t>(got.instrument - 1), true)) {
                    continue;
                }
                ++file_bad_rows;
                break;
            }
        }
        // Сверх потолков живой песни ячейки расходятся законно: нота без
        // инструмента не пишется.
        const uint32_t arena = static_cast<uint32_t>(memory::arena_used(live_mem->resident));
        if (arena > max_arena) max_arena = arena;
        if (live_song->sample_count > max_samples) max_samples = live_song->sample_count;
        if (live_song->instrument_count > max_instruments) max_instruments = live_song->instrument_count;
        if (live->stats().notes_over_cap != formats::midi::last_load_stats().notes_over_cap ||
            live->instruments_failed() != 0 || live->samples_capped() != 0) {
            if (live->instruments_failed() != 0) ++caps_arena;
            if (live->samples_capped() != 0) ++caps_samples;
            if (live->stats().notes_over_cap != 0) ++caps_instruments;
            ++skipped_caps;
            continue;
        }
        ++checked;
        rows += row_count;
        bad_rows += file_bad_rows;
        evicted += live->instruments_evicted();
        if (live->instruments_evicted() != 0) ++with_eviction;
        // Без вытеснения номера у путей одни - сверка всех инструментов.
        uint32_t file_bad_instruments = 0;
        if (live->instruments_evicted() == 0) {
            CHECK_EQ(live_song->instrument_count, file_song->instrument_count);
            for (uint16_t i = 0; i < file_song->instrument_count && i < live_song->instrument_count; ++i) {
                if (!same_instrument(*file_song, i, *live_song, i)) ++file_bad_instruments;
            }
        }
        bad_instruments += file_bad_instruments;
        requests += req.count;
        CHECK(req.max_index < live_song->sample_count || req.count == 0);
        if (file_bad_rows || file_bad_instruments) {
            ++bad_files;
            if (bad_files <= 5) {
                std::printf("  расхождение: %s строк %u, инструментов %u\n", path.c_str(), file_bad_rows,
                            file_bad_instruments);
            }
        }
    }
    std::printf("  файлов сверено %u (строк %u), пропущено: темп/громкость %u, потолки %u; "
                "расходится строк %u, инструментов %u; запросов PCM %u\n",
                checked, rows, skipped_effects, skipped_caps, bad_rows, bad_instruments, requests);
    std::printf("  потолки: арена %u, записи сэмплов %u, ноты без инструмента %u; пик арены %u Б, записей %u, "
                "инструментов %u; с вытеснением файлов %u, вытеснено %u\n",
                caps_arena, caps_samples, caps_instruments, max_arena, max_samples, max_instruments, with_eviction,
                evicted);
    CHECK(checked > 0);
    CHECK_EQ(bad_rows, 0u);
    CHECK_EQ(bad_instruments, 0u);
    memory::track_memory_destroy(*file_mem);
    memory::track_memory_destroy(*live_mem);
}

// Долгая живая сессия: программы (банк 0 и вариации GS) и ноты ударных по
// кругу, три прохода - инструментов втрое больше номеров. Ни одна нота не
// теряется, у каждой - инструмент своего пресета, пулы не текут.
void test_midi_live_eviction_long_session() {
    std::printf("test_midi_live_eviction_long_session\n");
    std::vector<uint8_t> blob = read_file(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  ПРОПУСК: банка нет\n");
        return;
    }
    auto mem = std::make_unique<memory::TrackMemory>();
    memory::track_memory_create(*mem);
    memory::track_memory_reset_for_new_track(*mem);
    auto song = std::make_unique<soundsinth::model::Song>();
    auto live = std::make_unique<formats::midi::LiveMidi>();
    Requests req;
    req.live = live.get();
    CHECK(live->begin(*song, *mem, bnk, 6, 125, &Requests::on_request, &Requests::on_retire, &req) == nullptr);

    struct Hit {
        uint8_t ch, bank_no, program, note;
    };
    std::vector<Hit> hits;
    for (uint32_t pass = 0; pass < 3; ++pass) {
        for (uint8_t p = 0; p < 128; ++p) hits.push_back(Hit{0, 0, p, 60});
        for (uint8_t p = 0; p < 128; ++p) hits.push_back(Hit{1, 8, p, 48});
        for (uint8_t n = 35; n <= 81; ++n) hits.push_back(Hit{9, 0, 0, n});
    }
    // Нота на строке, снятие строкой позже, следующая - через 40 строк: релиз
    // прошлой успевает отзвучать.
    constexpr uint32_t kGap = 40;
    uint32_t notes = 0, lost = 0, wrong = 0;
    for (const Hit& h : hits) {
        live->begin_row();
        if (h.ch != 9) {
            live->event(static_cast<uint8_t>(0xb0 | h.ch), 0, h.bank_no, 0);
            live->event(static_cast<uint8_t>(0xc0 | h.ch), h.program, 0, 0);
        }
        live->event(static_cast<uint8_t>(0x90 | h.ch), h.note, 100, 0);
        live->finish_row();
        ++notes;
        // Инструменты пресета этой ноты - по банку, как выбирает конвертер.
        const bank::BankPreset& preset = bank::bank_preset(bnk, h.ch == 9 ? 128 : h.bank_no, h.program);
        bank::NoteLayer layers[bank::kMaxNoteLayers];
        const uint32_t n = bank::select_note_layers(
            bnk, preset, h.note, 100, [](uint16_t, uint16_t) { return true; }, layers);
        uint32_t found = 0;
        for (uint32_t c = 0; c < kChannels; ++c) {
            const soundsinth::model::PatternCell& cell = live->row()[c];
            if (cell.note != h.note || cell.instrument == 0) continue;
            const uint16_t bi = live->bank_instrument(static_cast<uint16_t>(cell.instrument - 1));
            bool ok = false;
            for (uint32_t k = 0; k < n; ++k) ok = ok || bnk.layers[layers[k].layer].instrument == bi;
            if (!ok) ++wrong;
            ++found;
        }
        if (n != 0 && found == 0) ++lost;
        live->begin_row();
        live->event(static_cast<uint8_t>(0x80 | h.ch), h.note, 0, 0);
        live->finish_row();
        for (uint32_t r = 0; r < kGap; ++r) {
            live->begin_row();
            live->finish_row();
        }
    }
    std::printf("  нот %u, потеряно %u, чужой инструмент %u; вытеснено %u, инструментов %u, не встало %u; "
                "записей отдано %u, без записи %u\n",
                notes, lost, wrong, live->instruments_evicted(), song->instrument_count, live->instruments_failed(),
                live->records_retired(), live->samples_capped());
    // Работа, которая растёт с числом живых инструментов и делается в тике
    // рендера: на плате она и разгоняет худший тик.
    std::printf("  проходы: keymap %u, отдача записей %u раз (keymap %u, записей %u); на ноту keymap %u\n",
                live->keymap_visits(), live->retire_calls(), live->retire_keymap_visits(),
                live->retire_record_visits(), notes ? live->keymap_visits() / notes : 0u);
    CHECK_EQ(live->samples_capped(), 0u);
    CHECK_EQ(lost, 0u);
    CHECK_EQ(wrong, 0u);
    CHECK(live->instruments_evicted() > 0);
    CHECK_EQ(live->instruments_failed(), 0u);
    CHECK(song->instrument_count <= formats::midi::kLiveMaxInstruments);
    memory::track_memory_destroy(*mem);
}

// Упреждение: подготовка ноты заранее заказывает PCM, а ряд ячеек и
// инструменты выходят те же, что и без неё.
void test_midi_live_prefetch_matches() {
    std::printf("test_midi_live_prefetch_matches\n");
    std::vector<uint8_t> blob = read_file(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  ПРОПУСК: банка нет\n");
        return;
    }
    struct Side {
        std::unique_ptr<memory::TrackMemory> mem = std::make_unique<memory::TrackMemory>();
        std::unique_ptr<soundsinth::model::Song> song = std::make_unique<soundsinth::model::Song>();
        std::unique_ptr<formats::midi::LiveMidi> live = std::make_unique<formats::midi::LiveMidi>();
        Requests req;
    };
    Side with, without;
    for (Side* s : {&with, &without}) {
        memory::track_memory_create(*s->mem);
        memory::track_memory_reset_for_new_track(*s->mem);
        s->req.live = s->live.get();
        CHECK(s->live->begin(*s->song, *s->mem, bnk, 6, 125, &Requests::on_request, &Requests::on_retire, &s->req) ==
              nullptr);
    }

    struct Hit {
        uint8_t ch, program, note, velocity;
    };
    const Hit hits[] = {{0, 0, 60, 100},  {0, 0, 64, 100}, {0, 48, 55, 90}, {1, 30, 40, 127},
                        {9, 0, 38, 110},  {9, 0, 42, 64},  {2, 73, 72, 80}, {0, 0, 60, 30}};

    uint32_t requested_before_note = 0, cells_differ = 0;
    for (const Hit& h : hits) {
        // Упреждение: программа берётся та, что будет к ноте.
        const uint8_t bank_no = h.ch == 9 ? 128 : 0;
        const uint32_t before = with.req.count;
        with.live->prefetch_note(bank_no, h.program, h.note, h.velocity);
        if (with.req.count > before) ++requested_before_note;

        for (Side* s : {&with, &without}) {
            s->live->begin_row();
            if (h.ch != 9) s->live->event(static_cast<uint8_t>(0xc0 | h.ch), h.program, 0, 0);
            s->live->event(static_cast<uint8_t>(0x90 | h.ch), h.note, h.velocity, 0);
            s->live->finish_row();
        }
        for (uint32_t c = 0; c < kChannels; ++c) {
            if (!same_cell(with.live->row()[c], without.live->row()[c])) ++cells_differ;
        }
        for (Side* s : {&with, &without}) {
            s->live->begin_row();
            s->live->event(static_cast<uint8_t>(0x80 | h.ch), h.note, 0, 0);
            s->live->finish_row();
        }
    }

    std::printf("  нот %u, с упреждающим заказом PCM %u; расхождений ячеек %u; инструментов %u против %u\n",
                (unsigned)(sizeof(hits) / sizeof(hits[0])), requested_before_note, cells_differ,
                with.song->instrument_count, without.song->instrument_count);
    // Каждая новая нота обязана заказать PCM заранее; повтор той же - нет.
    CHECK(requested_before_note >= 6);
    CHECK_EQ(cells_differ, 0u);
    CHECK_EQ(with.song->instrument_count, without.song->instrument_count);
    for (uint16_t i = 0; i < with.song->instrument_count; ++i) {
        CHECK(same_instrument(*without.song, i, *with.song, i, /*lazy=*/false));
        CHECK_EQ(with.live->bank_instrument(i), without.live->bank_instrument(i));
    }
    for (Side* s : {&with, &without}) memory::track_memory_destroy(*s->mem);
}

void run_midi_live_tests() {
    test_midi_live_matches_file_loader();
    test_midi_live_eviction_long_session();
    test_midi_live_prefetch_matches();
}
