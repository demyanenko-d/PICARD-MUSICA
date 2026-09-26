// Сквозной путь живого MIDI на ПК: события .mid -> записи в порт A AY, как
// их делает ПЗУ 128 -> разбор последовательного потока -> очередь с форой ->
// живая песня. Проверяются два звена, которых нет в других тестах: поток на
// 31250 бод со своим временем и очередь, отдающая событие через фору.
//
// Эталон - та же живая песня, накормленная теми же событиями построчно (с
// загрузчиком .mid она сверена в test_midi_live). Ноты сверяются группами:
// строка файла против тиков, в которые она вышла. Внутри тика порядок
// каналов свой, поэтому группа сверяется как набор, а не по одному.
//
// Время: нота обязана выйти не раньше, чем через фору после прихода
// последнего бита своего сообщения, и не позже, чем через фору и тик.

#include "testing.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "ay_rom_writer.h"
#include "core/bank/bank_reader.h"
#include "core/config.h"
#include "core/model/song.h"
#include "core/formats/midi.h"
#include "core/formats/midi_live.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "core/live_midi/ay_midi.h"
#include "core/live_midi/live_stream.h"
#include "core/engine/tracker_engine.h"
#include "core/memory/sample_cache_catalog.h"
#include "core/live_midi/live_sysex.h"
#include "core/audio/sound_source.h"

using namespace soundsinth;

namespace {

constexpr const char* kBankPath = "release/banks/GeneralUser-GS.ssb";
constexpr uint32_t kChannels = formats::midi::kMaxChannels;
// Кадр 8N1 на 31250 бод - десять бит, 0.32 мс на байт.
constexpr double kSerialMsPerByte = 10.0 * 1000.0 / 31250.0;
constexpr uint32_t kLookaheadMs = SOUNDSINTH_LIVE_MIDI_LOOKAHEAD_MS;
// Живая сетка: строка = тик.
constexpr uint32_t kLiveTickMs = 2500u / SOUNDSINTH_LIVE_MIDI_TEMPO;
static_assert(kLiveTickMs * SOUNDSINTH_LIVE_MIDI_TEMPO == 2500u, "темп живого режима обязан давать целый тик");
// Дальше сорока секунд трека не идём: тик живой сетки мелкий, а файлов много.
constexpr double kSpanMs = 40000.0;

struct TraceEvent {
    uint8_t status, d1, d2, delay;
};

// Трасса загрузчика: события каждой строки.
struct Trace {
    std::vector<std::vector<TraceEvent>> events{1};
    uint32_t rows = 0;
    formats::midi::ConvertTrace hook;

