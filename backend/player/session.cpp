// SPDX-License-Identifier: MIT
// Логика плеера на Core1: сессии хоста, загрузка трека по фазам, позиция.

#include "player/session.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new> // размещающий new источника сессии

#include "platform/boot_mode.h"
#include "platform/compiler.h"
#include "platform/config_flash.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/log_rings.h"
#include "platform/mono_time.h"
#include "platform/usb_host.h"

#include "core/codec/pack_file_pcm.h"
#include "core/config/config_store.h"
#include "core/engine/engine_defs.h"
#include "core/engine/song_duration.h"
#include "core/formats/load_stats.h"
#include "core/formats/midi.h"

#include "devices/bdi/bdi.h" // bdi_poll
#include "devices/sd/card_protocol.h"
#include "devices/sd/spi_emu.h"
#include "devices/storage/storage.h"
#include "devices/storage/volumes.h"
#include "devices/zcontroller/zcontroller.h"

#include "player/config/config_page.h"
#include "player/config/config_service.h"      // нужен ли USB-хост
#include "player/nav/nav.h"                    // nav_poll
#include "player/config/config_term_service.h" // меню конфигуратора на плате
#include "player/config/tools_service.h"       // раздел TOOLS конфигуратора
#include "player/config.h"
#include "player/gs/bridge.h"
#include "player/hal/host_link.h"
#include "player/protocol/track_meta.h"
#include "player/live/session.h"
#include "player/shared_state.h"
#include "player/vgm/vgm_track.h"

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
constexpr uint32_t kEndFadeFrames        = SOUNDSINTH_END_FADE_SECONDS * kSampleRateHz;

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

// Оборот цикла Core1 или его ожидания: по g_core1_loops журнал Core0
// отличает занятый Core1 от стоящего.
SOUNDSINTH_ALWAYS_INLINE void count_core1_loop() {
    shared::g_core1_loops.store(shared::g_core1_loops.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
}

// Связь обслуживает hostlink, здесь то, что про трек: загрузка, план сэмплов,
// позиция. serve_and_wait уходит в BusByteSource и track_load_begin: пока
// загрузка ждёт данных, шина и карта обслуживаются.
//
// Строки журнала - в функциях SOUNDSINTH_NOINLINE: кадры run_session и
// session_orchestrator_run лежат под run_session_load, самым глубоким путём
// стека Core1 (4 КБ), и буферам печати в них не место.

// --- Отчёт об отказе загрузки ---
//
// Собирается целиком со счётчиками и печатается одним куском. Счёт отказов
// с запуска отличает разовый отказ от повторяющихся.
uint32_t s_load_failures = 0;

// Буфер отчёта 640 байт: звать только с верхнего уровня цикла, где загрузка
// уже вернулась, - не из загрузчика и не из serve_without_wait.
SOUNDSINTH_NOINLINE void report_load_failure(SessionOrchestrator& orch) {
    ++s_load_failures;
    // Построчно, без общего буфера на стеке Core1: строки отчёта могут
    // перемежаться строками Core0.
    debug_logf("LAST FAILURE #%" PRIu32 " at %" PRIu32 " ms: len=%" PRIu32 " error=%s\n", s_load_failures, platform::mono_ms(), orch.pending_file_length,
               orch.track.load_result.error ? orch.track.load_result.error : "(null)");
    // Причина каждого загрузчика, а не только последнего: иначе на .it
    // с сигнатурой IMPM в логе "unknown or unsupported MOD signature". У
    // журнала VGM загрузчики песен не пробуются, и строк нет.
    for (uint8_t i = 0; i < load::kLoaderCount; ++i) {
        const char* why = orch.track.load_result.attempt_errors[i];
        if (why != nullptr) debug_logf("  %-3s: %s\n", load::session_loader_name(i), why);
    }
    // Счётчики протокола при провале - за эту сессию: короткое чтение
    // бывает, когда протокол исчерпал попытки, здесь видно, так ли это,
    // а сбои прошлых треков причиной не выглядят. Двумя строками: одной
    // в буфер debug_logf (192 Б) не влезает.
    const protocol::HostProtocol::Counters c = orch.protocol.session_counters();
    debug_logf("  protocol (session): not understood %" PRIu32 ", gave up %" PRIu32 ", unknown %" PRIu32 ", lost byte %" PRIu32 "\n", c.naks, c.nak_giveups,
               c.unknown, c.data_short);
    debug_logf("  protocol (session): short done %" PRIu32 ", arg/data/done overflows %" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n", c.short_done, c.arg_overflow,
               c.data_overflow, c.done_overflow);
}

// Снять источник данных сессии: перед новой сессией и после провала или
// сброса. on_reset больше некуда передавать, фоновая догрузка
// останавливается (иначе следующий track_memory_reset_for_new_track
// разрушит то, во что она пишет).
// Разовая, на смену трека: во флеше, а не встроенной в цикл.
SOUNDSINTH_NOINLINE void close_session_source(SessionOrchestrator& orch) {
    if (orch.current_bus_source != nullptr) {
        orch.current_bus_source->~BusByteSource();
        orch.current_bus_source = nullptr;
    }
    orch.track = SessionOrchestrator::Track{};
}

// Отдать хосту прогресс загрузки сэмплов (кадр kStPsramStats): всего, в очереди,
// загружено. Отдельный кадр, а не бит в байте состояния: состояние
// воспроизведения и прогресс загрузки - разные величины.
void publish_load_progress(SessionOrchestrator& orch) {
    const uint16_t left = static_cast<uint16_t>(orch.track.plan.plan_count - orch.track.plan.plan_next);
    orch.protocol.set_psram_stats(orch.track.plan.plan_count, left, orch.track.plan.plan_next);
}

// Загрузка сэмпла idx из источника сессии - для progressive_load_next.
bool load_planned_sample(void* user, uint16_t idx, const char** reason_out) {
    auto& orch = *static_cast<SessionOrchestrator*>(user);
    // Флаг только для диагностики: попадают ли заминки ровно в окно
    // загрузки. Вокруг самой загрузки: шаг Waiting (при тесной памяти у .mid
    // - на каждом проходе цикла) ничего не грузит.
    shared::g_background_loading.store(true, std::memory_order_relaxed);
    const bool ok = load::load_track_sample(orch.track.load_result.format, orch.current_bus_source->as_byte_source(), shared::g_track_memory, shared::g_song,
                                            idx, reason_out);
    shared::g_background_loading.store(false, std::memory_order_relaxed);
    return ok;
}

// Провал - не ошибка трека (кончилась PSRAM, каталог, файл обрезан), но в
// логе должен быть: иначе незагруженный сэмпл виден только по молчащей
// ноте. Числа, а не слова: "PSRAM exhausted" при тысячах свободных страниц
// - другая причина, чем при нуле.
SOUNDSINTH_NOINLINE void log_load_failure(const SessionOrchestrator& orch, uint16_t idx, const char* reason) {
    const uint32_t slots_used = soundsinth::memory::sample_cache_used_count(shared::g_track_memory.sample_cache);
    char m[320];
    // Прервано - не отказ, а смена трека посреди догрузки: источник закрыт,
    // чтение короткое.
    std::snprintf(m, sizeof(m),
                  "sample %u NOT loaded (failure #%u%s): %s [pages free %" PRIu32 "/%" PRIu32 ", slots used %" PRIu32 "/%u, window missed %" PRIu32 "]\n", idx,
                  orch.track.plan.plan_failed, orch.current_bus_source->aborted() ? ", ABORTED by track change" : "", reason ? reason : "?",
                  soundsinth::memory::psram_free_page_count(shared::g_track_memory.psram),
                  // Фактическое число страниц, а не константа: граница зон подвижная,
                  // по константе свободных выходит больше, чем всего.
                  soundsinth::memory::psram_sample_page_count(shared::g_track_memory.psram), slots_used, soundsinth::memory::kSampleCacheCatalogCapacity,
                  orch.current_bus_source->window_missed());
    debug_log(m);
}

// Потери разбора трекерного файла; отказы сэмплов печатает шаг плана
// загрузки (log_load_failure).
SOUNDSINTH_NOINLINE void log_tracker_load_stats() {
    const auto& ls = soundsinth::model::g_tracker_load_stats;
    char m[224];
    std::snprintf(m, sizeof(m), "load: undecodable %u, dropped %u, sustain loops %u, autovibrato %u, cells unparsed %" PRIu32 "\n", ls.samples_nonresident,
                  ls.samples_dropped, ls.sustain_loops, ls.autovibrato, ls.effect_cells_dropped);
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
    char m[256];
    std::snprintf(m, sizeof(m), "samples NOT loaded: %" PRIu32 " of %u, first %u: %s\n", f.count, total, f.first, f.first_reason ? f.first_reason : "?");
    debug_log(m);
}

// Первое вытеснение и каждое kEvictLogStep-е: без строки в логе не отличить
// "работает" от "ни разу не сработало".
SOUNDSINTH_NOINLINE void log_evictions(const SessionOrchestrator& orch, uint16_t pos) {
    char m[160];
    const soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    std::snprintf(m, sizeof(m), "evict: total %u at order pos %u, reloaded %u, past the map cap %u, pages free %" PRIu32 "%s\n", orch.track.plan.evicted, pos,
                  orch.track.plan.reloaded, orch.track.plan.evict_capped, soundsinth::memory::psram_free_page_count(psram),
                  psram.free_list_broken ? ", FREE LIST CORRUPTED" : "");
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
               orch.track.load_result.total_frames, shared::g_song_generation.load(std::memory_order_relaxed),
               shared::g_engine_generation.load(std::memory_order_relaxed), shared::g_engine_alive.load(std::memory_order_relaxed),
               shared::g_load_in_progress.load(std::memory_order_relaxed));
    w.playback_stalled = true;
}

