// SPDX-License-Identifier: MIT
#include "player/live/session.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"

#include "core/config.h"
#include "core/formats/midi.h"
#include "core/formats/midi_live.h"
#include "core/memory/sample_cache_catalog.h"
#include "core/memory/track_memory.h"
#include "core/live_midi/ay_midi.h"
#include "core/live_midi/live_stream.h"
#include "core/live_midi/live_sysex.h"

#include "player/live/ay_tap.h"
#include "player/shared_state.h"
#include "player/live/live_requests.h"

namespace player::live {

namespace {

using soundsinth::model::PatternCell;

// Сколько сообщений подряд считать осмысленным потоком. Одно бывает от
// случайной записи в порт A: игры дёргают его под шум и биппер.
constexpr uint32_t kStartEvents = 3;

// Приём: разбор отвода AY, очередь с упреждением, живая песня и заказы PCM.
struct State {
    soundsinth::formats::midi::LiveMidi live;
    soundsinth::midi_in::LiveStream stream;
    player::live::LiveRequestRing requests;
    // Возврат записей сэмплов: рендер просит, подгрузка выбрасывает PCM и
    // подтверждает. Подтверждать сразу нельзя - номер займёт другой сэмпл
    // банка, PCM перепишется под играющим голосом.
    player::live::LiveRequestRing retire_asked;
    player::live::LiveRequestRing retire_done;
    uint32_t retire_pending[(soundsinth::formats::midi::kLiveMaxSamples + 31u) / 32u] = {};
    uint32_t retired                                                                  = 0;
    uint32_t retire_waiting                                                           = 0; // ждут, пока голоса их отпустят
    soundsinth::midi_in::AyMidiInput input;
    bool active           = false;
    uint32_t seen_events  = 0;     // сообщений до включения режима
    bool no_bank_reported = false; // об отсутствии банка сказано
    uint32_t started_ms   = 0;
    uint32_t loaded       = 0; // сэмплов прочитано из флеша за сеанс
    uint32_t load_repeats = 0; // заказов на уже резидентный сэмпл
    uint32_t load_failed  = 0;
    uint32_t notes        = 0; // нот сыграно
    soundsinth::midi_in::SysexTracker sysex;
    // История записи сэмпла: что с ней случилось в последний раз и на какой
    // строке. По ней разбирается молчащая нота - PCM не прочитан вовремя или
    // уже выброшен. Строка живого режима равна тику, 10 мс.
    enum RecState : uint8_t { kRecNothing = 0, kRecAsked = 1, kRecRead = 2, kRecDropped = 3 };
    uint8_t rec_state[soundsinth::formats::midi::kLiveMaxSamples] = {};
    uint16_t rec_row[soundsinth::formats::midi::kLiveMaxSamples]  = {};
    // Очередь чтения: сколько заказов ждёт, сколько ждал самый давний и
    // во что обходится само чтение. Эти числа решают спор - опаздывает
    // подготовка или последовательное чтение не успевает за упреждением.
    uint32_t read_queue_peak    = 0;
    uint32_t read_wait_max_rows = 0, read_wait_sum_rows = 0, read_count = 0;
    uint32_t read_us_max = 0, read_us_sum = 0;
    uint32_t missing_seen  = 0; // сколько молчащих нот уже разобрано
    uint32_t missing_asked = 0, missing_read = 0, missing_dropped = 0, missing_unknown = 0;
    uint32_t tap_peak = 0; // записей AY за один разбор - запас кольца отвода
    // Цена живой строки внутри тика рендера.
    uint32_t rows       = 0;
    uint32_t row_us_sum = 0;
    uint32_t row_us_max = 0;
    // Что делала самая дорогая строка.
    uint32_t worst_built = 0, worst_evicted = 0, worst_records = 0, worst_retire = 0;
};

State s_state;

// Заказ PCM из тика рендера: только положить номер в кольцо. Тянуть прогон
// из флеша здесь нельзя - это миллисекунды под звуком.
void on_request(void* user, uint16_t song_sample) {
    auto* st = static_cast<State*>(user);
    st->requests.push(song_sample);
    if (song_sample < soundsinth::formats::midi::kLiveMaxSamples) {
        st->rec_state[song_sample] = State::kRecAsked;
        st->rec_row[song_sample]   = static_cast<uint16_t>(st->rows);
    }
    if (st->requests.pending() > st->read_queue_peak) st->read_queue_peak = st->requests.pending();
}

// Возврат записи просит рендер; выбросить PCM и подтвердить - за подгрузкой:
// пока запись держит хоть один голос, её страницы трогать нельзя.
void on_retire(void* user, uint16_t song_sample) {
    static_cast<State*>(user)->retire_asked.push(song_sample);
}

// Держит ли сэмпл живой голос: карту публикует движок раз в тик.
bool sample_in_use(uint16_t index) {
    if (index >= shared::kSamplesInUseBits) return true; // чужих не видим - считаем занятыми
    return (shared::g_samples_in_use[index / 32u].load(std::memory_order_acquire) & (1u << (index % 32u))) != 0;
}

// Отвод AY -> сообщения -> очередь с упреждением. Возвращает, сколько
// сообщений разобрано; SysEx тоже считается - режим по нему и узнают, плееры
// начинают со сброса GM или GS.
// В SRAM: зовётся из тика движка (через live_row), то есть с того пути,
// который обязан жить в SRAM, и на каждом тике прокручивает всё кольцо
// отвода - выборки команд из флеша идут тем же QMI, которым Core0 читает
// сэмплы из PSRAM.
uint32_t SOUNDSINTH_HOT_PATH(drain_tap)(uint32_t now_ms, bool to_stream) {
    uint32_t got = 0, writes = 0;
    uint16_t write = 0;
    soundsinth::midi_in::MidiEvent ev{};
    while (ay_tap_pop(write)) {
        ++writes;
        const bool message = s_state.input.feed(write, ev);
        if (s_state.input.sysex_ready()) {
            ++got;
            if (to_stream) {
                s_state.sysex.apply(s_state.input.sysex(), s_state.input.sysex_len(), now_ms, s_state.stream);
            }
        }
        if (!message) continue;
        ++got;
        if (!to_stream) continue;
        uint8_t status = 0, d1 = 0, d2 = 0;
        soundsinth::midi_in::midi_event_bytes(ev, status, d1, d2);
        s_state.stream.push(now_ms, status, d1, d2);
    }
    // Сколько записей накопилось между разборами: запас кольца отвода видно
    // только отсюда, а считать их у писателя нельзя - это обработчик шины.
    if (writes > s_state.tap_peak) s_state.tap_peak = writes;
    return got;
}

// Просьба о памяти трека и подтверждение. Два ядра, поэтому атомики, а не
// поля состояния: состояние пишет только Core0.
std::atomic<bool> s_mem_request{false};
std::atomic<bool> s_mem_granted{false};

} // namespace

bool session_active() {
    return s_state.active;
}

void* row_user() {
    return &s_state;
}

const PatternCell* SOUNDSINTH_HOT_PATH(live_row)(void* user) {
    auto* st          = static_cast<State*>(user);
    const uint32_t t0 = platform::mono_us();
    // Что строка успела сделать - снимком до и после: по одному времени не
    // видно, на что оно ушло.
    const uint32_t built0 = st->live.instruments_built(), evicted0 = st->live.instruments_evicted();
    const uint32_t records0 = st->live.records_allocated(), retire0 = st->live.retire_calls();
    const uint32_t now = (platform::mono_us() / 1000u);
    // Подтверждения возврата: PCM записи уже выброшен, номер свободен.
    uint16_t done = 0;
    while (st->retire_done.pop(done)) {
        st->live.record_retired(done);
    }
    drain_tap(now, /*to_stream=*/true);
    const PatternCell* row = st->stream.tick(now);
    // Живая строка идёт внутри тика рендера, и её цена в max_tick_us не
    // отделена от голосов. Считаем отдельно: строит инструменты и заводит
    // записи сэмплов именно она.
    const uint32_t dt  = platform::mono_us() - t0;
    st->row_us_sum    += dt;
    ++st->rows;
    if (dt > st->row_us_max) {
        st->row_us_max    = dt;
        st->worst_built   = st->live.instruments_built() - built0;
        st->worst_evicted = st->live.instruments_evicted() - evicted0;
        st->worst_records = st->live.records_allocated() - records0;
        st->worst_retire  = st->live.retire_calls() - retire0;
    }
    return row;
}

bool stream_started(uint32_t now_ms) {
    if (s_state.active) return false;
    // Банк во флеше живому режиму нужен весь: сэмплы он тянет только
    // оттуда, а появиться на ходу банк не может. Отвод всё равно
    // разбирается, иначе кольцо переполнялось бы, а строка печатается
    // один раз - проход цепочки идёт раз в несколько миллисекунд.
    if (!shared::g_flash_bank.valid()) {
        drain_tap(now_ms, /*to_stream=*/false);
        if (!s_state.no_bank_reported) {
            s_state.no_bank_reported = true;
            debug_log("live: no bank in flash, live MIDI unavailable\n");
        }
        return false;
    }
    // Кладём в очередь сразу: события, по которым режим и распознан (обычно
    // выбор банка и программы), нужны песне - иначе первые ноты звучат не тем
    // или молчат.
    s_state.seen_events += drain_tap(now_ms, /*to_stream=*/true);
    return s_state.seen_events >= kStartEvents;
}

uint32_t silent_ms(uint32_t now_ms) {
    return s_state.active ? s_state.stream.silent_ms(now_ms) : 0;
}

void pump_stream(uint32_t now_ms) {
    // После включения режима очередь пишет тик рендера - второй
    // писатель сломал бы и автомат разбора, и SysEx.
    if (s_state.active) return;
    if (!shared::g_flash_bank.valid()) return;
    s_state.seen_events += drain_tap(now_ms, /*to_stream=*/true);
}

// Рукопожатие передачи памяти. Просьбу ставит Core0, подтверждение -
// Core1; снимает оба выход из режима.
void request_track_memory() {
    s_mem_request.store(true, std::memory_order_release);
}

bool wants_track_memory() {
    return s_mem_request.load(std::memory_order_acquire);
}

void grant_track_memory() {
    s_mem_granted.store(true, std::memory_order_release);
}

bool track_memory_granted() {
    return s_mem_granted.load(std::memory_order_acquire);
}

bool session_begin(uint32_t now_ms) {
    if (!shared::g_flash_bank.valid()) {
        debug_log("live: no bank in flash, live mode impossible\n");
        return false;
    }
    soundsinth::memory::track_memory_reset_for_new_track(shared::g_track_memory);
    s_state.requests.clear();
    s_state.retire_asked.clear();
    s_state.retire_done.clear();
    std::memset(s_state.retire_pending, 0, sizeof(s_state.retire_pending));
    s_state.retired        = 0;
    s_state.retire_waiting = 0;
    // Строка = тик: у живого потока сетки нет, и задержку ноты внутри строки
    // выписывать некуда.
    const char* err = s_state.live.begin(shared::g_song, shared::g_track_memory, shared::g_flash_bank,
                                         /*ticks_per_row=*/1, SOUNDSINTH_LIVE_MIDI_TEMPO, &on_request, &on_retire, &s_state);
    if (err != nullptr) {
        debug_logf("live: live song could not be built: %s\n", err);
        return false;
    }
    s_state.stream.begin(&s_state.live, SOUNDSINTH_LIVE_MIDI_LOOKAHEAD_MS);
    s_state.active       = true;
    s_state.started_ms   = now_ms;
    s_state.loaded       = 0;
    s_state.load_repeats = 0;
    s_state.tap_peak     = 0;
    std::memset(s_state.rec_state, 0, sizeof(s_state.rec_state));
    std::memset(s_state.rec_row, 0, sizeof(s_state.rec_row));
    s_state.read_queue_peak    = 0;
    s_state.read_wait_max_rows = 0;
    s_state.read_wait_sum_rows = 0;
    s_state.read_count         = 0;
    s_state.read_us_max        = 0;
    s_state.read_us_sum        = 0;
    s_state.missing_seen       = 0;
    s_state.missing_asked      = 0;
    s_state.missing_read       = 0;
    s_state.missing_dropped    = 0;
    s_state.missing_unknown    = 0;
    s_state.rows               = 0;
    s_state.row_us_sum         = 0;
    s_state.row_us_max         = 0;
    s_state.worst_built        = 0;
    s_state.worst_evicted      = 0;
    s_state.worst_records      = 0;
    s_state.worst_retire       = 0;
    s_state.load_failed        = 0;
    s_state.notes              = 0;
    s_state.seen_events        = 0;
    s_state.sysex.begin();
    // Состояние транспорта осталось бы от файла, который живой режим снёс,
    // а снимает его только загрузка следующего - и живой сеанс молчал бы с
    // первой секунды. Признак "трек доиграл" гасит выход затуханием, пауза
    // уводит сведение на короткий путь нулей, а перемотка прокрутила бы
    // тики. Живому сеансу ни одно из них не нужно.
    shared::g_song_ended.store(false, std::memory_order_relaxed);
    shared::g_playback_finished.store(false, std::memory_order_relaxed);
    shared::g_playback_paused.store(false, std::memory_order_relaxed);
    shared::g_seek_frames.store(0, std::memory_order_relaxed);
    debug_logf("live: mode on, bank %s, lookahead %u ms\n", shared::g_flash_bank.header->name, static_cast<unsigned>(SOUNDSINTH_LIVE_MIDI_LOOKAHEAD_MS));
    return true;
}

void session_end() {
    // Счётчик событий сбрасывается всегда, даже когда режим так и не
    // включился: иначе неудавшийся вход оставляет счётчик за порогом,
    // и цикл пробует войти каждые пять миллисекунд, заливая журнал.
    s_state.seen_events = 0;
    s_mem_request.store(false, std::memory_order_release);
    s_mem_granted.store(false, std::memory_order_release);
    if (!s_state.active) return;
    debug_logf("live: mode off, samples from flash %" PRIu32 " (repeats %" PRIu32 ", failures %" PRIu32 "), events lost %" PRIu32 ", requests lost %" PRIu32
               "\n",
               s_state.loaded, s_state.load_repeats, s_state.load_failed, s_state.stream.lost(), s_state.requests.lost());
    s_state.active = false;
}

bool SOUNDSINTH_HOT_PATH(serve_requests)() {
    if (!s_state.active) return false;
    bool worked = false;
    // Просьбы вернуть запись: копим, пока сэмпл держат голоса.
    uint16_t leaving = 0;
    while (s_state.retire_asked.pop(leaving)) {
        if (leaving < soundsinth::formats::midi::kLiveMaxSamples) {
            s_state.retire_pending[leaving / 32u] |= 1u << (leaving % 32u);
            ++s_state.retire_waiting;
        }
    }
    // По словам маски и только когда есть что возвращать. Побитовый обход всех
    // 384 слотов стоил около 18 мкс на каждом проходе цикла Core1, а обычное
    // состояние - возвращать нечего.
    constexpr uint16_t kRetireWords = (soundsinth::formats::midi::kLiveMaxSamples + 31u) / 32u;
    for (uint16_t w = 0; s_state.retire_waiting != 0 && w < kRetireWords; ++w) {
        // Сдвигом, а не счётом младших нулей: __builtin_ctz есть не у всех
        // компиляторов проекта, а выигрыш и так в пропуске нулевых слов.
        uint32_t bits = s_state.retire_pending[w];
        for (uint16_t b = 0; bits != 0; ++b, bits >>= 1) {
            if ((bits & 1u) == 0) continue;
            const uint16_t r = static_cast<uint16_t>(w * 32u + b);
            if (sample_in_use(r)) continue; // голос ещё читает страницы
            if (auto* entry = soundsinth::memory::sample_cache_find(shared::g_track_memory.sample_cache, r)) {
                soundsinth::memory::sample_cache_evict(shared::g_track_memory.sample_cache, shared::g_track_memory.psram, entry);
            }
            s_state.retire_pending[r / 32u] &= ~(1u << (r % 32u));
            --s_state.retire_waiting;
            ++s_state.retired;
            s_state.rec_state[r] = State::kRecDropped;
            s_state.rec_row[r]   = static_cast<uint16_t>(s_state.rows);
            s_state.retire_done.push(r);
            worked = true;
        }
    }
    uint16_t index = 0;
    while (s_state.requests.pop(index)) {
        // Заказ приходит на каждую ноту, а сэмпл чаще всего уже лежит: считаем
        // это отдельно, иначе по логу не видно, сколько флеша прочитано на
        // самом деле.
        if (soundsinth::memory::sample_cache_find(shared::g_track_memory.sample_cache, index) != nullptr) {
            ++s_state.load_repeats;
            continue;
        }
        const uint32_t t0  = platform::mono_us();
        const char* reason = nullptr;
        const bool done    = soundsinth::formats::midi::load_sample_from_bank(shared::g_track_memory, shared::g_flash_bank, shared::g_song, index, &reason);
        const uint32_t dt  = platform::mono_us() - t0;
        if (done) {
            ++s_state.loaded;
            s_state.read_us_sum += dt;
            ++s_state.read_count;
            if (dt > s_state.read_us_max) s_state.read_us_max = dt;
            if (index < soundsinth::formats::midi::kLiveMaxSamples) {
                // Сколько заказ пролежал в очереди: строка живого режима - тик,
                // 10 мс, так что это прямо сравнимо с упреждением. Считается до
                // перезаписи rec_row и только по живому заказу: у выброшенного
                // между заказом и чтением там уже строка выброса, и среднее
                // от него врало бы.
                if (s_state.rec_state[index] == State::kRecAsked) {
                    const uint16_t waited       = static_cast<uint16_t>(static_cast<uint16_t>(s_state.rows) - s_state.rec_row[index]);
                    s_state.read_wait_sum_rows += waited;
                    if (waited > s_state.read_wait_max_rows) s_state.read_wait_max_rows = waited;
                }
                s_state.rec_state[index] = State::kRecRead;
                s_state.rec_row[index]   = static_cast<uint16_t>(s_state.rows);
            }
        } else {
            // Первый отказ за сеанс - строкой с причиной и числами, как у
            // файлового пути: кончились страницы, полон каталог и сбой чтения
            // банка лечатся по-разному, а счётчик load_failed их не различает.
            if (s_state.load_failed == 0) {
                // Коротко: с самой длинной причиной строка занимает 180 байт
                // из 192 буфера debug_logf.
                debug_logf("live: record %u not read: %s [pages %" PRIu32 ", slots %" PRIu32 "/%u]\n", index, reason ? reason : "?",
                           soundsinth::memory::psram_free_page_count(shared::g_track_memory.psram),
                           soundsinth::memory::sample_cache_used_count(shared::g_track_memory.sample_cache),
                           static_cast<unsigned>(soundsinth::memory::kSampleCacheCatalogCapacity));
            }
            ++s_state.load_failed;
        }
        // Один сэмпл за проход: чтение прогона - миллисекунды, а между ними
        // цикл обслуживает шину. Остальные заказы ждут в кольце.
        return true;
    }
    return worked;
}

void note_missing(const soundsinth::engine::TrackerEngine& engine) {
    if (!s_state.active) return;
    const uint32_t total = engine.triggers_without_sample();
    if (total == s_state.missing_seen) return;
    // Кольцо движка короткое: пачку длиннее него разберём с конца, остальное
    // сосчитаем неизвестными - по крайней мере число сойдётся.
    uint32_t from = s_state.missing_seen;
    if (total - from > soundsinth::engine::TrackerEngine::kMissingRing) {
        s_state.missing_unknown += total - from - soundsinth::engine::TrackerEngine::kMissingRing;
        from                     = total - soundsinth::engine::TrackerEngine::kMissingRing;
    }
    for (uint32_t n = from; n != total; ++n) {
        const uint16_t rec = engine.missing_sample(n);
        if (rec >= soundsinth::formats::midi::kLiveMaxSamples) {
            ++s_state.missing_unknown;
            continue;
        }
        const uint8_t what = s_state.rec_state[rec];
        const uint16_t ago = static_cast<uint16_t>(static_cast<uint16_t>(s_state.rows) - s_state.rec_row[rec]);
        const char* name   = "not requested";
        switch (what) {
            case State::kRecAsked:
                name = "requested, not read";
                ++s_state.missing_asked;
                break;
            case State::kRecRead:
                name = "read";
                ++s_state.missing_read;
                break;
            case State::kRecDropped:
                name = "dropped";
                ++s_state.missing_dropped;
                break;
            default:
                ++s_state.missing_unknown;
                break;
        }
        // Первые несколько разбираем поимённо, дальше только счётчиками: нот
        // бывает десятки, а кольцо лога невелико.
        if (n < 8) {
            debug_logf("live: note silent, record %u - %s %u rows ago\n", static_cast<unsigned>(rec), name, static_cast<unsigned>(ago));
        }
    }
    s_state.missing_seen = total;
}

void session_log_stats() {
    if (!s_state.active) return;
    const uint32_t now                             = (platform::mono_us() / 1000u);
    const soundsinth::formats::midi::LoadStats& st = s_state.live.stats();
    // Свой буфер: строка длиннее 192 байт буфера debug_logf. Размер - по
    // худшему случаю: текст 260 байт и двенадцать чисел по десять цифр.
    char line[384];
    snprintf(line, sizeof(line),
             "live: %" PRIu32 " s, samples from flash %" PRIu32 " (repeats %" PRIu32 ", failures %" PRIu32 "), instruments %u, evicted %" PRIu32
             ", queued %" PRIu32 ", lost events %" PRIu32 "/requests %" PRIu32 ", records retired %" PRIu32 " (waiting %" PRIu32 "), silence %" PRIu32 " ms\n",
             (now - s_state.started_ms) / 1000u, s_state.loaded, s_state.load_repeats, s_state.load_failed,
             static_cast<unsigned>(shared::g_song.instrument_count), s_state.live.instruments_evicted(), s_state.stream.pending(), s_state.stream.lost(),
             s_state.requests.lost(), s_state.retired, s_state.retire_waiting, s_state.stream.silent_ms(now));
    debug_log(line);
    // Потери раскладки: из-за них нота не звучит или звучит не тем.
    snprintf(line, sizeof(line),
             "live: notes without a zone %" PRIu32 ", over the cap %" PRIu32 ", channel steals %" PRIu32 ", note-off without delay %" PRIu32
             ", vibrato %" PRIu32 ", instrument not placed %" PRIu32 ", layer without a record %" PRIu32 "\n",
             st.notes_no_zone, st.notes_over_cap, st.steals, st.off_delay_lost, st.vib_lost, s_state.live.instruments_failed(), s_state.live.samples_capped());
    debug_log(line);
    // Байты и ошибки кадра - от самого разборщика: без них "поток идёт, а
    // нот нет" не отличить от сбоя на линии AY и от промаха keymap. Строка
    // своим буфером: в 192 байта debug_logf она не умещается.
    snprintf(line, sizeof(line),
             "live: AY tap - peak per drain %" PRIu32 " writes of %u, lost %" PRIu32 "; bytes %" PRIu32 ", framing errors %" PRIu32 ", skipped %" PRIu32
             "; reverb %s\n",
             s_state.tap_peak, static_cast<unsigned>(kAyTapRing), ay_tap_lost(), s_state.input.bytes(), s_state.input.framing_errors(),
             s_state.input.skipped_bytes(), s_state.live.reverb_used() ? "on" : "silent (no CC91)");
    debug_log(line);
    // Живая строка внутри тика рендера: её цена растёт с числом инструментов,
    // а не с числом голосов - в max_tick_us это не видно.
    snprintf(line, sizeof(line),
             "live: row - peak %" PRIu32 " us, mean %" PRIu32 " us over %" PRIu32 " rows; keymap %" PRIu32 ", record retires %" PRIu32 " times (keymap %" PRIu32
             ", records %" PRIu32 ")\n",
             s_state.row_us_max, s_state.rows ? s_state.row_us_sum / s_state.rows : 0u, s_state.rows, s_state.live.keymap_visits(), s_state.live.retire_calls(),
             s_state.live.retire_keymap_visits(), s_state.live.retire_record_visits());
    debug_log(line);
    debug_logf("live: worst row - instruments %" PRIu32 " (evicted %" PRIu32 "), records %" PRIu32 ", retires %" PRIu32 "; prepare deferred %" PRIu32
               " times\n",
               s_state.worst_built, s_state.worst_evicted, s_state.worst_records, s_state.worst_retire, s_state.stream.deferred());
    // Молчащая нота: заведено не заранее, а на её же тике - PCM заказать
    // поздно; либо запись отдали, пока нота на неё ехала.
    debug_logf("live: missed the lookahead - instruments %" PRIu32 ", records %" PRIu32 "; fresh records retired %" PRIu32 "\n",
               s_state.live.instruments_late(), s_state.live.records_late(), s_state.live.records_retired_recent());
    // Спор двух причин: подготовка отстаёт или чтение не успевает за форой.
    snprintf(line, sizeof(line),
             "live: lookahead - note margin min %" PRIu32 " ms, just in time %" PRIu32 " times; read queue peak %" PRIu32 ", request waited max %" PRIu32
             " rows (mean %" PRIu32 "), read max %" PRIu32 " us (mean %" PRIu32 ")\n",
             s_state.stream.prefetch_lead_min_ms() == soundsinth::midi_in::LiveStream::kLeadNotMeasured ? 0u : s_state.stream.prefetch_lead_min_ms(),
             s_state.stream.prefetch_tight(), s_state.read_queue_peak, s_state.read_wait_max_rows,
             s_state.read_count ? s_state.read_wait_sum_rows / s_state.read_count : 0u, s_state.read_us_max,
             s_state.read_count ? s_state.read_us_sum / s_state.read_count : 0u);
    debug_log(line);
    if (s_state.missing_seen != 0) {
        debug_logf("live: silent notes %" PRIu32 " - record requested but not read %" PRIu32 ", read %" PRIu32 ", dropped %" PRIu32 ", not requested %" PRIu32
                   "\n",
                   s_state.missing_seen, s_state.missing_asked, s_state.missing_read, s_state.missing_dropped, s_state.missing_unknown);
    }
    if (s_state.sysex.drum_changes() != 0 || s_state.sysex.volume_changes() != 0) {
        debug_logf("live: SysEx - channel drum flag %" PRIu32 ", master volume %" PRIu32 "\n", s_state.sysex.drum_changes(), s_state.sysex.volume_changes());
    }
}

} // namespace player::live