    static void on_event(void* user, uint8_t status, uint8_t d1, uint8_t d2, uint8_t delay) {
        static_cast<Trace*>(user)->events.back().push_back(TraceEvent{status, d1, d2, delay});
    }
    static void on_row(void* user, const soundsinth::model::PatternCell*) {
        auto* t = static_cast<Trace*>(user);
        ++t->rows;
        t->events.emplace_back();
    }
};

// Запросы PCM и отдача записей: подтверждение сразу, как если бы загрузка на
// другом ядре уже выбросила PCM.
struct Requests {
    formats::midi::LiveMidi* live = nullptr;
    uint32_t count = 0;
    static void on_retire(void* user, uint16_t song_sample) {
        static_cast<Requests*>(user)->live->record_retired(song_sample);
    }
    static void on_request(void* user, uint16_t) { ++static_cast<Requests*>(user)->count; }
};

// Нота, вышедшая в строку: время, высота и инструмент банка (номера песни у
// двух путей свои).
struct Note {
    uint32_t at_ms;
    uint8_t note;
    uint16_t bank_inst;
};

bool same_note(const Note& a, const Note& b) { return a.note == b.note && a.bank_inst == b.bank_inst; }
bool note_less(const Note& a, const Note& b) {
    return a.note != b.note ? a.note < b.note : a.bank_inst < b.bank_inst;
}

void collect_notes(const formats::midi::LiveMidi& live, uint32_t at_ms, std::vector<Note>& out) {
    for (uint32_t c = 0; c < kChannels; ++c) {
        const soundsinth::model::PatternCell& cell = live.row()[c];
        if (!soundsinth::model::is_real_note(cell.note) || cell.instrument == 0) continue;
        out.push_back(Note{at_ms, cell.note, live.bank_instrument(static_cast<uint16_t>(cell.instrument - 1))});
    }
}

// Сообщение на линии: записи в порты AY и время прихода стоп-бита последнего
// байта. Линия одна, поэтому сообщения выстраиваются в очередь.
struct Wire {
    uint32_t done_ms;
    uint32_t row;
    std::vector<uint16_t> writes;
};

// Ноты одной строки файла и время прихода её сообщений.
struct Group {
    uint32_t at_ms = 0;      // время строки по сетке файла
    uint32_t first_ms = 0;   // приход самого раннего сообщения строки
    uint32_t last_ms = 0;    // приход самого позднего
    std::vector<Note> notes;
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// События трассы -> сообщения на линии со временем прихода. Линия одна:
// занятая отодвигает следующее сообщение, как на настоящем Спектруме.
uint32_t build_wires(const Trace& trace, uint32_t rows, double row_ms, double tick_ms, std::vector<Wire>& out,
                     std::vector<uint32_t>* row_first, std::vector<uint32_t>* row_last) {
    double line_free = 0.0, nominal = 0.0;
    uint32_t backlog = 0;
    for (uint32_t r = 0; r < rows; ++r) {
        for (const TraceEvent& e : trace.events[r]) {
            const double at = static_cast<double>(r) * row_ms + static_cast<double>(e.delay) * tick_ms;
            nominal = at > nominal ? at : nominal; // порядок событий - как у конвертера
            const uint8_t count = static_cast<uint8_t>(midi_in::midi_data_bytes(e.status) + 1u);
            const double start = nominal > line_free ? nominal : line_free;
            const double done = start + kSerialMsPerByte * static_cast<double>(count);
            line_free = done;
            pc_tests::AyRomWriter rom;
            if (count == 3) {
                rom.bytes({e.status, e.d1, e.d2});
            } else {
                rom.bytes({e.status, e.d1});
            }
            const uint32_t done_ms = static_cast<uint32_t>(done + 0.5);
            const uint32_t at_ms = static_cast<uint32_t>(nominal + 0.5);
            if (done_ms - at_ms > backlog) backlog = done_ms - at_ms;
            if (row_first != nullptr && done_ms < (*row_first)[r]) (*row_first)[r] = done_ms;
            if (row_last != nullptr && done_ms > (*row_last)[r]) (*row_last)[r] = done_ms;
            out.push_back(Wire{done_ms, r, std::move(rom.w)});
        }
    }
    return backlog;
}

// Буфер вывода: движок сводит в него, как на плате задача рендера.
void render(engine::TrackerEngine& e, std::vector<int32_t>& l, std::vector<int32_t>& r, uint32_t n) {
    l.assign(n, 0);
    r.assign(n, 0);
    mixbus::SoundSource* s = e.as_sound_source();
    s->render_add(s->self, l.data(), r.data(), n);
}

// Живая песня не должна упереться в потолки: за ними состав нот расходится
// законно, и сверять его нечем.
bool hit_caps(const formats::midi::LiveMidi& live) {
    return live.instruments_evicted() != 0 || live.instruments_failed() != 0 || live.samples_capped() != 0 ||
           live.stats().notes_over_cap != 0 || live.stats().steals != 0;
}

} // namespace

void test_live_chain_matches_direct_events() {
    std::printf("test_live_chain_matches_direct_events\n");
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
            // Путь идёт по тикам живой сетки - берём выборку, не весь набор.
            if (std::string(dir) == "music/src/midi" && (n++ % 25) != 0) continue;
            files.push_back(de.path().string());
        }
    }
    if (files.empty()) {
        std::printf("  ПРОПУСК: .mid нет\n");
        return;
    }
    std::sort(files.begin(), files.end());

    auto file_mem = std::make_unique<memory::TrackMemory>();
    auto ref_mem = std::make_unique<memory::TrackMemory>();
    auto live_mem = std::make_unique<memory::TrackMemory>();
    memory::track_memory_create(*file_mem);
    memory::track_memory_create(*ref_mem);
    memory::track_memory_create(*live_mem);
    auto file_song = std::make_unique<soundsinth::model::Song>();
    auto ref_song = std::make_unique<soundsinth::model::Song>();
    auto live_song = std::make_unique<soundsinth::model::Song>();
    auto ref = std::make_unique<formats::midi::LiveMidi>();
    auto live = std::make_unique<formats::midi::LiveMidi>();

    uint32_t checked = 0, skipped_effects = 0, skipped_caps = 0, bad_files = 0;
    uint32_t notes = 0, bad_groups = 0, missing = 0, extra = 0;
    uint32_t lost_events = 0, framing = 0, max_pending = 0, max_backlog = 0;
    uint32_t min_shift = 0xFFFFFFFFu, max_shift = 0, early = 0, late = 0;
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
        // Смена темпа по ходу трека сдвигает сетку файла: время события тогда
        // не посчитать по умолчаниям, а живому потоку темпа и не шлют.
        if (trace.hook.row_effects != 0) {
            ++skipped_effects;
            continue;
        }

        // Сетка файла: тик - 2500/темп мс, строка - speed тиков.
        const double tick_ms = 2500.0 / static_cast<double>(file_song->default_tempo);
        const double row_ms = tick_ms * static_cast<double>(file_song->default_speed);
        const uint32_t rows = std::min<uint32_t>(trace.rows, static_cast<uint32_t>(kSpanMs / row_ms) + 1);

        std::vector<Wire> wires;
        std::vector<uint32_t> row_first(rows, 0xFFFFFFFFu), row_last(rows, 0);
        const uint32_t backlog = build_wires(trace, rows, row_ms, tick_ms, wires, &row_first, &row_last);
        if (wires.empty()) continue;

        // Эталон: события построчно, сетка файла.
        memory::track_memory_reset_for_new_track(*ref_mem);
        Requests ref_req;
        ref_req.live = ref.get();
        const char* why = ref->begin(*ref_song, *ref_mem, bnk, file_song->default_speed,
                                     static_cast<uint8_t>(file_song->default_tempo), &Requests::on_request,
                                     &Requests::on_retire, &ref_req);
        CHECK(why == nullptr);
        if (why) return;
        for (uint8_t ch = 0; ch < 16; ++ch) ref->set_drum_channel(ch, ((trace.hook.drum_mask >> ch) & 1u) != 0);
        std::vector<Group> want;
        for (uint32_t r = 0; r < rows; ++r) {
            ref->begin_row();
            for (const TraceEvent& e : trace.events[r]) ref->event(e.status, e.d1, e.d2, e.delay);
            ref->finish_row();
            Group g;
            g.at_ms = static_cast<uint32_t>(static_cast<double>(r) * row_ms + 0.5);
            g.first_ms = row_first[r];
            g.last_ms = row_last[r];
            collect_notes(*ref, g.at_ms, g.notes);
            if (!g.notes.empty()) want.push_back(std::move(g));
        }

        // Живой путь: те же события через порт A и очередь с форой.
        memory::track_memory_reset_for_new_track(*live_mem);
        Requests live_req;
        live_req.live = live.get();
        why = live->begin(*live_song, *live_mem, bnk, /*ticks_per_row=*/1, SOUNDSINTH_LIVE_MIDI_TEMPO,
                          &Requests::on_request, &Requests::on_retire, &live_req);
        CHECK(why == nullptr);
        if (why) return;
        for (uint8_t ch = 0; ch < 16; ++ch) live->set_drum_channel(ch, ((trace.hook.drum_mask >> ch) & 1u) != 0);
        midi_in::LiveStream stream;
        stream.begin(live.get(), kLookaheadMs);
        midi_in::AyMidiInput input;
        std::vector<Note> got;
        size_t next = 0;
        const uint32_t end_ms = wires.back().done_ms + kLookaheadMs + 4u * kLiveTickMs;
        for (uint32_t t = 0; t <= end_ms; t += kLiveTickMs) {
            while (next < wires.size() && wires[next].done_ms <= t) {
                midi_in::MidiEvent ev{};
                for (uint16_t w : wires[next].writes) {
                    if (!input.feed(w, ev)) continue;
                    uint8_t status = 0, d1 = 0, d2 = 0;
                    midi_in::midi_event_bytes(ev, status, d1, d2);
                    stream.push(wires[next].done_ms, status, d1, d2);
                }
                ++next;
            }
            if (stream.pending() > max_pending) max_pending = stream.pending();
            stream.tick(t);
            collect_notes(*live, t, got);
        }
        lost_events += stream.lost();
        framing += input.framing_errors();

        if (hit_caps(*ref) || hit_caps(*live)) {
            ++skipped_caps;
            continue;
        }
        ++checked;
        if (backlog > max_backlog) max_backlog = backlog;

        // Группы строк против тиков. Если строки вышли в один тик, их ноты
        // перемешаны - такие группы сверяются вместе.
        uint32_t file_bad = 0, file_early = 0, file_late = 0;
        size_t gi = 0;
        for (size_t k = 0; k < want.size();) {
            std::vector<Note> mine;
            uint32_t first_ms = 0xFFFFFFFFu, last_ms = 0, at_ms = want[k].at_ms;
            size_t kk = k;
            for (;;) {
                mine.insert(mine.end(), want[kk].notes.begin(), want[kk].notes.end());
                first_ms = std::min(first_ms, want[kk].first_ms);
                last_ms = std::max(last_ms, want[kk].last_ms);
                const size_t end = gi + mine.size();
                // Граница пришлась на середину тика - забрать и следующую строку.
                if (kk + 1 < want.size() && end < got.size() && got[end - 1].at_ms == got[end].at_ms) {
                    ++kk;
                    continue;
                }
                break;
            }
            k = kk + 1;
            if (gi + mine.size() > got.size()) {
                missing += static_cast<uint32_t>(gi + mine.size() - got.size());
                ++file_bad;
                break;
            }
            std::vector<Note> theirs(got.begin() + gi, got.begin() + gi + mine.size());
            const uint32_t out_first = theirs.front().at_ms, out_last = theirs.back().at_ms;
            gi += mine.size();
            std::sort(mine.begin(), mine.end(), note_less);
            std::sort(theirs.begin(), theirs.end(), note_less);
            for (size_t i = 0; i < mine.size(); ++i) {
                if (!same_note(mine[i], theirs[i])) ++file_bad;
            }
            // Раньше форы нота выйти не может, позже форы с тиком - не должна.
            if (out_first < first_ms + kLookaheadMs) ++file_early;
            if (out_last > last_ms + kLookaheadMs + kLiveTickMs) ++file_late;
            const uint32_t shift = out_last - at_ms;
            if (shift < min_shift) min_shift = shift;
            if (shift > max_shift) max_shift = shift;
        }
        if (got.size() > gi) extra += static_cast<uint32_t>(got.size() - gi);
        notes += static_cast<uint32_t>(got.size());
        bad_groups += file_bad;
        early += file_early;
        late += file_late;
        if (file_bad || file_early || file_late || got.size() != gi) {
            ++bad_files;
            if (bad_files <= 5) {
                std::printf("  расхождение: %s нот %zu, не сошлось %u, раньше форы %u, позже %u, лишних %zu\n",
                            path.c_str(), got.size(), file_bad, file_early, file_late, got.size() - gi);
            }
        }
    }

    std::printf("  файлов сверено %u (нот %u), пропущено: темп/громкость %u, потолки %u; расходится файлов %u\n",
                checked, notes, skipped_effects, skipped_caps, bad_files);
    std::printf("  ноты: не сошлось %u, не вышло %u, лишних %u; раньше форы %u, позже форы с тиком %u\n", bad_groups,
                missing, extra, early, late);
    std::printf("  поток: событий потеряно %u, кадров битых %u, в очереди пик %u из %u; линия отстала на %u мс\n",
                lost_events, framing, max_pending, midi_in::kLiveStreamCapacity, max_backlog);
    if (min_shift != 0xFFFFFFFFu) {
        std::printf("  сдвиг строки: %u..%u мс при форе %u и тике %u\n", min_shift, max_shift, kLookaheadMs,
                    kLiveTickMs);
    }
    CHECK(checked > 0);
    CHECK_EQ(bad_groups, 0u);
    CHECK_EQ(missing, 0u);
    CHECK_EQ(extra, 0u);
    CHECK_EQ(early, 0u);
    CHECK_EQ(late, 0u);
    CHECK_EQ(lost_events, 0u);
    CHECK_EQ(framing, 0u);
    memory::track_memory_destroy(*file_mem);
    memory::track_memory_destroy(*ref_mem);
    memory::track_memory_destroy(*live_mem);
}