// Сбои связи за сессию (приросты с kHcStart). Печатается и при нуле:
// "сбоев не было" отличается от "диагностика выключена". Раз на сессию: на
// конце плана, в отказе или, если сессию сменили раньше, на старте
// следующей.
SOUNDSINTH_NOINLINE void log_session_link(SessionOrchestrator& orch, const protocol::HostProtocol::Counters& c, const char* when) {
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
    char m[256];
    const soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    std::snprintf(m, sizeof(m),
                  "background prefetch done: %u samples, %u failures, evicted %u, pages free %" PRIu32 "/%" PRIu32 "%s, longest step %" PRIu32
                  " ms (sample %u), t=%" PRIu32 " ms\n",
                  orch.track.plan.plan_count, orch.track.plan.plan_failed, orch.track.plan.evicted, soundsinth::memory::psram_free_page_count(psram),
                  soundsinth::memory::psram_sample_page_count(psram), psram.free_list_broken ? ", FREE LIST CORRUPTED" : "", orch.track.longest_step_us / 1000u,
                  orch.track.longest_step_sample, platform::mono_ms());
    debug_log(m);
}

// Сон холостого витка: будит любое прерывание связи (порт, заказ сектора,
// байт протокола, GS) или срок - флаги другого ядра (позиция, конец трека)
// ждут не дольше.
constexpr uint32_t kSleepMaxUs = 1000;

// Во флеше, а не в цикле Core1: в SRAM выбор строки стоил бы 12 байт.
SOUNDSINTH_NOINLINE void log_host_presence(bool here) {
    debug_log(here ? "prefetch: host is back, running\n" : "prefetch: waiting for the host (host outside the player)\n");
}

// Шаг повторного прохода по плану - не чаще: 4 записи плана за шаг, план
// до 810 записей - полный круг около 4 с, а упреждение у .mid 10 с.
constexpr uint32_t kRescanIntervalUs = 20000;

// Шаг фоновой догрузки (логика в ядре). false - ждать позицию, план кончился
// или источник закрыт: на false выходит цикл префетча.
// prefetch - фаза C, звука ещё нет: вытеснения нет никогда. Кадры
// g_playback_frames в это время - прошлого трека (обнуляет их публикация
// нового), и запрет по ним зависел бы от длины предыдущей песни.
bool SOUNDSINTH_HOT_PATH(load_next_planned_sample)(SessionOrchestrator& orch, bool prefetch = false) {
    load::ProgressiveLoader& pl = orch.track.plan;
    if (pl.plan_count == 0 || orch.current_bus_source == nullptr) return false;
    // Сессию бросили: RESET хоста или сброс машины (ожидание окна оборвано).
    if (orch.reset_by_host || orch.current_bus_source->aborted()) return false;
    // Очередь пройдена - дальше только повторный проход. Не было ни
    // вытеснения, ни отказа - возвращать нечего, и шаг кончается здесь, в
    // SRAM, без захода в загрузчик во флеше. Иначе - не чаще раза в
    // kRescanIntervalUs: на витке Core1 шаг иначе крутился бы постоянно.
    if (pl.plan_next >= pl.plan_count) {
        if (pl.evicted == 0 && pl.plan_failed == 0) return false;
        const uint32_t now_us = platform::mono_us();
        if (now_us - orch.track.last_rescan_us < kRescanIntervalUs) return false;
        orch.track.last_rescan_us = now_us;
    }

    const uint16_t pos = shared::g_order_pos.load(std::memory_order_acquire);
    pl.no_eviction     = prefetch || load::progressive_eviction_blocked(orch.track.load_result.duration.position_goes_back, orch.track.load_result.total_frames,
                                                                        shared::g_playback_frames.load(std::memory_order_relaxed));
    const load::SamplesInUse in_use{shared::g_samples_in_use, shared::kSamplesInUseBits};
    const uint16_t evicted_before    = pl.evicted;
    const bool queue_open            = pl.plan_next < pl.plan_count;
    uint16_t idx                     = 0;
    const char* reason               = nullptr;
    const uint32_t step_started_us   = platform::mono_us();
    const load::ProgressiveStep step = load::progressive_load_next(pl, shared::g_track_memory, pos, in_use, &load_planned_sample, &orch, &idx, &reason);
    // Весь шаг - один оборот внешнего цикла: всё это время стоят проверка
    // "доиграл" и позиция хосту.
    const uint32_t step_us = platform::mono_us() - step_started_us;
    if (step_us > orch.track.longest_step_us) {
        orch.track.longest_step_us     = step_us;
        orch.track.longest_step_sample = idx;
    }
    if (step == load::ProgressiveStep::Waiting || step == load::ProgressiveStep::Done) {
        return false;
    }

    publish_load_progress(orch);
    if (pl.evicted != evicted_before && (evicted_before == 0 || pl.evicted / kEvictLogStep != evicted_before / kEvictLogStep)) {
        log_evictions(orch, pos);
    }
    if (step == load::ProgressiveStep::Failed) log_load_failure(orch, idx, reason);
    // Конец очереди плана - один раз, на шаге, который взял её последнюю
    // запись. Дальше шаги - повторный проход.
    if (queue_open && pl.plan_next >= pl.plan_count) finish_plan(orch);
    return true;
}

