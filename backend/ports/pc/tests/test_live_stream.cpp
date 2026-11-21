// SPDX-License-Identifier: MIT
// Поток живого MIDI (midi_in/live_stream): событие ждёт свою фору, порядок
// не меняется, нота подготовлена до того, как зазвучит, а переполненное
// кольцо теряет и считает.

#include "testing.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "ay_rom_writer.h"

#include "core/bank/bank_reader.h"
#include "core/model/song.h"
#include "core/formats/midi_live.h"
#include "player/live/live_requests.h"
#include "player/live/live_tap.h"
#include "player/live/session.h"
#include "player/shared_state.h"
#include "core/memory/track_memory.h"
#include "core/engine/tracker_engine.h"
#include "core/formats/midi.h"
#include "core/live_midi/live_stream.h"
#include "core/audio/sound_source.h"

extern char g_test_log[8192];
extern uint32_t g_test_time_us;

using namespace soundsinth;

namespace {

constexpr const char* kBankPath = "release/banks/GeneralUser-GS.ssb";
constexpr uint32_t kLookaheadMs = 100;

std::vector<uint8_t> read_bank(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Requests {
    formats::midi::LiveMidi* live = nullptr;
    uint32_t count                = 0;
    static void on_retire(void* user, uint16_t song_sample) { static_cast<Requests*>(user)->live->record_retired(song_sample); }
    static void on_request(void* user, uint16_t) { ++static_cast<Requests*>(user)->count; }
};

// Живая песня поверх памяти трека: банк, память, песня и поток.
struct Session {
    std::unique_ptr<memory::TrackMemory> mem      = std::make_unique<memory::TrackMemory>();
    std::unique_ptr<soundsinth::model::Song> song = std::make_unique<soundsinth::model::Song>();
    std::unique_ptr<formats::midi::LiveMidi> live = std::make_unique<formats::midi::LiveMidi>();
    std::unique_ptr<midi_in::LiveStream> stream   = std::make_unique<midi_in::LiveStream>();
    Requests req;

    bool begin(const bank::Bank& bnk) {
        memory::track_memory_create(*mem);
        memory::track_memory_reset_for_new_track(*mem);
        req.live = live.get();
        if (live->begin(*song, *mem, bnk, 1, 250, &Requests::on_request, &Requests::on_retire, &req) != nullptr) {
            return false;
        }
        stream->begin(live.get(), kLookaheadMs);
        return true;
    }
    ~Session() { memory::track_memory_destroy(*mem); }
};

// Есть ли в строке нота note.
bool row_has_note(const soundsinth::model::PatternCell* row, uint8_t note) {
    for (uint32_t c = 0; c < formats::midi::kMaxChannels; ++c) {
        if (row[c].note == note && row[c].instrument != 0) return true;
    }
    return false;
}

// Рендер n кадров кусками, как у плеера.
void render(engine::TrackerEngine& e, std::vector<int32_t>& l, std::vector<int32_t>& r, uint32_t n) {
    l.assign(n, 0);
    r.assign(n, 0);
    mixbus::SoundSource* s    = e.as_sound_source();
    constexpr uint32_t kChunk = 1000;
    for (uint32_t d = 0; d < n; d += kChunk) {
        const uint32_t k = (n - d) < kChunk ? (n - d) : kChunk;
        s->render_add(s->self, l.data() + d, r.data() + d, k);
    }
}

} // namespace

void test_live_stream_lookahead() {
    std::printf("test_live_stream_lookahead\n");
    std::vector<uint8_t> blob = read_bank(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  SKIP: no bank\n");
        return;
    }
    Session s;
    CHECK(s.begin(bnk));

    // Событие пришло на 1000 мс; играть его тику после 1100.
    CHECK(s.stream->push(1000, 0xc0, 48, 0));   // смена программы
    CHECK(s.stream->push(1000, 0x90, 60, 100)); // нота

    // Тик сразу после прихода: играть нечего, но сэмплы уже заказаны.
    const uint32_t requests_before            = s.req.count;
    const soundsinth::model::PatternCell* row = s.stream->tick(1010);
    CHECK(!row_has_note(row, 60));
    CHECK(s.req.count > requests_before); // упреждение сработало
    const uint32_t requests_after_prefetch = s.req.count;
    CHECK_EQ(s.stream->pending(), 2u);

    // Фора не истекла - по-прежнему тишина.
    row = s.stream->tick(1099);
    CHECK(!row_has_note(row, 60));
    CHECK_EQ(s.stream->pending(), 2u);

    // Фора истекла - нота на этом тике. Заказ PCM на ноте повторяется
    // намеренно (повтор отсекает очередь загрузчика), важно, что он был и
    // до неё.
    row = s.stream->tick(1100);
    CHECK(row_has_note(row, 60));
    CHECK_EQ(s.stream->pending(), 0u);
    CHECK(s.req.count >= requests_after_prefetch);

    // Следующий тик - строка без ноты (событие одно).
    row = s.stream->tick(1110);
    CHECK(!row_has_note(row, 60));

    // Тишина считается от сыгранного события, а не от прихода.
    CHECK_EQ(s.stream->silent_ms(1110), 10u);
    // Спросили временем старше отметки (у спрашивающего свои часы, и рендер
    // успел сыграть событие позже): это не четыре миллиарда, а ноль.
    CHECK_EQ(s.stream->silent_ms(1090), 0u);
    CHECK_EQ(s.stream->lost(), 0u);
}

void test_live_stream_order_and_overflow() {
    std::printf("test_live_stream_order_and_overflow\n");
    std::vector<uint8_t> blob = read_bank(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  SKIP: no bank\n");
        return;
    }
    Session s;
    CHECK(s.begin(bnk));

    // Кольцо полно - события теряются и считаются.
    for (uint32_t i = 0; i < midi_in::kLiveStreamCapacity; ++i) {
        CHECK(s.stream->push(0, 0x90, static_cast<uint8_t>(36 + (i % 40)), 100));
    }
    CHECK(!s.stream->push(0, 0x90, 60, 100));
    CHECK_EQ(s.stream->lost(), 1u);

    // Всё, что влезло, выходит одним тиком после форы и в том же порядке:
    // снятие ноты после её взятия.
    const soundsinth::model::PatternCell* row = s.stream->tick(kLookaheadMs);
    CHECK_EQ(s.stream->pending(), 0u);
    uint32_t notes = 0;
    for (uint32_t c = 0; c < formats::midi::kMaxChannels; ++c) {
        if (row[c].note != soundsinth::model::kNoteNone && row[c].instrument != 0) ++notes;
    }
    CHECK(notes > 0);

    // Взятие и снятие одной клавиши в одном тике: нота есть, и она отпущена -
    // порядок внутри тика сохранён.
    Session s2;
    CHECK(s2.begin(bnk));
    CHECK(s2.stream->push(0, 0x90, 64, 100));
    CHECK(s2.stream->push(0, 0x80, 64, 0));
    row = s2.stream->tick(kLookaheadMs);
    CHECK(row_has_note(row, 64));
}

// Живой режим движка: строки берутся из потока, а не из паттернов. Нота
// молчит свою фору, потом звучит, а после снятия затихает.
void test_live_stream_engine_plays() {
    std::printf("test_live_stream_engine_plays\n");
    std::vector<uint8_t> blob = read_bank(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  SKIP: no bank\n");
        return;
    }

    // Подгрузка PCM - прямо в ответ на заказ: на ПК ждать нечего.
    struct Loader {
        const bank::Bank* bank        = nullptr;
        memory::TrackMemory* mem      = nullptr;
        soundsinth::model::Song* song = nullptr;
        formats::midi::LiveMidi* live = nullptr;
        uint32_t loaded = 0, failed = 0;
        static void on_request(void* user, uint16_t song_sample) {
            auto* l = static_cast<Loader*>(user);
            if (formats::midi::load_sample_from_bank(*l->mem, *l->bank, *l->song, song_sample)) {
                ++l->loaded;
            } else {
                ++l->failed;
            }
        }
        static void on_retire(void* user, uint16_t song_sample) { static_cast<Loader*>(user)->live->record_retired(song_sample); }
    };

    auto mem = std::make_unique<memory::TrackMemory>();
    memory::track_memory_create(*mem);
    memory::track_memory_reset_for_new_track(*mem);
    auto song   = std::make_unique<soundsinth::model::Song>();
    auto live   = std::make_unique<formats::midi::LiveMidi>();
    auto stream = std::make_unique<midi_in::LiveStream>();
    Loader loader;
    loader.bank = &bnk;
    loader.mem  = mem.get();
    loader.song = song.get();
    loader.live = live.get();
    // Строка = тик: темп 250 даёт ровно 10 мс на тик.
    CHECK(live->begin(*song, *mem, bnk, 1, 250, &Loader::on_request, &Loader::on_retire, &loader) == nullptr);
    stream->begin(live.get(), kLookaheadMs);

    // Часы живого режима идут тиками по 10 мс: источник строк сам считает,
    // сколько времени прошло.
    struct Clock {
        midi_in::LiveStream* stream = nullptr;
        uint32_t tick               = 0;
        static const soundsinth::model::PatternCell* row(void* user) {
            auto* c = static_cast<Clock*>(user);
            return c->stream->tick(c->tick++ * 10u);
        }
    };
    Clock clock;
    clock.stream = stream.get();

    engine::TrackerEngine e(*song, *mem);
    e.set_live_row_source(&Clock::row, &clock);

    // Нота на нулевой миллисекунде, снятие - на 300-й.
    CHECK(stream->push(0, 0xc0, 0, 0));
    CHECK(stream->push(0, 0x90, 60, 110));
    CHECK(stream->push(300, 0x80, 60, 0));

    constexpr uint32_t kFramesPer10ms = 441;
    std::vector<int32_t> l, r;
    auto render_ms = [&](uint32_t ms) {
        const uint32_t frames = ms / 10u * kFramesPer10ms;
        render(e, l, r, frames);
        int64_t acc = 0;
        for (uint32_t i = 0; i < frames; ++i)
            acc += std::abs(l[i]) + std::abs(r[i]);
        return acc / static_cast<int64_t>(frames == 0 ? 1 : frames);
    };

    // Первые 90 мс - фора: событие ещё ждёт.
    const int64_t silent = render_ms(90);
    // Дальше нота звучит.
    const int64_t sounding = render_ms(150);
    // Снятие прошло на 300 мс + фора; к 700 мс релиз отзвучал.
    render_ms(300);
    const int64_t released = render_ms(160);

    std::printf("  level: lookahead %lld, note %lld, after the release %lld; samples loaded %u, failures %u\n", (long long)silent, (long long)sounding,
                (long long)released, loader.loaded, loader.failed);
    CHECK_EQ(silent, 0);
    CHECK(sounding > 0);
    CHECK_EQ(loader.failed, 0u);
    CHECK(loader.loaded > 0);
    CHECK(released < sounding / 4);
    memory::track_memory_destroy(*mem);
}

// Кольцо заказов PCM между ядрами: порядок, опустошение и потери.
void test_live_requests_ring() {
    std::printf("test_live_requests_ring\n");
    player::live::LiveRequestRing ring;
    ring.clear();
    uint16_t got = 0;
    CHECK(!ring.pop(got));
    for (uint16_t i = 0; i < player::live::kLiveRequestCapacity; ++i)
        CHECK(ring.push(i));
    CHECK(!ring.push(999)); // полно
    CHECK_EQ(ring.lost(), 1u);
    CHECK_EQ(ring.pending(), player::live::kLiveRequestCapacity);
    for (uint16_t i = 0; i < player::live::kLiveRequestCapacity; ++i) {
        CHECK(ring.pop(got));
        CHECK_EQ(got, i); // порядок заказов сохранён
    }
    CHECK(!ring.pop(got));
    CHECK(ring.push(7)); // место освободилось
    CHECK(ring.pop(got));
    CHECK_EQ(got, static_cast<uint16_t>(7));
}

// Рукопожатие памяти трека шагами двух ядер. Core0 вышел из режима, пока
// Core1 был между проверкой просьбы и подтверждением: подтверждения на
// снятую просьбу быть не должно, иначе следующий вход сбросит память под
// фоновой догрузкой.
void test_live_memory_handshake() {
    std::printf("test_live_memory_handshake\n");
    player::live::session_end();
    player::live::request_track_memory();
    CHECK(player::live::wants_track_memory());
    player::live::session_end();
    player::live::grant_track_memory();
    CHECK(!player::live::track_memory_granted());

    player::live::request_track_memory();
    player::live::grant_track_memory();
    CHECK(player::live::track_memory_granted());
    CHECK(!player::live::wants_track_memory());
    player::live::session_end();
    CHECK(!player::live::track_memory_granted());
}

// Новый живой сеанс начинает счёт с нуля: строки, заказы, чтения, запас
// кольца отвода и худшая строка прошлого сеанса в его журнал не попадают.
void test_live_session_begin_clears_counters() {
    std::printf("test_live_session_begin_clears_counters\n");
    std::vector<uint8_t> blob = read_bank(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  SKIP: no bank\n");
        return;
    }
    if (shared::g_track_memory.resident.base == nullptr) memory::track_memory_create(shared::g_track_memory);
    shared::g_flash_bank = bnk;
    auto stats           = [] {
        g_test_log[0] = 0;
        player::live::session_log_stats();
        return std::string(g_test_log);
    };
    auto has = [](const std::string& log, const char* want) {
        if (log.find(want) != std::string::npos) return true;
        std::printf("  no \"%s\" in:\n%s", want, log.c_str());
        return false;
    };

    player::live::session_end();
    CHECK(player::live::session_begin(1000));
    pc_tests::AyRomWriter rom;
    rom.bytes({0xc0, 0x00});
    rom.bytes({0x90, 60, 100});
    for (const uint16_t w : rom.w)
        player::live::tap_push(0, (w & midi_in::kAyWriteSelect) != 0 ? player::live::TapPort::AySelect : player::live::TapPort::AyData,
                               static_cast<uint8_t>(w));
    for (int i = 0; i < 3; ++i) {
        player::live::live_row(player::live::row_user());
        while (player::live::serve_requests()) {
        }
    }
    const std::string first = stats();
    CHECK(has(first, "over 3 rows"));
    // В первом разборе могут быть и записи прошлых тестов.
    const size_t peak_at = first.find("peak per drain ");
    CHECK(peak_at != std::string::npos);
    CHECK(peak_at != std::string::npos && std::stoul(first.substr(peak_at + 15)) >= rom.w.size());
    CHECK(first.find("samples from flash 0 ") == std::string::npos);
    CHECK(first.find("read queue peak 0,") == std::string::npos);

    player::live::session_end();
    CHECK(player::live::session_begin(2000));
    const std::string again = stats();
    CHECK(has(again, "samples from flash 0 (repeats 0, failures 0)"));
    CHECK(has(again, "records retired 0 (waiting 0)"));
    CHECK(has(again, "peak per drain 0 writes"));
    CHECK(has(again, "row - peak 0 us, mean 0 us over 0 rows"));
    CHECK(has(again, "worst row - instruments 0 (evicted 0), records 0, retires 0;"));
    CHECK(has(again, "read queue peak 0, request waited max 0 rows (mean 0), read max 0 us (mean 0)"));
    CHECK(again.find("silent notes") == std::string::npos);

    player::live::session_end();
    soundsinth::memory::track_memory_reset_for_new_track(shared::g_track_memory);
    shared::g_flash_bank = bank::Bank{};
}

// Конец сеанса забывает поток: нота в очереди под форой и недособранное
// сообщение не выходят в следующем сеансе.
void test_live_session_end_forgets_stream() {
    std::printf("test_live_session_end_forgets_stream\n");
    std::vector<uint8_t> blob = read_bank(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  SKIP: no bank\n");
        return;
    }
    if (shared::g_track_memory.resident.base == nullptr) memory::track_memory_create(shared::g_track_memory);
    shared::g_flash_bank = bnk;
    auto feed            = [](std::initializer_list<uint8_t> msg) {
        pc_tests::AyRomWriter rom;
        rom.bytes(msg);
        for (const uint16_t w : rom.w)
            player::live::tap_push(0, (w & midi_in::kAyWriteSelect) != 0 ? player::live::TapPort::AySelect : player::live::TapPort::AyData,
                                              static_cast<uint8_t>(w));
    };
    auto queued = [] {
        g_test_log[0] = 0;
        player::live::session_log_stats();
        const std::string log(g_test_log);
        const size_t at = log.find("queued ");
        return at == std::string::npos ? ~0ul : std::stoul(log.substr(at + 7));
    };

    player::live::session_end();
    CHECK(player::live::session_begin(g_test_time_us / 1000u));
    feed({0x90, 60, 100});
    feed({0x91, 62}); // без скорости
    player::live::live_row(player::live::row_user());
    CHECK_EQ(queued(), 1ul); // нота ждёт форы
    player::live::session_end();

    CHECK(player::live::session_begin(g_test_time_us / 1000u));
    CHECK_EQ(queued(), 0ul);
    feed({100}); // без сброса разбора дособрал бы ноту 62
    player::live::live_row(player::live::row_user());
    CHECK_EQ(queued(), 0ul);

    player::live::session_end();
    soundsinth::memory::track_memory_reset_for_new_track(shared::g_track_memory);
    shared::g_flash_bank = bank::Bank{};
}

// Страниц PSRAM не осталось: отказавшая загрузка просит тик отдать запись
// молчащего инструмента и повторяется, когда страницы вернулись.
void test_live_pcm_short_frees_record() {
    std::printf("test_live_pcm_short_frees_record\n");
    std::vector<uint8_t> blob = read_bank(kBankPath);
    bank::Bank bnk;
    if (blob.empty() || !bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, nullptr)) {
        std::printf("  SKIP: no bank\n");
        return;
    }
    if (shared::g_track_memory.resident.base == nullptr) memory::track_memory_create(shared::g_track_memory);
    shared::g_flash_bank = bnk;
    auto feed            = [](std::initializer_list<uint8_t> msg) {
        pc_tests::AyRomWriter rom;
        rom.bytes(msg);
        for (const uint16_t w : rom.w)
            player::live::tap_push(0, (w & midi_in::kAyWriteSelect) != 0 ? player::live::TapPort::AySelect : player::live::TapPort::AyData,
                                              static_cast<uint8_t>(w));
    };
    auto rows = [](int n) {
        for (int i = 0; i < n; ++i) {
            g_test_time_us += 10000;
            player::live::live_row(player::live::row_user());
            while (player::live::serve_requests()) {
            }
        }
    };
    auto stats = [] {
        g_test_log[0] = 0;
        player::live::session_log_stats();
        return std::string(g_test_log);
    };