// Ударный канал объявляют SysEx по ходу игры: GS "Use For Rhythm Part" на
// третий канал. Нота после него обязана взять набор ударных, до него - свою
// программу. Проверяется весь путь: порт A, разбор SysEx, служебное событие
// в очереди, живая песня.
void test_live_chain_sysex_drum_channel() {
    std::printf("test_live_chain_sysex_drum_channel\n");
    std::vector<uint8_t> blob = read_file(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  ПРОПУСК: банка нет\n");
        return;
    }
    constexpr uint8_t kChannel = 2, kProgram = 0, kNote = 38, kVelocity = 100;

    // Один и тот же путь дважды: с объявлением канала ударным и без него.
    uint16_t inst[2] = {0xFFFF, 0xFFFF};
    for (uint32_t pass = 0; pass < 2; ++pass) {
        auto mem = std::make_unique<memory::TrackMemory>();
        memory::track_memory_create(*mem);
        memory::track_memory_reset_for_new_track(*mem);
        auto song = std::make_unique<soundsinth::model::Song>();
        auto live = std::make_unique<formats::midi::LiveMidi>();
        Requests req;
        req.live = live.get();
        CHECK(live->begin(*song, *mem, bnk, /*ticks_per_row=*/1, SOUNDSINTH_LIVE_MIDI_TEMPO, &Requests::on_request,
                          &Requests::on_retire, &req) == nullptr);
        midi_in::LiveStream stream;
        stream.begin(live.get(), kLookaheadMs);
        midi_in::SysexTracker sysex;
        sysex.begin();
        midi_in::AyMidiInput input;

        pc_tests::AyRomWriter rom;
        // Блок партии 3 - третий канал (партии 2..10 идут на первый..девятый).
        if (pass == 0) rom.bytes({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x13, 0x15, 0x02, 0x18, 0xF7});
        rom.bytes({static_cast<uint8_t>(0xc0 | kChannel), kProgram});
        rom.bytes({static_cast<uint8_t>(0x90 | kChannel), kNote, kVelocity});
        midi_in::MidiEvent ev{};
        for (uint16_t w : rom.w) {
            const bool message = input.feed(w, ev);
            if (input.sysex_ready()) sysex.apply(input.sysex(), input.sysex_len(), 0, stream);
            if (!message) continue;
            uint8_t status = 0, d1 = 0, d2 = 0;
            midi_in::midi_event_bytes(ev, status, d1, d2);
            stream.push(0, status, d1, d2);
        }
        CHECK_EQ(sysex.drum_changes(), pass == 0 ? 1u : 0u);
        CHECK(sysex.drum_channel(kChannel) == (pass == 0));

        for (uint32_t t = 0; t <= kLookaheadMs + 4u * kLiveTickMs; t += kLiveTickMs) {
            stream.tick(t);
            for (uint32_t c = 0; c < kChannels; ++c) {
                const soundsinth::model::PatternCell& cell = live->row()[c];
                if (cell.note != kNote || cell.instrument == 0) continue;
                inst[pass] = live->bank_instrument(static_cast<uint16_t>(cell.instrument - 1));
            }
        }
        CHECK(live->drum_channel(kChannel) == (pass == 0));
        memory::track_memory_destroy(*mem);
    }

    // Инструмент обязан совпасть с тем, что даёт банк: 128 - наборы ударных.
    uint16_t want[2] = {0xFFFF, 0xFFFF};
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const bank::BankPreset& preset = bank::bank_preset(bnk, pass == 0 ? 128u : 0u, kProgram);
        bank::NoteLayer layers[bank::kMaxNoteLayers];
        const uint32_t n = bank::select_note_layers(
            bnk, preset, kNote, kVelocity, [](uint16_t, uint16_t) { return true; }, layers);
        CHECK(n > 0);
        if (n > 0) want[pass] = bnk.layers[layers[0].layer].instrument;
    }
    std::printf("  инструмент: с SysEx %u (ждали %u), без %u (ждали %u)\n", inst[0], want[0], inst[1], want[1]);
    CHECK_EQ(inst[0], want[0]);
    CHECK_EQ(inst[1], want[1]);
    CHECK(inst[0] != inst[1]);
}