// Метаданные трека хосту (0x1B) - после 0x15. Кадры собираются из файла при
// выставлении из журнала VGM или из g_song: s_track_meta держит метку,
// класс, версию и указатели.
protocol::TrackMeta s_track_meta;

SOUNDSINTH_NOINLINE void publish_track_meta(SessionOrchestrator& orch, load::TrackFormat format, const vgm::Player* track) {
    // По порядку load::TrackFormat.
    static constexpr char kTags[][4] = {{}, {'M', 'I', 'D'}, {'I', 'T'}, {'X', 'M'}, {'S', '3', 'M'}, {'M', 'O', 'D'}, {'V', 'G', 'M'}};
    static_assert(sizeof(kTags) / sizeof(kTags[0]) == static_cast<uint8_t>(load::TrackFormat::Vgm) + 1u, "a tag for every track format");
    std::memcpy(s_track_meta.format, kTags[static_cast<uint8_t>(format)], sizeof(s_track_meta.format));
    s_track_meta.klass   = format == load::TrackFormat::Vgm    ? protocol::kClassChip
                           : format == load::TrackFormat::Midi ? protocol::kClassMidi
                                                               : protocol::kClassTracker;
    s_track_meta.version = track != nullptr ? static_cast<uint16_t>(track->info().version) : shared::g_song.format_version;
    s_track_meta.vgm     = track;
    s_track_meta.song    = track != nullptr ? nullptr : &shared::g_song;
    orch.protocol.set_track_meta(&protocol::track_meta_frame, &s_track_meta);
}

// VU и спектр, посчитанные другим ядром, - в кадры телеметрии; уходят те, что
// изменились и заказаны хостом.
void publish_meters(SessionOrchestrator& orch) {
    const uint32_t vu = shared::g_vu.load(std::memory_order_relaxed);
    orch.protocol.set_vu(static_cast<uint8_t>(vu), static_cast<uint8_t>(vu >> 8));
    uint8_t bands[16];
    for (uint32_t w = 0; w < 4; ++w) {
        const uint32_t v = shared::g_spectrum[w].load(std::memory_order_relaxed);
        for (uint32_t b = 0; b < 4; ++b) {
            bands[4 * w + b] = static_cast<uint8_t>(v >> (8 * b));
        }
    }
    orch.protocol.set_spectrum(bands);
}

// Позиция хосту раз в секунду. Зовётся и из цикла сеанса, и из ожидания
// данных на шаге догрузки: сэмпл грузится окнами подряд, иногда секундами,
// и часы хоста перескакивали секунду.
void pump_position(SessionOrchestrator& orch, uint32_t now) {
    if (now - orch.last_position_us < kPositionIntervalUs) return;
    const MinSec t = to_min_sec(shared::g_playback_frames.load(std::memory_order_relaxed));
    // Перемотка вперёд есть у всего, что играет из файла; у живого MIDI её
    // нет - строки приходят из потока, отматывать нечего.
    const bool seekable = !live::session_active();
    uint8_t flags       = seekable ? protocol::kStateSeekable : 0u;
    if (shared::g_playback_paused.load(std::memory_order_relaxed)) flags |= protocol::kStatePaused;
    orch.protocol.set_position(t.minutes, t.seconds, protocol::PlaybackState::Playing, flags);
    orch.last_position_us = now;
}

// VU и спектр хосту, как только Core0 их обновил: свой таймер добавил бы до
// шага задержки. Зовётся и из цикла сеанса, и из ожидания данных: догрузка
// сэмпла идёт окнами подряд, иногда секундами.
void pump_meters(SessionOrchestrator& orch) {
    const uint32_t seq = shared::g_meter_seq.load(std::memory_order_acquire);
    if (seq == orch.meter_seq || !shared::g_meter_wanted.load(std::memory_order_relaxed)) return;
    orch.meter_seq = seq;
    publish_meters(orch);
}

// Обслуживание карты и шины, пока загрузка ждёт данных от хоста. user -
// SessionOrchestrator.
//
// Карта обслуживается первой и во всех сборках: на машине без своего
// интерфейса хост читает файл с нашей карты, и без обслуживания выходит
// круговое ожидание - Z80 ждёт сектор, плата ждёт Z80.
void SOUNDSINTH_HOT_PATH(serve_and_wait)(void* user) {
    count_core1_loop();
    devices::sd::sd_spi_task();
    hal::host_service_and_wait();
    watch_core0(platform::mono_us());
    platform::usb_host_task_in_wait();
    devices::zcontroller::zcontroller_tick();
    auto& orch = *static_cast<SessionOrchestrator*>(user);
    pump_meters(orch);
    if (orch.background_load) pump_position(orch, platform::mono_us());
}

// Арбитр носителя пропускает хост вперёд фоновых чтений (банк .mid):
// заказы хоста забирает эмулятор карты.
void SOUNDSINTH_HOT_PATH(yield_storage_to_host)(void*) {
    devices::sd::sd_spi_task();
}

// Обслуживание из ожиданий драйвера карты: только шина. Эмулятор карты
// отсюда не двигается - он сам пошёл бы к карте, которая занята тем самым
// ожиданием, и получил бы отказ.
void SOUNDSINTH_HOT_PATH(serve_bus_while_card_waits)(void*) {
    count_core1_loop();
    hal::host_service();
}

// Ожидания внутри USB-стека - обмен с флешкой, при отказе устройства до
// 2 с: к шине добавляется цикл эмулятора карты. Без него заказ сектора от
// машины в это время не берёт никто, и esxDOS, прождав токен сорок тысяч
// опросов, срывается.
//
// Только для USB. Ожиданию самой карты этого давать нельзя: цикл
// эмулятора сам ходит к носителю, и вышла бы рекурсия в драйвер карты.
// В обратную сторону всё закрыто признаком внутри sd_spi_task: ожидание
// обмена с флешкой приходит сюда изнутри этого же цикла.
void SOUNDSINTH_HOT_PATH(serve_bus_and_card_while_usb_waits)(void* user) {
    serve_bus_while_card_waits(user);
    devices::sd::sd_spi_task();
}