    player::live::session_end();
    CHECK(player::live::session_begin(g_test_time_us / 1000u));
    memory::PsramStore& psram = shared::g_track_memory.psram;
    feed({0xc0, 0x00});
    feed({0x90, 60, 100});
    rows(10);
    feed({0x80, 60, 0});
    rows(300); // пианино замолчало больше секунды назад
    CHECK(stats().find("samples from flash 1 (repeats") != std::string::npos);
    while (memory::psram_free_page_count(psram) > 0)
        (void)memory::psram_alloc_page(psram);

    feed({0x99, 42, 100}); // хэт: сэмпл короче фортепианного
    rows(10);
    const std::string log = stats();
    std::printf("%s", log.c_str());
    CHECK(log.find("samples from flash 2 (repeats") != std::string::npos);
    CHECK(log.find("failures 0)") == std::string::npos);
    CHECK(log.find("records retired 1 ") != std::string::npos);

    player::live::session_end();
    soundsinth::memory::track_memory_reset_for_new_track(shared::g_track_memory);
    shared::g_flash_bank = bank::Bank{};
}

void run_live_stream_tests() {
    test_live_stream_lookahead();
    test_live_stream_order_and_overflow();
    test_live_stream_engine_plays();
    test_live_requests_ring();
    test_live_memory_handshake();
    test_live_session_begin_clears_counters();
    test_live_session_end_forgets_stream();
    test_live_pcm_short_frees_record();
}