// Пачка смен программы в одной строке - так плеер начинает новую вещь.
// Подготовка обязана разложить постройки по строкам, а не делать их все разом:
// на плате девять построек в одной строке дали 4.2 мс при тике 10 мс. Ноты при
// этом не теряются - к своему тику инструмент готов.
void test_live_chain_spreads_instrument_builds() {
    std::printf("test_live_chain_spreads_instrument_builds\n");
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
    CHECK(live->begin(*song, *mem, bnk, /*ticks_per_row=*/1, SOUNDSINTH_LIVE_MIDI_TEMPO, &Requests::on_request,
                      &Requests::on_retire, &req) == nullptr);
    midi_in::LiveStream stream;
    stream.begin(live.get(), kLookaheadMs);

    // Восемь каналов, у каждого своя программа и нота - всё в один момент.
    constexpr uint32_t kChannelsUsed = 8;
    static const uint8_t kPrograms[kChannelsUsed] = {0, 19, 24, 33, 48, 56, 73, 90};
    for (uint8_t c = 0; c < kChannelsUsed; ++c) {
        CHECK(stream.push(0, static_cast<uint8_t>(0xc0 | c), kPrograms[c], 0));
        CHECK(stream.push(0, static_cast<uint8_t>(0x90 | c), static_cast<uint8_t>(60 + c), 100));
    }

    uint32_t worst_per_row = 0, notes = 0;
    uint32_t prev = live->instruments_built();
    for (uint32_t t = 0; t <= kLookaheadMs + 20u * kLiveTickMs; t += kLiveTickMs) {
        stream.tick(t);
        const uint32_t now = live->instruments_built();
        if (now - prev > worst_per_row) worst_per_row = now - prev;
        prev = now;
        for (uint32_t c = 0; c < kChannels; ++c) {
            const soundsinth::model::PatternCell& cell = live->row()[c];
            if (soundsinth::model::is_real_note(cell.note) && cell.instrument != 0) ++notes;
        }
    }
    std::printf("  инструментов %u, в худшей строке %u (предел %u), подготовка отложена %u раз; нот вышло %u\n",
                live->instruments_built(), worst_per_row, midi_in::kLiveBuildsPerRow, stream.deferred(), notes);
    // Предел на подготовку; на своём тике нота достраивает инструмент сама,
    // поэтому строка с нотами может выйти за него - но не на всю пачку.
    CHECK(worst_per_row < kChannelsUsed);
    CHECK(stream.deferred() > 0);
    CHECK(notes >= kChannelsUsed);
    memory::track_memory_destroy(*mem);
}