SOUNDSINTH_HOT_PATH_ATTR("so_on_session_start")
void on_session_start(void* user, uint32_t, uint32_t file_length, uint8_t telemetry, uint8_t load_order) {
    auto* self                = static_cast<SessionOrchestrator*>(user);
    self->pending_file_length = file_length;
    self->telemetry           = telemetry;
    // Всё, кроме ByPlayback, - ByFile: хост без выбора стратегии шлёт ноль.
    self->pending_load_order = (load_order == static_cast<uint8_t>(load::LoadOrder::ByPlayback)) ? load::LoadOrder::ByPlayback : load::LoadOrder::ByFile;
    self->session_pending    = true;
}

SOUNDSINTH_HOT_PATH_ATTR("so_on_reset")
void on_reset(void* user) {
    auto* self          = static_cast<SessionOrchestrator*>(user);
    self->reset_by_host = true;
    self->playing       = false;
    // START и RESET в одном разборе: сессию, от которой хост ушёл, не
    // начинать. START после RESET взведёт её снова - колбэки идут по порядку
    // кадров.
    self->session_pending = false;
    if (self->current_bus_source) self->current_bus_source->on_reset();
    // Хост от трека ушёл. Безусловно, даже без следующей сессии (ESC тоже
    // глушит звук).
    shared::forget_built_track();
}

// Трек платы снят плеером GS или живым MIDI: план и фоновая догрузка
// закрываются, хост о закрытии не узнаёт (Ended не уходит) - без строки это
// в журнале не отличить от залипания. who - кто снял.
SOUNDSINTH_NOINLINE void drop_track(SessionOrchestrator& orch, const char* who) {
    debug_logf("session: board track dropped by %s, loaded %u of %u, t=%" PRIu32 " ms\n", who, orch.track.plan.plan_next, orch.track.plan.plan_count,
               platform::mono_ms());
    close_session_source(orch);
    orch.playing = false;
}

// Музыка без хоста остановлена (сброс машины или F10): трек и его
// догрузка закрыты, собранный, но не начатый - не собирается, движок с
// живым MIDI сносит Core0. Карта GS сброшена, если просили.
SOUNDSINTH_NOINLINE void stop_music(SessionOrchestrator& orch, bool gs_reset) {
    // F10 под живым MIDI ничего не делает: поток идёт с машины, и снос
    // движка только сбросил бы программы до конца пьесы. F10 - чтобы
    // выключить музыку платы после сброса при STOP_MUSIC_ON_RESET=0.
    if (!gs_reset && live::session_active()) {
        debug_log("session: F10 ignored - live MIDI is playing\n");
        return;
    }
    debug_logf("session: music stopped by %s, playing %u, t=%" PRIu32 " ms\n", gs_reset ? "the machine reset" : "F10", orch.playing ? 1u : 0u,
               platform::mono_ms());
    close_session_source(orch);
    orch.playing = false;
    if (gs_reset) {
        gs::bridge_machine_reset();
    } else {
        gs::bridge_stop_effects();
    }
    shared::forget_built_track();
}

// Конец трека: Ended хосту и строка журнала. Почему кончился - с числами.
// "Загрузился и сразу уехал на следующий" одинаково выглядит для двух
// причин: секвенсор объявил файл вырожденным, или длительность посчиталась
// нулём и played >= total сработало сразу.
SOUNDSINTH_NOINLINE void notify_ended(SessionOrchestrator& orch, uint32_t played, bool song_ended) {
    orch.track.ended_notified = true;
    const MinSec t            = to_min_sec(played);
    orch.protocol.set_position(t.minutes, t.seconds, protocol::PlaybackState::Ended);
    debug_logf("track ended: %s, played %" PRIu32 " s of %" PRIu32 " s, t=%" PRIu32 " ms\n", song_ended ? "sequencer declared the end" : "full duration played",
               played / kSampleRateHz, orch.track.load_result.total_frames / kSampleRateHz, platform::mono_ms());
    // Итог догрузки за трек: строка evict: идёт только на каждом 64-м
    // вытеснении.
    const load::ProgressiveLoader& pl = orch.track.plan;
    if (pl.plan_count != 0) {
        debug_logf("plan: evicted %u, reloaded %u, rescan failed %u, failed %u, capped %u\n", pl.evicted, pl.reloaded, pl.rescan_failed, pl.plan_failed,
                   pl.evict_capped);
    }
}

SOUNDSINTH_NOINLINE void log_duration(const SessionOrchestrator& orch, uint32_t phase_a_ms) {
    const auto& d = orch.track.load_result.duration;
    debug_logf("duration: %" PRIu32 " ms, rows %" PRIu32 ", ticks %" PRIu32 ", stop=%s (%u,%u)%s, phase A %" PRIu32 " ms\n",
               static_cast<uint32_t>(static_cast<uint64_t>(orch.track.load_result.total_frames) * 1000u / kSampleRateHz), d.rows, d.ticks,
               soundsinth::engine::duration_stop_name(d.stop), d.stop_order_pos, d.stop_row, d.position_goes_back ? ", backwards" : "", phase_a_ms);
}

// Числа загрузки .mid: загрузчик сам не печатает (printf с Core1 посреди
// обмена по шине).
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
    // map - байт карты позиций; 0 - карты нет, вытеснение по last_use.
    const load::UseMap& m = orch.track.plan.use_map;
    debug_logf("prefetch: %u of %u samples, %u order pos, %" PRIu32 " ms, failed %d, teardown waited %" PRIu32 " ms, plan %" PRIu32 " ms, map %" PRIu32
               " B, t=%" PRIu32 " ms\n",
               r.samples, orch.track.plan.plan_count, r.positions, r.prefetch_ms, r.failed, r.teardown_ms, r.plan_ms,
               m.words != nullptr ? load::use_map_bytes(m) : 0u, platform::mono_ms());
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
    // перебором: до SOUNDSINTH_MAX_VOICES итераций за период.
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

// Шаги сессии встроены в run_session (SOUNDSINTH_ALWAYS_INLINE): под
// phase_a_parse лежит run_session_load - самый глубокий путь стека Core1,
// лишний кадр между ними нежелателен.

// Фаза A - метаданные без PCM и план. Буфер загрузчика к этому моменту
// свободен. false - файл не распознан или битый.
SOUNDSINTH_ALWAYS_INLINE bool phase_a_parse(SessionOrchestrator& orch) {
    load::progressive_attach_plan(orch.track.plan, reinterpret_cast<uint16_t*>(shared::g_track_memory.loader_scratch_buffer));
    const uint32_t started_us = platform::mono_us();
    const bool ok = load::run_session_load(orch.current_bus_source->as_byte_source(), shared::g_track_memory, shared::g_song, orch.track.load_result,
                                           /*metadata_only=*/true, load::plan_arrays_of(orch.track.plan), &orch.track.plan.use_map);
    if (!ok) return false;
    // Затухание перед концом: последние секунды, у короткого трека - не
    // больше четверти. Длина не продлевается: возвратом к пройденной строке
    // кончается почти любая песня, и за концом звучало бы её начало.
    if (const uint32_t total = orch.track.load_result.total_frames; total != 0) {
        orch.end_fade = total / 4u < kEndFadeFrames ? total / 4u : kEndFadeFrames;
    }
    publish_file_info(orch.track.load_result.total_frames, shared::g_song);
    publish_track_meta(orch, orch.track.load_result.format, nullptr);
    log_duration(orch, (platform::mono_us() - started_us) / 1000u);
    if (orch.track.load_result.format == load::TrackFormat::Midi) {
        log_midi_stats();
    } else {
        log_tracker_load_stats();
    }
    return true;
}

