// SPDX-License-Identifier: MIT
// Логика плеера на Core1: сессии хоста, загрузка трека по фазам, позиция.

#include "player/session.h"
#include "player/live/session.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new> // размещающий new источника сессии

#include "platform/compiler.h"
#include "platform/log.h"
#include "platform/log_rings.h"
#include "platform/hot_path.h"
#include "platform/boot_mode.h"
#include "platform/mono_time.h"
#include "platform/usb_host.h"

#include "core/config/config_page.h"
#include "core/engine/engine_defs.h"
#include "core/codec/pack_file_pcm.h"
#include "core/engine/song_duration.h"
#include "core/formats/load_stats.h"
#include "core/formats/midi.h"

#include "devices/sd/card_protocol.h"      // sd_card_set_wait_service
#include "devices/sd/spi_emu.h"            // sd_spi_task
#include "devices/storage/storage.h"       // storage_set_host_yield
#include "devices/config/config_service.h" // нужен ли USB-хост

#include "player/gs/bridge.h"
#include "player/hal/host_link.h"
#include "player/shared_state.h"
#include "player/config.h"

namespace player {

namespace {

using soundsinth::engine::kSampleRateHz;          // в этих отсчётах считает compute_song_total_frames
constexpr uint32_t kPositionIntervalUs = 1000000; // раз в секунду, для MM:SS достаточно
// Шаг перемотки за нажатие.
constexpr uint32_t kSeekStepFrames = 10u * kSampleRateHz;
// Столько без чтения порта состояния - хост ушёл из плеера. Клиент читает
// его в цикле без пауз; 200 мс - с запасом на кадр экрана и загрузку файла
// хостом.
constexpr uint32_t kHostGoneUs = 200000;
// Нагрузка обновляется чаще позиции. Окно усреднения 400 мс - 20-38 тиков
// секвенсора. Кадр телеметрии перезаписывается последним значением, очереди
// нет; под нагрузкой период растягивает сам цикл.
constexpr uint32_t kEngineLoadIntervalUs = 400000;

// Вытеснения печатаются первое и каждое kEvictLogStep-е.
constexpr uint16_t kEvictLogStep = 64;

uint8_t clamp_u8(uint32_t v, uint32_t hi) {
    return static_cast<uint8_t>(v > hi ? hi : v);
}

// Выходные отсчёты -> MM:SS для хоста; минуты - две цифры BCD, дальше по
// кругу.
struct MinSec {
    uint8_t minutes;
    uint8_t seconds;
};
MinSec to_min_sec(uint32_t frames) {
    const uint32_t total_seconds = frames / kSampleRateHz;
    return {static_cast<uint8_t>((total_seconds / 60u) % 100u), static_cast<uint8_t>(total_seconds % 60u)};
}

// Связь обслуживает hostlink, здесь то, что про трек: загрузка, план сэмплов,
// позиция. serve_and_wait уходит в BusByteSource и track_load_begin: пока
// загрузка ждёт данных, шина и карта обслуживаются.

// --- Отчёт об отказе загрузки ---
//
// Собирается целиком со счётчиками и печатается одним куском. Счёт отказов
// с запуска отличает разовый отказ от повторяющихся.
uint32_t s_load_failures = 0;

// Буфер отчёта - в своём кадре (SOUNDSINTH_NOINLINE): звать только с верхнего
// уровня цикла session_orchestrator_run, где загрузка уже вернулась, - не из
// загрузчика и не из serve_without_wait (стек Core1 4 КБ).
SOUNDSINTH_NOINLINE void report_load_failure(SessionOrchestrator& orch) {
    ++s_load_failures;
    char buf[640];
    const int cap = static_cast<int>(sizeof(buf));
    int off       = std::snprintf(buf, static_cast<size_t>(cap), "LAST FAILURE #%" PRIu32 " at %" PRIu32 " ms: len=%" PRIu32 " error=%s\n", s_load_failures,
                                  platform::mono_us() / 1000u, orch.pending_file_length, orch.load_result.error ? orch.load_result.error : "(null)");
    // Причина каждого загрузчика, а не только последнего: иначе на .it
    // с сигнатурой IMPM в логе "неизвестная сигнатура MOD".
    for (uint8_t i = 0; i < player::load::kLoaderCount; ++i) {
        if (off < cap) {
            const char* why = orch.load_result.attempt_errors[i];
            off += std::snprintf(buf + off, static_cast<size_t>(cap - off), "  %-3s: %s\n", player::load::session_loader_name(i), why ? why : "(no reason)");
        }
    }
    // Причины загрузчиков по-русски, по два байта на букву: отчёт в буфер
    // влезает не всегда, а snprintf об усечении молчит. Обрыв на середине
    // отличается от полного отчёта только этой пометкой.
    bool truncated = off >= cap;
    // Счётчики протокола при провале - за эту сессию: короткое чтение
    // бывает, когда протокол исчерпал попытки, здесь видно, так ли это,
    // а сбои прошлых треков причиной не выглядят.
    if (off < cap) {
        const player::protocol::HostProtocol::Counters c = orch.protocol.session_counters();
        const int n                                      = std::snprintf(buf + off, static_cast<size_t>(cap - off),
                                                                         "  protocol (session): not understood %" PRIu32 ", gave up %" PRIu32 ", unknown %" PRIu32 ", lost byte %" PRIu32
                                                                         ", short done %" PRIu32 ", arg/data/done overflows %" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
                                                                         c.naks, c.nak_giveups, c.unknown, c.data_short, c.short_done, c.arg_overflow, c.data_overflow, c.done_overflow);
        if (n >= cap - off) truncated = true;
    }
    // Пометка - на место хвоста: дописывать некуда, буфер уже полон.
    if (truncated) {
        static constexpr char kCut[] = "\n  REPORT TRUNCATED\n";
        std::memcpy(buf + sizeof(buf) - sizeof(kCut), kCut, sizeof(kCut));
    }
    debug_log(buf);
}

// Снять источник данных сессии: перед новой сессией и после провала или
// сброса. on_reset больше некуда передавать, фоновая догрузка
// останавливается (иначе следующий track_memory_reset_for_new_track
// разрушит то, во что она пишет).
void close_session_source(SessionOrchestrator& orch) {
    if (orch.current_bus_source != nullptr) {
        orch.current_bus_source->~BusByteSource();
        orch.current_bus_source = nullptr;
    }
    orch.plan                = player::load::ProgressiveLoader{};
    orch.longest_step_us     = 0;
    orch.longest_step_sample = 0;
    orch.load_result         = player::load::SessionLoadResult{};
}

// Отдать хосту прогресс загрузки сэмплов (кадр kStPsramStats): всего, в очереди,
// загружено. Отдельный кадр, а не бит в байте состояния: состояние
// воспроизведения и прогресс загрузки - разные величины.
void publish_load_progress(SessionOrchestrator& orch) {
    const uint16_t left = static_cast<uint16_t>(orch.plan.plan_count - orch.plan.plan_next);
    orch.protocol.set_psram_stats(orch.plan.plan_count, left, orch.plan.plan_next);
}

// Загрузка сэмпла idx из источника сессии - для progressive_load_next.
bool load_planned_sample(void* user, uint16_t idx, const char** reason_out) {
    auto& orch = *static_cast<SessionOrchestrator*>(user);
    // Флаг только для диагностики: попадают ли заминки ровно в окно
    // загрузки. Вокруг самой загрузки: шаг Waiting (при тесной памяти у .mid
    // - на каждом проходе цикла) ничего не грузит.
    shared::g_background_loading.store(true, std::memory_order_relaxed);
    const bool ok = player::load::load_track_sample(orch.load_result.format, orch.current_bus_source->as_byte_source(), shared::g_track_memory, shared::g_song,
                                                    idx, reason_out);
    shared::g_background_loading.store(false, std::memory_order_relaxed);
    return ok;
}

// Строки журнала - отдельными функциями, после шага: их буферы не лежат
// на стеке Core1 под вызовом загрузчика.

// Провал - не ошибка трека (кончилась PSRAM, каталог, файл обрезан), но в
// логе должен быть: иначе незагруженный сэмпл виден только по молчащей
// ноте. Числа, а не слова: "PSRAM кончилась" при тысячах свободных страниц
// - другая причина, чем при нуле.
SOUNDSINTH_NOINLINE void log_load_failure(const SessionOrchestrator& orch, uint16_t idx, const char* reason) {
    const uint32_t slots_used = soundsinth::memory::sample_cache_used_count(shared::g_track_memory.sample_cache);
    char m[320]; // причина отказа бывает по-русски, UTF-8 по два байта
    // Прервано - не отказ, а смена трека посреди догрузки: источник закрыт,
    // чтение короткое.
    std::snprintf(m, sizeof(m),
                  "sample %u NOT loaded (failure #%u%s): %s [pages free %" PRIu32 "/%" PRIu32 ", slots used %" PRIu32 "/%u, window missed %" PRIu32 "]\n", idx,
                  orch.plan.plan_failed, orch.current_bus_source->aborted() ? ", ABORTED by track change" : "", reason ? reason : "?",
                  soundsinth::memory::psram_free_page_count(shared::g_track_memory.psram),
                  // Фактическое число страниц, а не константа: граница зон подвижная,
                  // по константе свободных выходит больше, чем всего.
                  soundsinth::memory::psram_sample_page_count(shared::g_track_memory.psram), slots_used, soundsinth::memory::kSampleCacheCatalogCapacity,
                  orch.current_bus_source->window_missed());
    debug_log(m);
}

// Потери разбора трекерного файла - те же числа, что строка "load:"
// pc_player; отказы сэмплов плата печатает по плану загрузки.
SOUNDSINTH_NOINLINE void log_tracker_load_stats() {
    const auto& ls = soundsinth::model::g_tracker_load_stats;
    char m[224]; // кириллица в UTF-8 по два байта
    std::snprintf(m, sizeof(m), "load: undecodable %u, dropped %u, sustain loops %u, autovibrato %u, cells unparsed %" PRIu32 "\n", ls.samples_nonresident,
                  ls.samples_dropped, ls.ignored_sustain_loops, ls.ignored_autovibrato, ls.effect_cells_dropped);
    debug_log(m);
}

// Отказы загрузки без плана (сэмплов больше kProgressiveMaxSamples): одна
// строка после цикла - первый отказ и сколько всего. Строка на каждый отказ
// переполнила бы кольцо лога: сэмплов там сотни.
struct UnplannedFailures {
    uint32_t count           = 0;
    uint16_t first           = 0;
    const char* first_reason = nullptr;
};

SOUNDSINTH_NOINLINE void log_unplanned_failures(const UnplannedFailures& f, uint16_t total) {
    char m[256]; // причина отказа бывает по-русски, UTF-8 по два байта
    std::snprintf(m, sizeof(m), "samples NOT loaded: %" PRIu32 " of %u, first %u: %s\n", f.count, total, f.first, f.first_reason ? f.first_reason : "?");
    debug_log(m);
}

// Первое вытеснение и каждое kEvictLogStep-е: без строки в логе не отличить
// "работает" от "ни разу не сработало".
SOUNDSINTH_NOINLINE void log_evictions(const SessionOrchestrator& orch, uint16_t pos) {
    char m[160];
    const soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    std::snprintf(m, sizeof(m), "evict: total %u at order pos %u, past the map cap %u, pages free %" PRIu32 "%s\n", orch.plan.evicted, pos,
                  orch.plan.evict_capped, soundsinth::memory::psram_free_page_count(psram), psram.free_list_broken ? ", FREE LIST CORRUPTED" : "");
    debug_log(m);
}

// --- Застои, которых иначе в журнале не видно ---
//
// Трек "не доиграл никогда" (played стоит при playing) и стоящий Core0
// (обороты app_task стоят). Журнал обоих ядер выводит log_task на Core0,
// поэтому о Core0 - прямой печатью, не чаще раза в 10 с. Только основной
// поток Core1, не ISR.
constexpr uint32_t kStallUs       = 2000000;
constexpr uint32_t kStallRepeatUs = 10000000;

struct StallWatch {
    uint32_t loops             = 0;
    uint32_t loops_at_us       = 0;
    uint32_t busy_at_loops     = 0;
    uint32_t core0_reported_us = 0;
    bool core0_stalled         = false;
    uint32_t played            = 0;
    uint32_t played_at_us      = 0;
    bool playback_stalled      = false;
    bool was_playing           = false;
    uint32_t generation        = 0;
    uint32_t watch_at_us       = 0; // когда в тело сторожа заходили прошлый раз
};
StallWatch s_stall;

// Зовётся и из serve_without_wait: Core1 ждёт сноса движка в track_load_begin без
// срока, и стоящий Core0 должен быть виден и там.
SOUNDSINTH_NOINLINE void SOUNDSINTH_HOT_PATH(watch_core0)(uint32_t now) {
    StallWatch& w        = s_stall;
    const uint32_t loops = shared::g_app_task_loops.load(std::memory_order_relaxed);
    if (loops == 0) return; // app_task ещё не запущен (Core0 грузит банк и прочее)
    const uint32_t busy = shared::g_render_busy_us.load(std::memory_order_relaxed);
    if (loops != w.loops) {
        if (w.core0_stalled) {
            char m[64];
            std::snprintf(m, sizeof(m), "core0: alive again after %" PRIu32 " ms\n", (now - w.loops_at_us) / 1000u);
            debug_log(m);
        }
        w.loops         = loops;
        w.loops_at_us   = now;
        w.busy_at_loops = busy;
        w.core0_stalled = false;
        return;
    }
    if (now - w.loops_at_us < kStallUs) return;
    if (w.core0_stalled && now - w.core0_reported_us < kStallRepeatUs) return;
    // Занятость рендера растёт вровень со временем - RenderTask не отдаёт
    // ядро; стоит - Core0 встал (assert, отказ, отладчик).
    char m[96];
    std::snprintf(m, sizeof(m), "core0 stall %" PRIu32 " ms: busy +%" PRIu32 " ms, gen %" PRIu32 "/%" PRIu32 " alive %d\n", (now - w.loops_at_us) / 1000u,
                  (busy - w.busy_at_loops) / 1000u, shared::g_song_generation.load(std::memory_order_relaxed),
                  shared::g_engine_generation.load(std::memory_order_relaxed), shared::g_engine_alive.load(std::memory_order_relaxed));
    platform::log_put_blocking(m);
    w.core0_stalled     = true;
    w.core0_reported_us = now;
}

// Трек играет, а отыгранное не растёт: движок не собран (поколение
// собранного отстаёт), рендер или app_task стоят, или загрузка держит
// запрет. Строка на застой и строка на возобновление.
SOUNDSINTH_NOINLINE void SOUNDSINTH_HOT_PATH(watch_playback)(const SessionOrchestrator& orch, uint32_t now) {
    StallWatch& w = s_stall;
    // Не чаще четверти порога: сработать сторож может раз в kStallUs, а
    // зовут его на каждом прерывании шины, и тело - шесть атомарных
    // загрузок с ветвлением. На витке остаётся сравнение двух чисел.
    if (now - w.watch_at_us < kStallUs / 4u) return;
    w.watch_at_us         = now;
    const uint32_t played = shared::g_playback_frames.load(std::memory_order_relaxed);
    // Отсчёт - с начала игры нового трека: во время загрузки основной цикл не
    // крутится, и без этого застой отсчитывался бы от прошлого трека.
    const uint32_t gen = shared::g_song_generation.load(std::memory_order_relaxed);
    const bool started = (orch.playing && !w.was_playing) || gen != w.generation;
    w.was_playing      = orch.playing;
    w.generation       = gen;
    // Пауза и перемотка останавливают позицию нарочно - это не застой.
    // Отсчёт при этом сдвигается, поэтому после снятия паузы сторож не
    // объявит застоем первые же секунды.
    const bool held = shared::g_playback_paused.load(std::memory_order_relaxed) || shared::g_seek_frames.load(std::memory_order_relaxed) != 0;
    if (!orch.playing || started || held || shared::g_playback_finished.load(std::memory_order_relaxed) || played != w.played) {
        if (w.playback_stalled && played != w.played) debug_log("playback resumed\n");
        w.played           = played;
        w.played_at_us     = now;
        w.playback_stalled = false;
        return;
    }
    if (w.playback_stalled || now - w.played_at_us < kStallUs) return;
    debug_logf("playback stalled: played=%" PRIu32 " total=%" PRIu32 " gen=%" PRIu32 " built=%" PRIu32 " alive=%d load=%d\n", played,
               orch.load_result.total_frames, shared::g_song_generation.load(std::memory_order_relaxed),
               shared::g_engine_generation.load(std::memory_order_relaxed), shared::g_engine_alive.load(std::memory_order_relaxed),
               shared::g_load_in_progress.load(std::memory_order_relaxed));
    w.playback_stalled = true;
}

// Сбои связи за сессию (приросты с kHcStart). Печатается и при нуле:
// "сбоев не было" отличается от "диагностика выключена". Раз на сессию: на
// конце плана, в отказе или, если сессию сменили раньше, на старте
// следующей.
SOUNDSINTH_NOINLINE void log_session_link(SessionOrchestrator& orch, const player::protocol::HostProtocol::Counters& c, const char* when) {
    char sm[224];
    std::snprintf(sm, sizeof(sm),
                  "window check (%s): frame failures %" PRIu32 ", gave up %" PRIu32 ", unknown %" PRIu32 ", lost byte %" PRIu32 ", short done %" PRIu32
                  ", overflow data/done/arg %" PRIu32 "/%" PRIu32 "/%" PRIu32 ", events lost %" PRIu32 "\n",
                  when, c.naks, c.nak_giveups, c.unknown, c.data_short, c.short_done, c.data_overflow, c.done_overflow, c.arg_overflow, c.events_lost);
    debug_log(sm);
    orch.link_reported = true;
}

// План кончился. Сброса кэша XIP нет: кэш у ядер общий, мимо кэша PSRAM не
// читает никто, а xip_cache_clean_all() из-за обхода errata E11 инвалидирует
// весь кэш с кодом Core0 - под звук щелчки.
SOUNDSINTH_NOINLINE void finish_plan(SessionOrchestrator& orch) {
    log_session_link(orch, orch.protocol.session_counters(), "planned set loaded");
    char m[256]; // кириллица в UTF-8 по два байта
    const soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    std::snprintf(m, sizeof(m),
                  "background prefetch done: %u samples, %u failures, evicted %u, pages free %" PRIu32 "/%" PRIu32 "%s, longest step %" PRIu32
                  " ms (sample %u), t=%" PRIu32 " ms\n",
                  orch.plan.plan_count, orch.plan.plan_failed, orch.plan.evicted, soundsinth::memory::psram_free_page_count(psram),
                  soundsinth::memory::psram_sample_page_count(psram), psram.free_list_broken ? ", FREE LIST CORRUPTED" : "", orch.longest_step_us / 1000u,
                  orch.longest_step_sample, (platform::mono_us() / 1000u));
    debug_log(m);
}

// Сон холостого витка: будит любое прерывание связи (порт, заказ сектора,
// байт протокола, GS) или срок - флаги другого ядра (позиция, конец трека)
// ждут не дольше.
constexpr uint32_t kSleepMaxUs = 1000;

// Строка - в своём кадре: вызывается из цикла, не из загрузчика.
SOUNDSINTH_NOINLINE void log_host_presence(bool here) {
    debug_log(here ? "prefetch: host is back, running\n" : "prefetch: waiting for the host (host outside the player)\n");
}

// Шаг фоновой догрузки (логика в ядре). false - ждать позицию, план кончился
// или источник закрыт: на false выходит цикл префетча.
// Упреждение только у .mid: замер на 114499 файлах при 200 КБ/с - девяти из
// десяти хватает секунды упреждения и около мегабайта памяти.
bool SOUNDSINTH_HOT_PATH(load_next_planned_sample)(SessionOrchestrator& orch) {
    player::load::ProgressiveLoader& pl = orch.plan;
    if (pl.plan_next >= pl.plan_count || orch.current_bus_source == nullptr) return false;
    if (orch.aborted || orch.current_bus_source->aborted()) return false;

    const uint16_t pos = shared::g_order_pos.load(std::memory_order_acquire);
    pl.no_eviction     = player::load::progressive_eviction_blocked(orch.load_result.duration.position_goes_back, orch.load_result.total_frames,
                                                                    shared::g_playback_frames.load(std::memory_order_relaxed));
    const player::load::SamplesInUse in_use{shared::g_samples_in_use, shared::kSamplesInUseBits};
    const uint16_t evicted_before  = pl.evicted;
    uint16_t idx                   = 0;
    const char* reason             = nullptr;
    const uint32_t step_started_us = platform::mono_us();
    const player::load::ProgressiveStep step =
        player::load::progressive_load_next(pl, shared::g_track_memory, pos, in_use, &load_planned_sample, &orch, &idx, &reason);
    // Весь шаг - один оборот внешнего цикла: всё это время стоят проверка
    // "доиграл" и позиция хосту.
    const uint32_t step_us = platform::mono_us() - step_started_us;
    if (step_us > orch.longest_step_us) {
        orch.longest_step_us     = step_us;
        orch.longest_step_sample = idx;
    }
    if (step == player::load::ProgressiveStep::Waiting || step == player::load::ProgressiveStep::Done) {
        return false;
    }

    publish_load_progress(orch);
    if (pl.evicted != evicted_before && (evicted_before == 0 || pl.evicted / kEvictLogStep != evicted_before / kEvictLogStep)) {
        log_evictions(orch, pos);
    }
    if (step == player::load::ProgressiveStep::Failed) log_load_failure(orch, idx, reason);
    // Переход к концу плана - на шаге Loaded/Failed, один раз: после него
    // этот вызов выходит раньше, по plan_next.
    if (pl.plan_next >= pl.plan_count) finish_plan(orch);
    return true;
}

} // namespace

void publish_file_info(uint32_t total_frames, const soundsinth::model::Song& song) {
    // Ноль - длительность неизвестна: проход длительности не влез в память
    // трека. Хост в этом случае рисует прочерки, а 00:00 он прочитал бы как
    // пустой трек.
    constexpr uint8_t kUnknown = player::protocol::HostProtocol::kTimeUnknown;
    const MinSec t             = to_min_sec(total_frames);
    const uint8_t mm           = total_frames != 0 ? t.minutes : kUnknown;
    const uint8_t ss           = total_frames != 0 ? t.seconds : kUnknown;
    player::hal::host_set_file_info(mm, ss, song.sample_count, song.pattern_count, song.instrument_count);
}

namespace {

// Обслуживание карты и шины, пока загрузка ждёт данных от хоста.
//
// Карта обслуживается первой и во всех сборках: на машине без своего
// интерфейса хост читает файл с нашей карты, и без обслуживания выходит
// круговое ожидание - Z80 ждёт сектор, плата ждёт Z80.
void SOUNDSINTH_HOT_PATH(serve_and_wait)(void*) {
    shared::g_core1_loops.store(shared::g_core1_loops.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    devices::sd::sd_spi_task();
    player::hal::host_service_and_wait();
    watch_core0(platform::mono_us());
}

} // namespace

// Арбитр носителя пропускает хост вперёд фоновых чтений (банк .mid):
// заказы хоста забирает эмулятор карты.
namespace {
void SOUNDSINTH_HOT_PATH(yield_storage_to_host)(void*) {
    devices::sd::sd_spi_task();
}
} // namespace

void SOUNDSINTH_HOT_PATH(serve_without_wait)(void*) {
    shared::g_core1_loops.store(shared::g_core1_loops.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    devices::sd::sd_spi_task();
    player::hal::host_service();
}

namespace {
// Долгие проходы разбора зовут то же, что ожидание байта от хоста: эмулятор
// карты и протокол. Отдельное имя - чтобы крюк ставился осознанно, а не
// случайным совпадением подписи.
void SOUNDSINTH_HOT_PATH(session_service)(void* user) {
    serve_without_wait(user);
}
} // namespace

namespace {

// Обслуживание из ожиданий драйвера карты: только шина. Эмулятор карты
// отсюда не двигается - он сам пошёл бы к карте, которая занята тем самым
// ожиданием, и получил бы отказ.
void SOUNDSINTH_HOT_PATH(serve_bus_while_card_waits)(void*) {
    shared::g_core1_loops.store(shared::g_core1_loops.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    player::hal::host_service();
}

// Ожидания внутри USB-стека: к шине добавляется цикл эмулятора
// карты. Пауза перечисления - около 500 мс по замеру платы, и всё это время
// заказ сектора от машины никто не брал: esxDOS ждал токен сорок тысяч
// опросов и срывался.
//
// Только для USB. Ожиданию самой карты этого давать нельзя: цикл
// эмулятора сам ходит к носителю, и вышла бы рекурсия в драйвер карты.
// В обратную сторону всё закрыто признаком внутри sd_spi_task: ожидание
// обмена с флешкой приходит сюда изнутри этого же цикла.
void SOUNDSINTH_HOT_PATH(serve_bus_and_card_while_usb_waits)(void* user) {
    serve_bus_while_card_waits(user);
    devices::sd::sd_spi_task();
}

} // namespace

namespace {

SOUNDSINTH_HOT_PATH_ATTR("so_on_session_start")
void on_session_start(void* user, uint32_t, uint32_t file_length, uint8_t, uint8_t load_order) {
    auto* self                = static_cast<SessionOrchestrator*>(user);
    self->pending_file_length = file_length;
    // Всё, кроме ByPlayback, - ByFile: хост без выбора стратегии шлёт ноль.
    self->pending_load_order =
        (load_order == static_cast<uint8_t>(player::load::LoadOrder::ByPlayback)) ? player::load::LoadOrder::ByPlayback : player::load::LoadOrder::ByFile;
    self->session_pending = true;
}

SOUNDSINTH_HOT_PATH_ATTR("so_on_reset")
void on_reset(void* user) {
    auto* self    = static_cast<SessionOrchestrator*>(user);
    self->aborted = true;
    self->playing = false;
    // START и RESET в одном разборе: сессию, от которой хост ушёл, не
    // начинать. START после RESET взведёт её снова - колбэки идут по порядку
    // кадров.
    self->session_pending = false;
    if (self->current_bus_source) self->current_bus_source->on_reset();
    // Трек, поднятый до сброса и ещё не собранный, не собирать: хост от
    // него ушёл.
    shared::g_stale_generation.store(shared::g_song_generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
    // Безусловно, даже без следующей сессии (ESC тоже глушит звук). Core0
    // (app_task) останавливает текущий TrackerEngine/RenderTask.
    shared::g_teardown_requested.store(true, std::memory_order_release);
}

} // namespace

player::protocol::HostProtocol::Callbacks session_orchestrator_callbacks(SessionOrchestrator& orch) {
    player::protocol::HostProtocol::Callbacks cb;
    cb.user             = &orch;
    cb.on_session_start = &on_session_start;
    cb.on_reset         = &on_reset;
    return cb;
}

namespace {

// Строки цикла - своими кадрами (SOUNDSINTH_NOINLINE): кадр
// session_orchestrator_run лежит под run_session_load, самым глубоким путём
// стека Core1, и буферам печати в нём не место.

// Трек платы снят командой загрузки модуля GS: хост о закрытии не узнаёт (Ended не уходит) -
// без строки это в журнале не отличить от залипания.
SOUNDSINTH_NOINLINE void log_track_taken_by_gs(const SessionOrchestrator& orch) {
    debug_logf("session: board track dropped by the GS player (#30), loaded %u of %u, t=%" PRIu32 " ms\n", orch.plan.plan_next, orch.plan.plan_count,
               (platform::mono_us() / 1000u));
}

SOUNDSINTH_NOINLINE void log_track_taken_by_live(const SessionOrchestrator& orch) {
    debug_logf("session: board track dropped by live MIDI, loaded %u of %u, t=%" PRIu32 " ms\n", orch.plan.plan_next, orch.plan.plan_count,
               (platform::mono_us() / 1000u));
}

// Почему кончился - с числами. "Загрузился и сразу уехал на следующий"
// одинаково выглядит для двух причин: секвенсор объявил файл вырожденным,
// или длительность посчиталась нулём и played >= total сработало сразу.
SOUNDSINTH_NOINLINE void log_track_ended(const SessionOrchestrator& orch, uint32_t played, bool song_ended) {
    debug_logf("track ended: %s, played %" PRIu32 " s of %" PRIu32 " s, t=%" PRIu32 " ms\n", song_ended ? "sequencer declared the end" : "full duration played",
               played / kSampleRateHz, orch.load_result.total_frames / kSampleRateHz, (platform::mono_us() / 1000u));
}

SOUNDSINTH_NOINLINE void log_duration(const SessionOrchestrator& orch, uint32_t phase_a_ms) {
    const auto& d = orch.load_result.duration;
    debug_logf("duration: %" PRIu32 " ms, rows %" PRIu32 ", ticks %" PRIu32 ", stop=%s (%u,%u)%s, phase A %" PRIu32 " ms\n",
               static_cast<uint32_t>(static_cast<uint64_t>(orch.load_result.total_frames) * 1000u / kSampleRateHz), d.rows, d.ticks,
               soundsinth::engine::duration_stop_name(d.stop), d.stop_order_pos, d.stop_row, d.position_goes_back ? ", backwards" : "", phase_a_ms);
}

// Числа загрузки .mid - загрузчик сам не печатает (printf с Core1 посреди
// обмена по шине), строки те же, что у pc_player.
SOUNDSINTH_NOINLINE void log_midi_stats() {
    const auto& st = soundsinth::formats::midi::last_load_stats();
    char mm[192];
    std::snprintf(mm, sizeof(mm), "midi: instr %" PRIu32 ", samples %" PRIu32 ", keymap %" PRIu32 ", env %" PRIu32 " -> arena ~%" PRIu32 " B\n", st.instruments,
                  st.samples, st.keymap_ranges, st.envelopes, st.arena_bytes);
    debug_log(mm);
    if (st.psram_known) {
        std::snprintf(mm, sizeof(mm), "midi: PSRAM pages free %" PRIu32 ", samples need %" PRIu32 " KB, patterns %" PRIu32 " KB\n", st.free_pages,
                      st.samples_need_kb, st.patterns_kb);
        debug_log(mm);
    }
    if (st.steals || st.off_delay_lost || st.vib_lost || st.tempo_deferred || st.tempo_dropped) {
        std::snprintf(mm, sizeof(mm),
                      "midi: losses - steals %" PRIu32 ", note-off without delay %" PRIu32 ", vibrato %" PRIu32 ", tempo deferred %" PRIu32
                      ", past the tempo map %" PRIu32 "\n",
                      st.steals, st.off_delay_lost, st.vib_lost, st.tempo_deferred, st.tempo_dropped);
        debug_log(mm);
    }
    if (st.notes_over_cap || st.notes_no_zone) {
        std::snprintf(mm, sizeof(mm), "midi: silent notes - over 511 instruments %" PRIu32 ", no zone in the bank %" PRIu32 "\n", st.notes_over_cap,
                      st.notes_no_zone);
        debug_log(mm);
    }
}

// Сколько ждали до первой ноты и за что: размен "задержка против неполного
// состава" - числом в логе.
struct PrefetchReport {
    uint16_t samples;
    uint16_t positions;
    uint32_t prefetch_ms;
    int failed;
    uint32_t teardown_ms;
    uint32_t plan_ms;
};

SOUNDSINTH_NOINLINE void log_prefetch(const SessionOrchestrator& orch, const PrefetchReport& r) {
    debug_logf("prefetch: %u of %u samples, %u order pos, %" PRIu32 " ms, failed %d, teardown waited %" PRIu32 " ms, plan %" PRIu32 " ms, t=%" PRIu32 " ms\n",
               r.samples, orch.plan.plan_count, r.positions, r.prefetch_ms, r.failed, r.teardown_ms, r.plan_ms, (platform::mono_us() / 1000u));
}

SOUNDSINTH_NOINLINE void log_plan_disabled(uint16_t samples) {
    debug_logf("progressive loading disabled: samples %u > %u\n", samples, player::load::kProgressiveMaxSamples);
}

// Сброс от хоста посреди чтения закрывает источник, загрузчик получает
// короткое чтение и говорит "файл обрезан", хотя файл целый.
SOUNDSINTH_NOINLINE void log_load_aborted(const SessionOrchestrator& orch) {
    debug_logf("load interrupted by a track change (len=%" PRIu32 ") - not a failure\n", orch.pending_file_length);
}

// Средняя полифония и загрузка ядра за период опроса. Счётчики в
// shared_state монотонные: сбрасывать их с этого ядра - гонка. Прошлые
// значения хранятся здесь, считается разность.
struct EngineLoadWatch {
    uint32_t last_us;
    uint32_t busy_us;
    uint32_t voice_ticks;
    uint32_t voice_sq;
};

EngineLoadWatch engine_load_watch_now(uint32_t now) {
    return {now, shared::g_render_busy_us.load(std::memory_order_relaxed), shared::g_voice_count_ticks.load(std::memory_order_relaxed),
            shared::g_voice_count_sq_sum.load(std::memory_order_relaxed)};
}

void update_engine_load(SessionOrchestrator& orch, EngineLoadWatch& w, uint32_t now) {
    if (now - w.last_us < kEngineLoadIntervalUs) return;
    const EngineLoadWatch cur = engine_load_watch_now(now);
    // Пик читается со сбросом: максимум за период монотонным числом не
    // выразить.
    const uint32_t vpeak = shared::g_voice_count_peak.exchange(0, std::memory_order_relaxed);

    const uint32_t d_busy  = cur.busy_us - w.busy_us; // разность переживает переполнение uint32
    const uint32_t d_wall  = now - w.last_us;
    const uint32_t d_ticks = cur.voice_ticks - w.voice_ticks;
    const uint32_t d_sq    = cur.voice_sq - w.voice_sq;

    uint32_t cpu = (d_wall > 0) ? (d_busy * 100u) / d_wall : 0u;
    if (cpu > 100u) cpu = 100u; // округление на границе

    // Квадратичное среднее с округлением вверх: хватит ли ядра, решает
    // всплеск, а обычное среднее его почти не замечает. Корень целочисленный,
    // перебором: до SOUNDSINTH_MAX_VOICES итераций раз в две секунды.
    const uint32_t mean_sq = (d_ticks > 0) ? (d_sq / d_ticks) : 0u;
    uint32_t typical       = 0;
    while ((typical + 1u) * (typical + 1u) <= mean_sq) {
        ++typical; // floor(sqrt)
    }
    if (typical * typical < mean_sq) ++typical; // -> ceil

    // Уровень сброса голосов считает Core0 (видит бюджет), здесь только
    // пересылка.
    const uint32_t cull = shared::g_voice_cull_level.load(std::memory_order_relaxed);
    orch.protocol.set_engine_load(clamp_u8(typical, 255u), clamp_u8(vpeak, 255u), static_cast<uint8_t>(cpu), clamp_u8(cull, shared::kVoiceCullLevelMax));
    w = cur;
}

// Новая сессия хоста: снос прошлого трека, фазы A-D. true - трек играет.
// Кадр этой функции и цикла лежит под run_session_load: буферы печати - в
// функциях строк.
bool run_session(SessionOrchestrator& orch) {
    orch.session_pending = false;
    orch.aborted         = false;
    // Прошлую сессию сменили до конца её плана (next/prev, ESC, прерванная
    // загрузка, план пуст или больше плана) - её итог сейчас.
    if (!orch.link_reported) {
        log_session_link(orch, orch.protocol.previous_session_counters(), "previous session");
    }
    orch.link_reported = false;

    // Прошлая сессия и её недогруженный хвост закрываются до
    // track_memory_reset_for_new_track внутри run_session_load.
    close_session_source(orch);
    // Сброс от хоста уже попросил снести движок, но Core0 делает это не
    // мгновенно: загрузка ждёт сноса, пока шина обслуживается.
    shared::g_core1_phase.store(shared::Core1Phase::TeardownWait, std::memory_order_relaxed);
    const uint32_t teardown_wait_started_us = platform::mono_us();
    shared::track_load_begin(&serve_and_wait, nullptr);
    const uint32_t teardown_wait_ms = (platform::mono_us() - teardown_wait_started_us) / 1000u;
    shared::g_core1_phase.store(shared::Core1Phase::Load, std::memory_order_relaxed);
    // Трек через протокол платы - плееру GS мегабайт больше не нужен.
    gs::bridge_release_buffer();
    orch.current_bus_source = new (orch.bus_source_storage) player::load::BusByteSource(orch.protocol, orch.pending_file_length, &serve_and_wait, nullptr);
    // Сброс, пришедший, пока track_load_begin ждал сноса, источника ещё не
    // застал: без этого фаза A ждала бы окна от ушедшего хоста до
    // следующего сброса.
    if (orch.aborted) orch.current_bus_source->on_reset();

    // Фаза A - метаданные без PCM. Массивы плана отдаются ей же: план
    // собирается проходом длительности, и второго обхода песни не нужно.
    // Буфер загрузчика к этому моменту свободен - разбор кончился.
    player::load::progressive_attach_plan(orch.plan, reinterpret_cast<uint16_t*>(shared::g_track_memory.loader_scratch_buffer));
    const uint32_t phase_a_started_us = platform::mono_us();
    bool ok = player::load::run_session_load(orch.current_bus_source->as_byte_source(), shared::g_track_memory, shared::g_song, orch.load_result,
                                             /*metadata_only=*/true, orch.plan.plan_indices, player::load::kProgressiveMaxSamples);
    if (ok) {
        publish_file_info(orch.load_result.total_frames, shared::g_song);
        log_duration(orch, (platform::mono_us() - phase_a_started_us) / 1000u);
        if (orch.load_result.format == player::load::TrackFormat::Midi) {
            log_midi_stats();
        } else {
            log_tracker_load_stats();
        }
    }

    uint16_t prefetch_count = 0;
    if (ok && !orch.aborted) {
        if (shared::g_song.sample_count <= player::load::kProgressiveMaxSamples) {
            // Фаза B - план. Граница префетча у .mid по времени
            // (SOUNDSINTH_MIDI_PREFETCH_SECONDS), а не по паттернам: их длина
            // зависит от сетки и темпа. Проход секвенсора берёт буфер сценариев,
            // свободный к этому моменту.
            uint16_t prefetch_positions = player::load::kPrefetchOrderPositions;
#if SOUNDSINTH_MIDI_PREFETCH_SECONDS
            if (orch.load_result.format == player::load::TrackFormat::Midi) {
                // По кадрам начала паттернов, посчитанным при разборе: проход
                // секвенсора прогнал бы весь трек через конвертер заново.
                const uint16_t by_time = soundsinth::formats::midi::order_positions_in_frames(SOUNDSINTH_MIDI_PREFETCH_SECONDS * kSampleRateHz);
                if (by_time > prefetch_positions) prefetch_positions = by_time;
                // Это же упреждение фоновой догрузки. Только для .mid: у трекерных
                // подкачка не мерена.
                orch.plan.lead_positions = by_time;
            }
#endif
            // Массивы плана - в loader_scratch_buffer: разбор кончился, до
            // close_session_source он принадлежит плану. Прицеплены ещё до
            // фазы A, которая план и собрала.
            const uint32_t plan_started_us = platform::mono_us();
            // План собран проходом длительности - остаётся поделить его на
            // префетч и хвост. Проход был неполным - плана нет, идём старым
            // обходом по order: он не смотрит переходы, зато не зависит от
            // того, докуда дошёл секвенсор.
            const player::load::PlaybackPlan plan =
                orch.load_result.plan_usable ? player::load::plan_split_prefetch(shared::g_song, orch.plan.plan_indices, orch.load_result.plan_count,
                                                                                 orch.plan.plan_first_use, prefetch_positions, orch.pending_load_order)
                                             : player::load::plan_playback_order(shared::g_song, shared::g_track_memory.psram, orch.plan.plan_indices,
                                                                                 player::load::kProgressiveMaxSamples, orch.plan.plan_last_use,
                                                                                 orch.pending_load_order, prefetch_positions, orch.plan.plan_first_use);
            const uint32_t plan_ms = (platform::mono_us() - plan_started_us) / 1000u;
            orch.plan.plan_count   = plan.count;
            prefetch_count         = plan.prefetch_count;
            orch.plan.plan_next    = 0;
            player::load::progressive_plan_changed(orch.plan);
            // План есть - хосту можно сказать, сколько сэмплов всего и сколько в
            // очереди.
            publish_load_progress(orch);
            // Фаза C - префетч: сэмплы первых позиций, остальное фоном под звук.
            const uint32_t prefetch_started_us = platform::mono_us();
            const uint16_t failed_before       = orch.plan.plan_failed;
            while (orch.plan.plan_next < prefetch_count && load_next_planned_sample(orch)) {
            }
            log_prefetch(orch, PrefetchReport{prefetch_count, prefetch_positions, (platform::mono_us() - prefetch_started_us) / 1000u,
                                              orch.plan.plan_failed - failed_before, teardown_wait_ms, plan_ms});
            if (orch.aborted || orch.current_bus_source->aborted()) ok = false;
        } else {
            // Сэмплов больше, чем помещается в план, - грузится всё до старта
            // звука, по индексам подряд и только PCM: повторный run_session_load
            // заставил бы хост читать файл с начала. Такое бывает у XM (до 128
            // инструментов по 16 сэмплов) и у .mid, где сэмплы идут из банка.
            log_plan_disabled(shared::g_song.sample_count);
            UnplannedFailures fails;
            for (uint16_t i = 0; i < shared::g_song.sample_count; ++i) {
                if (orch.aborted || orch.current_bus_source->aborted()) {
                    ok = false;
                    break;
                }
                const char* reason = nullptr;
                if (!player::load::load_track_sample(orch.load_result.format, orch.current_bus_source->as_byte_source(), shared::g_track_memory, shared::g_song,
                                                     i, &reason)) {
                    if (fails.count++ == 0) {
                        fails.first        = i;
                        fails.first_reason = reason;
                    }
                }
            }
            if (fails.count != 0) log_unplanned_failures(fails, shared::g_song.sample_count);
        }
    }

    // Движок собирается только по смене поколения ниже: снятый запрет без
    // неё ничего не собирает, и при отказе тоже.
    shared::track_load_end();
    if (ok && !orch.aborted) {
        // Фаза D - старт звука. Хвост плана догружается фоном во внешнем цикле.
        // Сброса кэша XIP нет: движок читает сэмплы тем же кэшем.
        shared::publish_loaded_track(orch.load_result.total_frames);
        orch.protocol.mark_session_ready();
        orch.playing = true;
        return true;
    }
    // Неудача (нераспознанный или битый файл) или сброс посреди загрузки -
    // своего кода ошибки в протоколе нет, сессия закрывается без
    // SESSION_READY. Прервано - не отказ, в отчёт не попадает.
    orch.playing = false;
    if (orch.aborted) {
        log_load_aborted(orch);
        // end_session() не нужен: протокол уже сброшен RESET хоста.
        close_session_source(orch);
        return false;
    }
    report_load_failure(orch);
    // Числа .mid и при отказе: по ним видно, где загрузка встала - на слоях,
    // на арене или на зоне паттернов. Признаки *_known говорят, докуда дошло.
    if (soundsinth::formats::midi::last_load_stats().layers_known) log_midi_stats();
    orch.link_reported = true;
    close_session_source(orch);
    orch.protocol.end_session();
    return false;
}

// Конфигуратор настроек: ПЗУ правит значения прямо в странице, а правила
// считает плата. Пересчёт и выход разбираются здесь, а не в прерывании:
// оба трогают настройки целиком.
void serve_configurator(SessionOrchestrator& orch) {
    uint8_t* const page = platform::config_rom_page();
    if (page == nullptr) return;
    const uint32_t cap = platform::config_rom_page_bytes();

    if (orch.protocol.take_config_refresh()) {
        soundsinth::config::Settings draft = devices::config::settings();
        const bool ok                      = soundsinth::config::config_page_apply(page, cap, draft);
        if (ok) {
            devices::config::settings_set(draft);
            soundsinth::config::config_page_refresh(draft, page, cap);
        }
        orch.protocol.arm_config_done(ok);
    }

    const player::protocol::ConfigExit exit = orch.protocol.take_config_exit();
    if (exit == player::protocol::ConfigExit::None) return;

    // Выход - это перезагрузка платы: записывать флеш и карту отсюда
    // нельзя, на время стирания пропадает окно XIP. Запрос исполнит
    // загрузка.
    platform::BootRequest req;
    req.configurator = exit == player::protocol::ConfigExit::Reenter;
    req.save         = exit == player::protocol::ConfigExit::Save;
    // Приглашение с карты снимает любой осознанный выход; простой сброс
    // запроса не оставляет, и машина попадёт в конфигуратор снова.
    req.drop_open_gui = true;
    req.settings      = devices::config::settings();
    debug_logf("config: exit %u, reboot\n", static_cast<unsigned>(exit));
    // Машину в сброс: после перезагрузки платы шины для неё нет.
    player::hal::host_hold_for_reboot();
    platform::boot_reboot(req);
}

} // namespace

// Шина и протокол целиком на Core1: обработчики шины и этот цикл на одном
// ядре, гонок по HostProtocol нет (он не потокобезопасен). По времени этот
// цикл не критичен: защёлкивающая часть обмена сделана в ISR и PIO.
void SOUNDSINTH_HOT_PATH(session_orchestrator_run)(SessionOrchestrator& orch) {
    shared::g_bank.serve = &serve_without_wait;
    devices::storage::storage_set_host_yield(&yield_storage_to_host, nullptr);
    // Долгие проходы разбора: без этого крюка проход длительности держит
    // виток Core1 секундами, и заказ сектора от esxDOS ждёт всё это время.
    player::load::session_loader_set_service(&session_service, nullptr);
    // И перекодировка сэмпла: кодирование между окнами хоста шину не
    // обслуживает, а самый долгий шаг доходил до 4.5 с.
    soundsinth::model::pack_file_pcm_set_service(&session_service, nullptr);
    devices::sd::sd_card_set_wait_service(&serve_bus_while_card_waits, nullptr);
    // То же для флешки: её сектор идёт миллисекунды, и всё это время машина
    // ждёт ответа на шине.
    //
    // Стек USB поднимается всегда, значит и обслуживать его надо всегда:
    // виток берёт критическую секцию и гасит на этом ядре все прерывания,
    // в том числе шинные, и без этого крюка машина вставала бы.
    platform::usb_host_set_wait_service(&serve_bus_and_card_while_usb_waits, nullptr);

    debug_log("session_orchestrator_run: entering event loop\n");

    player::hal::host_release_reset();
    debug_log("session_orchestrator_run: host reset released\n");

    uint32_t last_position_us = platform::mono_us();
    // Прошлые значения монотонных счётчиков нагрузки: период усреднения
    // задаёт этот цикл.
    EngineLoadWatch load_watch = engine_load_watch_now(last_position_us);
    // true - Ended для текущего трека уже выставлен; кадр позиции уходит хосту
    // по изменению, повторять set_position() не нужно.
    bool ended_notified = false;

    for (;;) {
        // Плеер GS начал загрузку модуля: план и фоновая догрузка прошлого
        // трека закрываются, иначе они вытесняли бы и грузили по чужому плану
        // поверх модуля.
        if ((orch.playing || orch.plan.plan_count != 0) && gs::bridge_wants_track_memory()) {
            log_track_taken_by_gs(orch);
            close_session_source(orch);
            orch.playing = false;
        }
        // Живой режим просит память трека: режет её begin() на Core0, а
        // наполняет фоновая догрузка здесь. Пока сеанс не закрыт и план не
        // погашен, reset шёл бы под выделяющим - список свободных страниц
        // пересобирался бы под чужой рукой. Подтверждение - разрешение
        // входить.
        if (player::live::wants_track_memory() && !player::live::track_memory_granted()) {
            if (orch.playing) {
                log_track_taken_by_live(orch);
                close_session_source(orch);
                orch.playing = false;
            }
            player::live::grant_track_memory();
        }
        // То же при входе в живой режим: он снял файловый трек, и тот не
        // вернётся. Сеанс закрывается сразу, пока кадры ещё сравнимы с
        // длительностью файла: после выхода g_playback_frames замирает на
        // значении живого движка, и конец либо не наступает никогда, либо
        // приходит с чужим временем.
        if (orch.playing && player::live::session_active()) {
            const uint32_t played = shared::g_playback_frames.load(std::memory_order_relaxed);
            if (!ended_notified) {
                const MinSec t = to_min_sec(played);
                orch.protocol.set_position(t.minutes, t.seconds, player::protocol::PlaybackState::Ended);
                ended_notified = true;
                log_track_ended(orch, played, false);
            }
            orch.playing                  = false;
            orch.load_result.total_frames = 0;
        }
        // Эмуляция GS: разобрать накопленный модуль. Здесь, а не в прерывании:
        // снос прежнего движка - десятки миллисекунд, разбор - единицы.
        shared::g_core1_loops.store(shared::g_core1_loops.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        shared::g_core1_phase.store(shared::Core1Phase::Loop, std::memory_order_relaxed);
        shared::g_core1_step.store(shared::Core1Step::GsPoll, std::memory_order_relaxed);
        gs::bridge_poll();
        const uint32_t now = platform::mono_us();
        shared::g_core1_step.store(shared::Core1Step::Watch, std::memory_order_relaxed);
        watch_core0(now);
        watch_playback(orch, now);
        shared::g_core1_step.store(shared::Core1Step::HostService, std::memory_order_relaxed);
        player::hal::host_service();
        // Пока не вызвано, хост на чтении сектора ждёт, поэтому - в самом частом
        // месте цикла. Во всех сборках: карту читает и DivMMC.
        shared::g_core1_step.store(shared::Core1Step::SdTask, std::memory_order_relaxed);
        devices::sd::sd_spi_task();
        // Виток USB-хоста: другого планировщика на этом ядре нет. Стек
        // разбирает уже принятое и возвращается; на ПК это пустышка.
        shared::g_core1_step.store(shared::Core1Step::UsbTask, std::memory_order_relaxed);
        platform::usb_host_task();
        // Управление от хоста: прерывание его только накопило.
        switch (orch.protocol.take_transport_request()) {
            case player::protocol::TransportOp::PauseToggle: {
                const bool on = !shared::g_playback_paused.load(std::memory_order_relaxed);
                shared::g_playback_paused.store(on, std::memory_order_relaxed);
                debug_logf("transport: pause %s\n", on ? "on" : "off");
                break;
            }
            case player::protocol::TransportOp::SeekForward:
                // Копится: нажали дважды - уехали вдвое дальше. Исполняет
                // Core0, когда выход погаснет.
                if (orch.playing) {
                    shared::g_seek_frames.fetch_add(kSeekStepFrames, std::memory_order_relaxed);
                }
                break;
            // Назад движок не умеет: секвенсор к нулю не возвращается.
            default:
                break;
        }
        // Конфигуратор настроек: правка значений и выход. Здесь, а не в
        // прерывании - пересчёт трогает настройки целиком.
        serve_configurator(orch);

        if (orch.playing) {
            // Loading выставлен при начале сессии; здесь Playing раз в секунду и
            // разово Ended. Конец - g_song_ended (вырожденный файл) или отыграна
            // длительность прохода: без второго трек крутится по кругу, плагин не
            // уходит дальше.
            //
            // В живом режиме конца не бывает: длина осталась от снесённого
            // файла, а кадры считает живой движок - сравнивать не с чем.
            // Без проверки live::session_active() сеанс глохнет навсегда: признак
            // снимает только загрузка следующего трека.
            const uint32_t played = shared::g_playback_frames.load(std::memory_order_relaxed);
            const bool song_ended = shared::g_song_ended.load(std::memory_order_relaxed);
            const bool finished =
                !player::live::session_active() && (song_ended || (orch.load_result.total_frames > 0 && played >= orch.load_result.total_frames));
            if (finished) {
                // Сначала заглушить выход, потом сообщить: секвенсор уже ушёл на
                // restart_position и играет трек второй раз, начало слышно поверх
                // затихающего конца, пока хост не закроется.
                shared::g_playback_finished.store(true, std::memory_order_relaxed);
                if (!ended_notified) {
                    const MinSec t = to_min_sec(played);
                    orch.protocol.set_position(t.minutes, t.seconds, player::protocol::PlaybackState::Ended);
                    ended_notified = true;
                    log_track_ended(orch, played, song_ended);
                }
            } else if (now - last_position_us >= kPositionIntervalUs) {
                const MinSec t = to_min_sec(shared::g_playback_frames.load(std::memory_order_relaxed));
                // Перемотка есть у всего, что играет с паттернами; у живого
                // MIDI её нет - строки приходят из потока, отматывать нечего.
                uint8_t flags = player::live::session_active() ? 0u : player::protocol::kStateSeekable;
                if (shared::g_playback_paused.load(std::memory_order_relaxed)) flags |= player::protocol::kStatePaused;
                orch.protocol.set_position(t.minutes, t.seconds, player::protocol::PlaybackState::Playing, flags);
                last_position_us = now;
            }

            update_engine_load(orch, load_watch, now);
        }

        if (!orch.session_pending) {
            // Фоновая догрузка - один сэмпл за проход цикла: между сэмплами шина
            // обслуживается, next/prev/ESC работают. Сэмплы файла идут только от
            // хоста, поэтому вне плеера шаг не начинается; сэмплы .mid - из банка
            // на карте, и догрузка идёт без хоста.
            const bool from_host = orch.load_result.format != player::load::TrackFormat::Midi;
            const bool host_here = platform::mono_us() - player::hal::host_last_seen_us() < kHostGoneUs;
            if (orch.playing && from_host && host_here != orch.host_here) {
                orch.host_here = host_here;
                log_host_presence(host_here);
            }
            // Живая подгрузка - перед плановой: её ждёт нота, которая уже
            // выписана в строку, а план грузит впрок.
            shared::g_core1_step.store(shared::Core1Step::LiveServe, std::memory_order_relaxed);
            const bool live_loaded = player::live::serve_requests();
            const bool loaded      = live_loaded || (orch.playing && (host_here || !from_host) && load_next_planned_sample(orch));
            shared::g_core1_step.store(shared::Core1Step::IdleWait, std::memory_order_relaxed);
            if (!loaded) player::hal::idle_wait(kSleepMaxUs);
            continue;
        }
        // Новая сессия: состояние прошлого трека не должно перенестись.
        // g_song_ended и g_playback_finished сбрасывает track_load_begin.
        ended_notified = false;
        shared::g_core1_step.store(shared::Core1Step::RunSession, std::memory_order_relaxed);
        if (run_session(orch)) last_position_us = platform::mono_us();
    }
}

} // namespace player