// Отложенная подготовка не имеет права отстать от чтения: сыгранные события
// готовить незачем, а их места в кольце писатель уже вправе занять заново.
// Случай - когда вся очередь становится играбельной разом, а подготовка за ту
// же строку успевает лишь пару построек.
void test_live_chain_prefetch_never_behind_read() {
    std::printf("test_live_chain_prefetch_never_behind_read\n");
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
    CHECK(live->begin(*song, *mem, bnk, /*ticks_per_row=*/1, SOUNDSINTH_LIVE_MIDI_TEMPO, &Requests::on_request,
                      &Requests::on_retire, &req) == nullptr);
    midi_in::LiveStream stream;
    stream.begin(live.get(), kLookaheadMs);

    uint32_t pushed = 0;
    for (uint8_t c = 0; c < 16; ++c) {
        CHECK(stream.push(0, static_cast<uint8_t>(0xc0 | c), static_cast<uint8_t>(c * 7), 0));
        CHECK(stream.push(0, static_cast<uint8_t>(0x90 | c), static_cast<uint8_t>(48 + c), 100));
        pushed += 2;
    }
    // Строка, в которой фора истекла сразу у всех: читатель забирает всю
    // очередь, а подготовка успевает лишь предел построек и остаётся позади.
    stream.tick(kLookaheadMs);
    const uint32_t behind = stream.prefetched();
    // Следующая строка обязана выровнять её по чтению, а не идти по сыгранным
    // местам кольца.
    stream.tick(kLookaheadMs + kLiveTickMs);
    std::printf("  событий %u, в очереди %u, подготовка отстала до %u, выровнена до %u, отложено %u раз\n", pushed,
                stream.pending(), behind, stream.prefetched(), stream.deferred());
    CHECK_EQ(stream.pending(), 0u);
    CHECK(behind < pushed); // отставание действительно было
    CHECK_EQ(stream.prefetched(), pushed);
    memory::track_memory_destroy(*mem);
}