// Файл сеанса - журнал VGM или его обёртка gzip, по первым байтам.
bool session_is_vgm(SessionOrchestrator& orch) {
    const soundsinth::formats::ByteSource src = orch.current_bus_source->as_byte_source();
    uint8_t head[4]                           = {};
    return src.seek(src.self, 0) && src.read(src.self, head, sizeof(head)) == sizeof(head) && vgm::sniff(head, sizeof(head));
}

SOUNDSINTH_NOINLINE void log_vgm(const vgm::Player& track, uint32_t file_bytes, uint32_t load_ms) {
    const vgm::Info& info = track.info();
    debug_logf("vgm: version %x.%02x, file %" PRIu32 " B, %" PRIu32 " s%s, chips %" PRIu32 ", chip memory %u B, arena %u B, loaded in %" PRIu32 " ms\n",
               static_cast<unsigned>(info.version >> 8), static_cast<unsigned>(info.version & 0xffu), file_bytes, info.total_samples / vgm::kRate,
               info.loop_start != 0 ? " with a loop" : "", info.count, static_cast<unsigned>(vgm::Player::memory_need(info)),
               static_cast<unsigned>(soundsinth::memory::arena_used(shared::g_track_memory.resident)), load_ms);
    for (uint32_t i = 0; i < info.count; ++i) {
        debug_logf("vgm: chip %s%s at %" PRIu32 " Hz, volume %u/256, SSG %u/256\n", vgm::chip_name(info.chips[i].kind), info.chips[i].second ? " (second)" : "",
                   info.chips[i].clock, static_cast<unsigned>(info.chips[i].volume), static_cast<unsigned>(info.chips[i].ssg_volume));
    }
    if (info.unsupported != nullptr) debug_logf("vgm: %s is in the file and stays silent\n", info.unsupported);
    // Теги: трек, игра, система. Строка журнала - до 192 байт.
    char tag[3][48];
    const bool named = track.tag(0, tag[0], sizeof(tag[0])) | track.tag(2, tag[1], sizeof(tag[1])) | track.tag(4, tag[2], sizeof(tag[2]));
    if (named) debug_logf("vgm: \"%s\" - %s (%s)\n", tag[0], tag[1], tag[2]);
}

// Ударные YM2608 - образ ПЗУ чипа, 8 КБ: файл YM2608.ROM в корне карты
// (регистр имени FatFs не различает), в арену трека. Его в прошивке нет:
// это код Yamaha. Том и файл - во временном буфере трека; том снимается,
// только если его смонтировали здесь.
const uint8_t* ym2608_rom_from_card(soundsinth::memory::TrackMemory& mem) {
    namespace st     = devices::storage;
    namespace memory = soundsinth::memory;
    struct Work {
        FATFS fs;
        FIL file;
    };
    if (!st::volume_present(st::kVolumeCard)) return nullptr;
    auto* const w = reinterpret_cast<Work*>(memory::scratch_take(mem.scratch, memory::Scratch::RhythmRom, sizeof(Work)));
    if (w == nullptr) return nullptr;
    const bool mounted = st::volume_mounted(st::kVolumeCard);
    uint8_t* rom       = nullptr;
    if (st::volume_attach(st::kVolumeCard, &w->fs)) {
        if (f_open(&w->file, "0:/YM2608.ROM", FA_READ) == FR_OK) {
            UINT got = 0;
            if (f_size(&w->file) == vgm::kRhythmRomBytes) rom = static_cast<uint8_t*>(memory::arena_alloc(mem.resident, vgm::kRhythmRomBytes, 4));
            if (rom != nullptr && (f_read(&w->file, rom, vgm::kRhythmRomBytes, &got) != FR_OK || got != vgm::kRhythmRomBytes)) rom = nullptr;
            f_close(&w->file);
        }
        if (!mounted) st::volume_detach(st::kVolumeCard);
    }
    memory::scratch_leave(mem.scratch, memory::Scratch::RhythmRom);
    debug_logf("vgm: YM2608 rhythm ROM %s\n", rom != nullptr ? "from the card" : "not on the card (YM2608.ROM, 8192 bytes) - the rhythm is silent");
    return rom;
}

// Журнал VGM вместо песни: файл целиком в PSRAM трека, плеер и чипы в арене.
// Сэмплов и плана догрузки нет. nullptr - отказ, причина в load_result.
SOUNDSINTH_NOINLINE vgm::Player* load_vgm(SessionOrchestrator& orch) {
    orch.track.load_result    = load::SessionLoadResult{};
    const uint32_t started_us = platform::mono_us();
    vgm::Player* const track  = vgm::load_track(orch.current_bus_source->as_byte_source(), shared::g_track_memory, &orch.track.load_result.error,
                                                &serve_without_wait, nullptr, &ym2608_rom_from_card);
    if (track == nullptr) return nullptr;
    orch.track.load_result.format       = load::TrackFormat::Vgm;
    orch.track.load_result.total_frames = vgm::track_frames(*track);
    orch.end_fade                       = vgm::track_fade_frames(*track);
    // Счётчики песни хосту - нули: паттернов и сэмплов у журнала нет.
    new (&shared::g_song) soundsinth::model::Song();
    publish_file_info(orch.track.load_result.total_frames, shared::g_song);
    publish_track_meta(orch, load::TrackFormat::Vgm, track);
    orch.protocol.set_psram_stats(0, 0, 0);
    log_vgm(*track, orch.pending_file_length, (platform::mono_us() - started_us) / 1000u);
    return track;
}

// Граница префетча у .mid по времени (SOUNDSINTH_MIDI_PREFETCH_SECONDS), а
// не по паттернам: их длина зависит от сетки и темпа.
SOUNDSINTH_ALWAYS_INLINE uint16_t prefetch_positions_for(SessionOrchestrator& orch) {
    uint16_t positions = load::kPrefetchOrderPositions;
#if SOUNDSINTH_MIDI_PREFETCH_SECONDS
    if (orch.track.load_result.format == load::TrackFormat::Midi) {
        // По кадрам начала паттернов, посчитанным при разборе: проход
        // секвенсора прогнал бы весь трек через конвертер заново.
        const uint16_t by_time = soundsinth::formats::midi::order_positions_in_frames(SOUNDSINTH_MIDI_PREFETCH_SECONDS * kSampleRateHz);
        if (by_time > positions) positions = by_time;
        // Это же упреждение фоновой догрузки. Только для .mid: у трекерных
        // подкачка не мерена. Замер на 114499 файлах при 200 КБ/с -
        // девяти из десяти хватает секунды упреждения и около мегабайта
        // памяти.
        orch.track.plan.lead_positions = by_time;
    }
#endif
    return positions;
}

// Фаза B - план, фаза C - префетч: сэмплы первых позиций, остальное фоном
// под звук. Брошенную по дороге сессию видит reset_by_host.
SOUNDSINTH_ALWAYS_INLINE void plan_and_prefetch(SessionOrchestrator& orch, uint32_t teardown_wait_ms) {
    // Проход секвенсора берёт буфер сценариев, свободный к этому моменту.
    const uint16_t prefetch_positions = prefetch_positions_for(orch);
    // Массивы плана - в loader_scratch_buffer: разбор кончился, до
    // close_session_source он принадлежит плану. Прицеплены ещё до
    // фазы A, которая план и собрала.
    const uint32_t plan_started_us = platform::mono_us();
    // План собран проходом длительности - остаётся поделить его на
    // префетч и хвост. Проход был неполным - плана нет, идём
    // обходом по order: он не смотрит переходы, зато не зависит от
    // того, докуда дошёл секвенсор.
    const load::PlaybackPlan plan =
        orch.track.load_result.plan_usable
            ? load::plan_split_prefetch(shared::g_song, orch.track.plan.plan_indices, orch.track.load_result.plan_count, orch.track.plan.plan_first_use,
                                        prefetch_positions, orch.pending_load_order)
            : load::plan_playback_order(shared::g_song, shared::g_track_memory.psram, orch.track.plan.plan_indices, load::kProgressiveMaxSamples,
                                        orch.track.plan.plan_last_use, orch.pending_load_order, prefetch_positions, orch.track.plan.plan_first_use);
    const uint32_t plan_ms     = (platform::mono_us() - plan_started_us) / 1000u;
    orch.track.plan.plan_count = plan.count;
    orch.track.plan.plan_next  = 0;
    load::progressive_plan_changed(orch.track.plan);
    // План есть - хосту можно сказать, сколько сэмплов всего и сколько в
    // очереди.
    publish_load_progress(orch);
    const uint32_t prefetch_started_us = platform::mono_us();
    const uint16_t failed_before       = orch.track.plan.plan_failed;
    while (orch.track.plan.plan_next < plan.prefetch_count && load_next_planned_sample(orch, /*prefetch=*/true)) {
    }
    log_prefetch(orch, PrefetchReport{plan.prefetch_count, prefetch_positions, (platform::mono_us() - prefetch_started_us) / 1000u,
                                      orch.track.plan.plan_failed - failed_before, teardown_wait_ms, plan_ms});
}

// Сэмплов больше, чем помещается в план, - грузится всё до старта звука,
// по индексам подряд и только PCM: повторный run_session_load заставил бы
// хост читать файл с начала. Такое бывает у XM (до 128 инструментов по 16
// сэмплов) и у .mid, где сэмплы идут из банка. Сброс хоста обрывает цикл.
SOUNDSINTH_ALWAYS_INLINE void load_all_unplanned(SessionOrchestrator& orch) {
    debug_logf("progressive loading disabled: samples %u > %u\n", shared::g_song.sample_count, load::kProgressiveMaxSamples);
    UnplannedFailures fails;
    for (uint16_t i = 0; i < shared::g_song.sample_count && !orch.reset_by_host; ++i) {
        const char* reason = nullptr;
        if (!load::load_track_sample(orch.track.load_result.format, orch.current_bus_source->as_byte_source(), shared::g_track_memory, shared::g_song, i,
                                     &reason)) {
            if (fails.count++ == 0) {
                fails.first        = i;
                fails.first_reason = reason;
            }
        }
    }
    if (fails.count != 0) log_unplanned_failures(fails, shared::g_song.sample_count);
}

// Неудача (нераспознанный или битый файл) или сброс посреди загрузки -
// сессия закрывается без SESSION_READY; об отказе хосту говорит состояние
// Failed в кадре позиции, причина - только в журнале. Прервано - не отказ, в
// отчёт не попадает.
SOUNDSINTH_ALWAYS_INLINE void close_failed_session(SessionOrchestrator& orch) {
    orch.playing = false;
    if (orch.reset_by_host || (orch.current_bus_source != nullptr && orch.current_bus_source->aborted())) {
        // Сброс посреди чтения закрывает источник, загрузчик получает
        // короткое чтение и говорит "file truncated", хотя файл целый.
        debug_logf("load interrupted by a track change (len=%" PRIu32 ") - not a failure\n", orch.pending_file_length);
        // end_session() не нужен: протокол уже сброшен RESET хоста.
        close_session_source(orch);
        return;
    }
    report_load_failure(orch);
    // Числа .mid и при отказе: по ним видно, где загрузка встала - на слоях,
    // на арене или на зоне паттернов. Признаки *_known говорят, докуда дошло.
    if (soundsinth::formats::midi::last_load_stats().layers_known) log_midi_stats();
    orch.link_reported = true;
    close_session_source(orch);
    orch.protocol.end_session();
    // После end_session(): он сбрасывает несданную телеметрию.
    orch.protocol.set_position(0, 0, protocol::PlaybackState::Failed);
}

// Новая сессия хоста: снос прошлого трека, фазы A-D, хвост - фоном. true -
// трек играет.
// Раз на трек и сама зовёт только флеш (разбор, загрузчик, снос движка):
// во флеше, а не встроенной в цикл Core1.
SOUNDSINTH_NOINLINE bool run_session(SessionOrchestrator& orch) {
    // Метаданные прошлого трека читают его файл: до загрузки нового - снять.
    orch.protocol.set_track_meta(nullptr, nullptr);
    orch.session_pending = false;
    orch.reset_by_host   = false;
    orch.end_fade        = 0;
    shared::g_meter_wanted.store((orch.telemetry & (protocol::HostProtocol::kTelemetryVu | protocol::HostProtocol::kTelemetrySpectrum)) != 0,
                                 std::memory_order_relaxed);
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
    shared::track_load_begin(&serve_and_wait, &orch);
    const uint32_t teardown_wait_ms = (platform::mono_us() - teardown_wait_started_us) / 1000u;
    shared::g_core1_phase.store(shared::Core1Phase::Load, std::memory_order_relaxed);
    // Трек через протокол платы - плееру GS мегабайт больше не нужен.
    gs::bridge_release_buffer();
    orch.current_bus_source = new (orch.bus_source_storage) load::BusByteSource(orch.protocol, orch.pending_file_length, &serve_and_wait, &orch);
    // Сброс, пришедший, пока track_load_begin ждал сноса, источника ещё не
    // застал: без этого фаза A ждала бы окна от ушедшего хоста до
    // следующего сброса.
    if (orch.reset_by_host) orch.current_bus_source->on_reset();

    vgm::Player* track = nullptr;
    bool ok            = false;
    if (session_is_vgm(orch)) {
        track = load_vgm(orch);
        ok    = track != nullptr;
    } else {
        ok = phase_a_parse(orch);
        if (ok && !orch.reset_by_host) {
            if (shared::g_song.sample_count <= load::kProgressiveMaxSamples) {
                plan_and_prefetch(orch, teardown_wait_ms);
            } else {
                load_all_unplanned(orch);
            }
        }
    }

    // Движок собирается только по смене поколения ниже: снятый запрет без
    // неё ничего не собирает, и при отказе тоже.
    shared::track_load_end();
    if (ok && !orch.reset_by_host) {
        // Фаза D - старт звука. Хвост плана догружается фоном во внешнем цикле.
        // Сброса кэша XIP нет: движок читает сэмплы тем же кэшем.
        shared::publish_loaded_track(orch.track.load_result.total_frames, track, orch.end_fade);
        orch.protocol.mark_session_ready();
        orch.playing = true;
        return true;
    }
    close_failed_session(orch);
    return false;
}