// Стенд, повторяющий устройство платы. Отличие от прочих проверок живого
// пути одно, но решающее: подгрузка не отвечает на заказ сразу, а идёт
// отдельной чередой - как задача app_task, которую рендер не ждёт. Сэмпл
// появляется в памяти через время чтения из флеша (банк даёт 9.6 МБ/с),
// заказы обслуживаются по одному и не чаще опроса задачи. В этом зазоре на
// плате и рождаются молчащие ноты: движок считает их triggers_without_sample.
//
// Время течёт по отрендеренным кадрам, а не по часам машины: прогон
// повторяем. Строка живого режима видит время начала своего буфера -
// огрубление на 5.8 мс против форы в 100.
void test_live_chain_board_model() {
    std::printf("test_live_chain_board_model\n");
    std::vector<uint8_t> blob = read_file(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  ПРОПУСК: банка нет\n");
        return;
    }
    // Файлы с плотной сменой программ: на плате молчащие ноты видны именно на
    // таких. Трассу даёт файловый путь, а он тянет все инструменты трека
    // сразу - самые плотные файлы в арену не влезают, хотя живьём играют
    // (там инструменты строятся лениво, с вытеснением). Берём первый, который
    // и плотен, и читается.
    // Вещи идут подряд одним сеансом, как их гоняют на плате: инструменты и
    // записи сэмплов копятся через весь набор, доходят до потолков и
    // перерабатываются - именно в этом режиме плата и теряет ноты. Пауза
    // между вещами меньше таймаута выхода, иначе сеанс оборвётся.
    // Трассу даёт файловый путь, а он тянет все инструменты трека сразу -
    // самые плотные файлы в арену не влезают, хотя живьём играют; такие
    // пропускаем.
    auto file_mem = std::make_unique<memory::TrackMemory>();
    memory::track_memory_create(*file_mem);
    auto file_song = std::make_unique<soundsinth::model::Song>();
    std::vector<std::string> files;
    {
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator("SD/test2", ec)) {
            const std::string ext = de.path().extension().string();
            if (ext == ".mid" || ext == ".MID") files.push_back(de.path().string());
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) files.push_back("SD/test_music/midi/Dance.mid");
    }
    constexpr uint32_t kGapMs = 2000; // меньше таймаута выхода по тишине
    std::vector<Wire> wires;
    std::string played;
    uint32_t offset_ms = 0, taken = 0;
    for (const std::string& candidate : files) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) continue;
        const std::vector<uint8_t> file = read_file(candidate);
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
        if (!ok || trace.hook.row_effects != 0) continue; // смена темпа сетку сдвигает
        const double tick_ms = 2500.0 / static_cast<double>(file_song->default_tempo);
        const double row_ms = tick_ms * static_cast<double>(file_song->default_speed);
        std::vector<Wire> part;
        build_wires(trace, trace.rows, row_ms, tick_ms, part, nullptr, nullptr);
        if (part.empty()) continue;
        for (Wire& w : part) {
            w.done_ms += offset_ms;
            wires.push_back(std::move(w));
        }
        offset_ms = wires.back().done_ms + kGapMs;
        played += (taken ? ", " : "");
        played += std::filesystem::path(candidate).filename().string();
        ++taken;
    }
    memory::track_memory_destroy(*file_mem);
    if (wires.empty()) {
        std::printf("  ПРОПУСК: .mid нет\n");
        return;
    }
    const std::string path = played;

    auto mem = std::make_unique<memory::TrackMemory>();
    memory::track_memory_create(*mem);
    memory::track_memory_reset_for_new_track(*mem);
    auto song = std::make_unique<soundsinth::model::Song>();
    auto live = std::make_unique<formats::midi::LiveMidi>();
    midi_in::LiveStream stream;
    midi_in::AyMidiInput input;

    // Подгрузка как на плате: заказ в очередь, чтение по одному, сэмпл виден
    // лишь по времени готовности.
    struct Loader {
        const bank::Bank* bank = nullptr;
        memory::TrackMemory* mem = nullptr;
        soundsinth::model::Song* song = nullptr;
        formats::midi::LiveMidi* live = nullptr;
        const uint64_t* now_us = nullptr;
        struct Job {
            uint16_t index;
            uint64_t ready_us;
        };
        std::vector<Job> queue;
        std::vector<uint16_t> retire_pending;
        uint64_t busy_until_us = 0;
        uint32_t requests = 0, loaded = 0, repeats = 0, failed = 0, retired = 0;
        uint32_t queue_peak = 0;
        // Карта занятых сэмплов: её публикует движок раз в тик, как на плате.
        std::vector<uint32_t> in_use;

        static void on_request(void* user, uint16_t song_sample) {
            auto* l = static_cast<Loader*>(user);
            ++l->requests;
            if (memory::sample_cache_find(l->mem->sample_cache, song_sample) != nullptr) {
                ++l->repeats; // уже лежит - плата на это тоже времени не тратит
                return;
            }
            // Время чтения из флеша по размеру сэмпла: банк даёт 9.6 МБ/с.
            const uint16_t bs = static_cast<uint16_t>(l->song->samples[song_sample].file_offset);
            const uint32_t bytes = bs < l->bank->header->sample_count ? l->bank->samples[bs].pcm_packed_bytes : 0u;
            const uint64_t read_us = 1u + static_cast<uint64_t>(bytes) * 1000000u / (96u * 100000u);
            const uint64_t start = l->busy_until_us > *l->now_us ? l->busy_until_us : *l->now_us;
            l->busy_until_us = start + read_us;
            l->queue.push_back(Job{song_sample, l->busy_until_us});
            if (l->queue.size() > l->queue_peak) l->queue_peak = static_cast<uint32_t>(l->queue.size());
        }
        static void on_retire(void* user, uint16_t song_sample) {
            static_cast<Loader*>(user)->retire_pending.push_back(song_sample);
        }
        bool busy(uint16_t idx) const {
            return idx >= in_use.size() * 32u || (in_use[idx / 32u] & (1u << (idx % 32u))) != 0;
        }
        // Шаг задачи подгрузки: читает готовое и возвращает записи, которые
        // отпустили голоса, - тем же порядком, что live_session.
        void step(uint64_t now) {
            for (size_t i = 0; i < retire_pending.size();) {
                const uint16_t r = retire_pending[i];
                if (busy(r)) {
                    ++i;
                    continue;
                }
                if (auto* e = memory::sample_cache_find(mem->sample_cache, r)) {
                    memory::sample_cache_evict(mem->sample_cache, mem->psram, e);
                }
                live->record_retired(r);
                ++retired;
                retire_pending.erase(retire_pending.begin() + static_cast<long>(i));
            }
            size_t done = 0;
            while (done < queue.size() && queue[done].ready_us <= now) {
                if (formats::midi::load_sample_from_bank(*mem, *bank, *song, queue[done].index)) {
                    ++loaded;
                } else {
                    ++failed;
                }
                ++done;
            }
            if (done != 0) queue.erase(queue.begin(), queue.begin() + static_cast<long>(done));
        }
    };
    uint64_t now_us = 0;
    Loader loader;
    loader.bank = &bnk;
    loader.mem = mem.get();
    loader.song = song.get();
    loader.live = live.get();
    loader.now_us = &now_us;
    loader.in_use.assign(64, 0);

    CHECK(live->begin(*song, *mem, bnk, /*ticks_per_row=*/1, SOUNDSINTH_LIVE_MIDI_TEMPO, &Loader::on_request,
                      &Loader::on_retire, &loader) == nullptr);
    stream.begin(live.get(), kLookaheadMs);

    // Источник строк: приём записей AY и тик очереди - как live_row на плате.
    struct Rows {
        midi_in::LiveStream* stream = nullptr;
        midi_in::AyMidiInput* input = nullptr;
        const std::vector<Wire>* wires = nullptr;
        const uint64_t* now_us = nullptr;
        size_t next = 0;
        uint32_t lost = 0;
        static const soundsinth::model::PatternCell* row(void* user) {
            auto* c = static_cast<Rows*>(user);
            const uint32_t now_ms = static_cast<uint32_t>(*c->now_us / 1000u);
            while (c->next < c->wires->size() && (*c->wires)[c->next].done_ms <= now_ms) {
                midi_in::MidiEvent ev{};
                for (uint16_t w : (*c->wires)[c->next].writes) {
                    if (!c->input->feed(w, ev)) continue;
                    uint8_t status = 0, d1 = 0, d2 = 0;
                    midi_in::midi_event_bytes(ev, status, d1, d2);
                    if (!c->stream->push(now_ms, status, d1, d2)) ++c->lost;
                }
                ++c->next;
            }
            return c->stream->tick(now_ms);
        }
    };
    Rows rows_src;
    rows_src.stream = &stream;
    rows_src.input = &input;
    rows_src.wires = &wires;
    rows_src.now_us = &now_us;

    engine::TrackerEngine e(*song, *mem);
    e.set_live_row_source(&Rows::row, &rows_src);
    e.set_voice_cull_enabled(true);
    struct Observer {
        Loader* loader = nullptr;
        static void on_tick(void* user, uint16_t, const uint16_t* indices, uint8_t count) {
            auto* l = static_cast<Observer*>(user)->loader;
            std::fill(l->in_use.begin(), l->in_use.end(), 0u);
            for (uint8_t i = 0; i < count; ++i) {
                const uint16_t idx = indices[i];
                if (idx < l->in_use.size() * 32u) l->in_use[idx / 32u] |= 1u << (idx % 32u);
            }
        }
    };
    Observer obs;
    obs.loader = &loader;
    e.set_tick_observer(&Observer::on_tick, &obs);

    // Прогон: буфер за буфером, между ними - шаг задачи подгрузки не чаще её
    // опроса на плате.
    constexpr uint32_t kFrames = SOUNDSINTH_AUDIO_BUFFER_FRAMES;
    constexpr uint64_t kBufferUs = kFrames * 1000000ull / engine::kSampleRateHz;
    constexpr uint64_t kPollUs = 5000; // app_task в живом режиме
    const uint64_t end_us = (static_cast<uint64_t>(wires.back().done_ms) + kLookaheadMs + 2000u) * 1000ull;
    std::vector<int32_t> l, r;
    uint64_t last_poll = 0;
    uint32_t buffers = 0;
    while (now_us < end_us) {
        if (now_us - last_poll >= kPollUs) {
            loader.step(now_us);
            last_poll = now_us;
        }
        render(e, l, r, kFrames);
        now_us += kBufferUs;
        ++buffers;
    }

    std::printf("  %s: буферов %u, заказов %u (повторов %u), прочитано %u, отказов %u, очередь чтения пик %u\n",
                path.c_str(), buffers, loader.requests, loader.repeats, loader.loaded, loader.failed,
                loader.queue_peak);
    std::printf("  инструментов %u (вытеснено %u), записей отдано %u; мимо упреждения - инструментов %u, записей %u; "
                "отдано свежих %u\n",
                live->instruments_built(), live->instruments_evicted(), loader.retired, live->instruments_late(),
                live->records_late(), live->records_retired_recent());
    std::printf("  МОЛЧАЩИХ НОТ %u; событий потеряно %u/%u\n", e.triggers_without_sample(), rows_src.lost,
                stream.lost());
    CHECK_EQ(loader.failed, 0u);
    CHECK_EQ(stream.lost(), 0u);
    CHECK(e.triggers_without_sample() == 0u);
    memory::track_memory_destroy(*mem);
}

void run_live_chain_tests() {
    test_live_chain_board_model();
    test_live_chain_spreads_instrument_builds();
    test_live_chain_prefetch_never_behind_read();
    test_live_chain_matches_direct_events();
    test_live_chain_sysex_drum_channel();
}