SOUNDSINTH_NOINLINE void reboot_from_configurator(protocol::ConfigExit exit) {
    platform::BootRequest req;
    req.configurator = exit == protocol::ConfigExit::Reenter;
    // Приглашение с карты снимает любой осознанный выход; простой сброс
    // запроса не оставляет, и машина попадёт в конфигуратор снова.
    req.drop_open_gui = true;
    debug_logf("config: exit %u, reboot\n", static_cast<unsigned>(exit));
    // Машину в сброс: после перезагрузки платы шины для неё нет.
    hal::host_hold_for_reboot();
    uint8_t* const page = platform::config_rom_page();
    if (exit == protocol::ConfigExit::Save && page != nullptr && platform::config_rom_page_bytes() >= soundsinth::config::kPageBytes) {
        // Блок собирается в странице конфигуратора: машина в сбросе и её
        // уже не читает.
        const soundsinth::config::NextWrite next = soundsinth::config::store_next(platform::config_flash_view(0), platform::config_flash_view(1));
        soundsinth::config::page_build(page, player::config::settings(), next.counter);
        platform::boot_reboot_saving(req, static_cast<uint32_t>(next.slot), page, soundsinth::config::kPageBytes);
    }
    platform::boot_reboot(req);
}

// Управление от хоста: прерывание его только накопило. Встроено в виток:
// опрос идёт на каждом проходе.
SOUNDSINTH_ALWAYS_INLINE void serve_transport(SessionOrchestrator& orch) {
    switch (orch.protocol.take_transport_request()) {
        case protocol::TransportOp::PauseToggle: {
            const bool on = !shared::g_playback_paused.load(std::memory_order_relaxed);
            shared::g_playback_paused.store(on, std::memory_order_relaxed);
            debug_logf("transport: pause %s\n", on ? "on" : "off");
            break;
        }
        case protocol::TransportOp::SeekForward:
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
}

// Режим конфигуратора: ветка одного режима - во флеше. В цикле Core1
// (ОЗУ) от неё одна проверка и один вызов: всякая правка здесь иначе
// перекладывает и раздувает сам цикл.
SOUNDSINTH_NOINLINE void serve_config_mode(SessionOrchestrator& orch, uint8_t* page) {
    player::config::tools_serve(page, shared::g_track_memory.scratch);
    // Меню на плате: выход выбран там же, а не командой ПЗУ.
    if (player::config::config_term_on()) {
        const soundsinth::config::ConfigUiExit term = player::config::config_term_serve(page);
        if (term == soundsinth::config::ConfigUiExit::Save) {
            player::config::settings_set(player::config::config_term_draft());
            reboot_from_configurator(protocol::ConfigExit::Save);
        }
        if (term == soundsinth::config::ConfigUiExit::Discard) reboot_from_configurator(protocol::ConfigExit::Discard);
    }
    const protocol::ConfigExit exit = orch.protocol.take_config_exit();
    if (exit != protocol::ConfigExit::None) reboot_from_configurator(exit);
}

void serve_configurator(SessionOrchestrator& orch) {
    uint8_t* const page = platform::config_rom_page();
    if (page != nullptr) serve_config_mode(orch, page);
}

} // namespace

void publish_file_info(uint32_t total_frames, const soundsinth::model::Song& song) {
    // Ноль - длительность неизвестна: проход длительности не влез в память
    // трека. Хост в этом случае рисует прочерки, а 00:00 он прочитал бы как
    // пустой трек.
    constexpr uint8_t kUnknown = protocol::HostProtocol::kTimeUnknown;
    const MinSec t             = to_min_sec(total_frames);
    const uint8_t mm           = total_frames != 0 ? t.minutes : kUnknown;
    const uint8_t ss           = total_frames != 0 ? t.seconds : kUnknown;
    hal::host_set_file_info(mm, ss, song.sample_count, song.pattern_count, song.instrument_count);
}

void SOUNDSINTH_HOT_PATH(serve_without_wait)(void*) {
    count_core1_loop();
    devices::sd::sd_spi_task();
    hal::host_service();
}

protocol::HostProtocol::Callbacks session_orchestrator_callbacks(SessionOrchestrator& orch) {
    protocol::HostProtocol::Callbacks cb;
    cb.user             = &orch;
    cb.on_session_start = &on_session_start;
    cb.on_reset         = &on_reset;
    return cb;
}

// Шина и протокол целиком на Core1: обработчики шины и этот цикл на одном
// ядре, гонок по HostProtocol нет (он не потокобезопасен). По времени этот
// цикл не критичен: защёлкивающая часть обмена сделана в ISR и PIO.
void SOUNDSINTH_HOT_PATH(session_orchestrator_run)(SessionOrchestrator& orch) {
    shared::g_bank.serve = &serve_without_wait;
    // Флеш-банк читает живой MIDI: сэмпл - до 16.8 мс одним чтением, без
    // обслуживания заказы карты и шина всё это время ждут.
    shared::g_flash_bank.serve = &serve_without_wait;
    devices::storage::storage_set_host_yield(&yield_storage_to_host, nullptr);
    // Долгие проходы разбора: без обслуживания проход длительности держит
    // виток Core1 секундами, и заказ сектора от esxDOS ждёт всё это время.
    load::session_loader_set_service(&serve_without_wait, nullptr);
    // И перекодировка сэмпла: кодирование между окнами хоста шину не
    // обслуживает, а самый долгий шаг - до 4.5 с.
    soundsinth::model::pack_file_pcm_set_service(&serve_without_wait, nullptr);
    devices::sd::sd_card_set_wait_service(&serve_bus_while_card_waits, nullptr);
    // То же для флешки: её сектор идёт миллисекунды, и всё это время машина
    // ждёт ответа на шине.
    //
    // Стек USB поднимается всегда, значит и обслуживать его надо всегда:
    // виток берёт критическую секцию и гасит на этом ядре все прерывания,
    // в том числе шинные, и без обслуживания машина встаёт.
    platform::usb_host_set_wait_service(&serve_bus_and_card_while_usb_waits, nullptr);

    debug_log("session_orchestrator_run: entering event loop\n");

    hal::host_release_reset();
    debug_log("session_orchestrator_run: host reset released\n");

    orch.last_position_us = platform::mono_us();
    // Прошлые значения монотонных счётчиков нагрузки: период усреднения
    // задаёт этот цикл.
    EngineLoadWatch load_watch = engine_load_watch_now(orch.last_position_us);
    uint32_t stops_seen        = shared::g_music_stop_req.load(std::memory_order_relaxed);
    uint32_t gs_resets_seen    = shared::g_gs_reset_req.load(std::memory_order_relaxed);
    for (;;) {
        if (const uint32_t r = shared::g_music_stop_req.load(std::memory_order_acquire); r != stops_seen) {
            stops_seen       = r;
            const uint32_t g = shared::g_gs_reset_req.load(std::memory_order_relaxed);
            stop_music(orch, g != gs_resets_seen);
            gs_resets_seen = g;
        }
        // Плеер GS начал загрузку модуля: план и фоновая догрузка прошлого
        // трека закрываются, иначе они вытесняли бы и грузили по чужому плану
        // поверх модуля.
        if ((orch.playing || orch.track.plan.plan_count != 0) && gs::bridge_wants_track_memory()) {
            drop_track(orch, "the GS player (#30)");
        }
        // Живой режим просит память трека: её делит begin() на Core0, а пишет
        // в неё фоновая догрузка здесь. Пока сеанс и план не закрыты, сброс
        // памяти шёл бы посреди выделения страниц, и список свободных страниц
        // пересобирался бы во время записи в него. grant_track_memory
        // разрешает вход в живой режим.
        if (live::wants_track_memory()) {
            if (orch.playing) drop_track(orch, "live MIDI");
            gs::bridge_track_memory_taken();
            live::grant_track_memory();
        }
        // То же при входе в живой режим: он снял файловый трек, и тот не
        // вернётся. Сеанс закрывается сразу, пока кадры ещё сравнимы с
        // длительностью файла: после выхода g_playback_frames замирает на
        // значении живого движка, и конец либо не наступает никогда, либо
        // приходит с чужим временем.
        if (orch.playing && live::session_active()) {
            if (!orch.track.ended_notified) notify_ended(orch, shared::g_playback_frames.load(std::memory_order_relaxed), false);
            orch.playing                        = false;
            orch.track.load_result.total_frames = 0;
        }
        // Эмуляция GS: разобрать накопленный модуль. Здесь, а не в прерывании:
        // снос прежнего движка - десятки миллисекунд, разбор - единицы.
        count_core1_loop();
        shared::g_core1_phase.store(shared::Core1Phase::Loop, std::memory_order_relaxed);
        shared::g_core1_step.store(shared::Core1Step::GsPoll, std::memory_order_relaxed);
        gs::bridge_poll();
        const uint32_t now = platform::mono_us();
        shared::g_core1_step.store(shared::Core1Step::Watch, std::memory_order_relaxed);
        watch_core0(now);
        watch_playback(orch, now);
        shared::g_core1_step.store(shared::Core1Step::HostService, std::memory_order_relaxed);
        hal::host_service();
        // Пока не вызвано, хост на чтении сектора ждёт, поэтому - в самом частом
        // месте цикла. Во всех сборках: карту читает и DivMMC.
        shared::g_core1_step.store(shared::Core1Step::SdTask, std::memory_order_relaxed);
        devices::sd::sd_spi_task();
        // Виток USB-хоста: другого планировщика на этом ядре нет. Стек
        // разбирает уже принятое и возвращается; на ПК это пустышка.
        shared::g_core1_step.store(shared::Core1Step::UsbTask, std::memory_order_relaxed);
        platform::usb_host_task();
        // После витка USB: флешка встаёт в нём, и порт 0x77 узнаёт о ней
        // тем же витком.
        devices::zcontroller::zcontroller_tick();
        serve_transport(orch);
        // Конфигуратор настроек: правка значений и выход.
        serve_configurator(orch);
        // Beta Disk: цилиндры образов с карты и запись в них. Здесь, а не в
        // ожиданиях носителя: карту отсюда можно читать без рекурсии.
        devices::bdi::bdi_poll();
        player::nav::nav_poll();

        if (orch.playing) {
            // Playing раз в секунду, Ended один раз: по g_song_ended или по
            // отыгранной длительности, иначе трек идёт по кругу и плагин не
            // уходит дальше. В живом режиме конца нет: длина от снесённого
            // файла, а взведённый признак конца глушит сеанс до следующего трека.
            const uint32_t played = shared::g_playback_frames.load(std::memory_order_relaxed);
            const bool song_ended = shared::g_song_ended.load(std::memory_order_relaxed);
            const bool finished =
                !live::session_active() && (song_ended || (orch.track.load_result.total_frames > 0 && played >= orch.track.load_result.total_frames));
            if (finished) {
                // Сначала заглушить выход, потом сообщить: секвенсор уже ушёл на
                // restart_position и играет трек второй раз, начало слышно поверх
                // затихающего конца, пока хост не закроется.
                shared::g_playback_finished.store(true, std::memory_order_relaxed);
                if (!orch.track.ended_notified) notify_ended(orch, played, song_ended);
            } else {
                pump_position(orch, now);
            }

            update_engine_load(orch, load_watch, now);
            pump_meters(orch);
        }

        if (!orch.session_pending) {
            // Фоновая догрузка - один сэмпл за проход цикла: между сэмплами шина
            // обслуживается, next/prev/ESC работают. Сэмплы файла идут только от
            // хоста, поэтому вне плеера шаг не начинается; сэмплы .mid - из банка
            // на карте, и догрузка идёт без хоста; у журнала VGM догружать нечего.
            const bool from_host = orch.track.load_result.format != load::TrackFormat::Midi && orch.track.load_result.format != load::TrackFormat::Vgm;
            const bool host_here = platform::mono_us() - hal::host_last_seen_us() < kHostGoneUs;
            if (orch.playing && from_host && host_here != orch.host_here) {
                orch.host_here = host_here;
                log_host_presence(host_here);
            }
            // Живая подгрузка - перед плановой: её ждёт нота, которая уже
            // выписана в строку, а план грузит впрок.
            shared::g_core1_step.store(shared::Core1Step::LiveServe, std::memory_order_relaxed);
            bool loaded = live::serve_requests();
            if (!loaded && orch.playing && (host_here || !from_host)) {
                shared::g_core1_step.store(shared::Core1Step::PlanLoad, std::memory_order_relaxed);
                orch.background_load = true;
                loaded               = load_next_planned_sample(orch);
                orch.background_load = false;
            }
            shared::g_core1_step.store(shared::Core1Step::IdleWait, std::memory_order_relaxed);
            if (!loaded) hal::idle_wait(kSleepMaxUs);
            continue;
        }
        // Новая сессия: состояние прошлого трека снимает close_session_source,
        // g_song_ended и g_playback_finished - track_load_begin.
        shared::g_core1_step.store(shared::Core1Step::RunSession, std::memory_order_relaxed);
        if (run_session(orch)) orch.last_position_us = platform::mono_us();
    }
}

} // namespace player
