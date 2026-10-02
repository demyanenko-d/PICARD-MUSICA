// SPDX-License-Identifier: MIT
// Звуковая цепочка плеера: движок, задача рендера, микшер и вывод.

#include "player/audio/chain.h"

#include <cinttypes>
#include <cstdio>
#include <new> // размещающий new для движка и задачи рендера

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"
#include "platform/os.h"

#include "core/audio/mixbus.h"
#include "core/engine/tracker_engine.h"
#include "core/engine/voice.h" // счётчики голоса для диагностики

#include "devices/config/config_service.h"

#include "player/hal/audio_out.h"
#include "player/shared_state.h"
#include "player/audio/buffer_pool.h"
#include "player/audio/render_task.h"
#include "player/audio/sequencer_task.h"
#include "player/config.h"
#include "player/live/ay_tap.h"
#include "player/live/session.h"

namespace player {
namespace {

// Миллисекунды той же шкалы, что у живого сеанса: счётчик 32-битный и
// заворачивается раз в 71.6 минуты, поэтому сравниваются только разности.
inline uint32_t now_ms() {
    return platform::mono_us() / 1000u;
}

// --- Движок и задача рендера без кучи ---
//
// Оба пересоздаются на каждый трек. Хранилище статическое: объект строится
// размещающим new и разрушается явным вызовом деструктора.
// sizeof(TrackerEngine) считает компилятор: рост движка (массивы по
// SOUNDSINTH_MAX_VOICES) даёт ошибку линковки, а не панику malloc; что у
// движка нет членов с кучей, проверяет static_assert у класса.
alignas(soundsinth::engine::TrackerEngine) uint8_t s_engine_storage[sizeof(soundsinth::engine::TrackerEngine)];
alignas(player::audio::RenderTask) uint8_t s_render_task_storage[sizeof(player::audio::RenderTask)];
alignas(player::audio::SequencerTask) uint8_t s_sequencer_task_storage[sizeof(player::audio::SequencerTask)];
// Только для строки лога: цепочка живёт локальной переменной задачи, а
// печатает диагностику функция без неё.
player::audio::RenderTask* s_render_task_for_log = nullptr;
// Он же для секвенсора. Счётчик тиков читается из чужой задачи без атомика:
// выровненное 32-битное слово читается одной командой, порванным не бывает.
player::audio::SequencerTask* s_sequencer_task_for_log = nullptr;
// Пул буферов вывода живёт всё время работы, но строится в задаче: его
// очереди - объекты ОС. Локальная переменная задачи не годится, буферы
// внутри объекта (4 КБ).
alignas(player::audio::BufferPool) uint8_t s_pool_storage[sizeof(player::audio::BufferPool)];
// Микшер - тоже не локальная переменная задачи: два с лишним килобайта в
// кадре, а кадр этот самый глубокий у app_task. Живёт он всё время работы,
// как и пул.
alignas(soundsinth::mixbus::MixBus) uint8_t s_mix_storage[sizeof(soundsinth::mixbus::MixBus)];

// TickObserver движка: раз за тик из RenderTask публикует для другого ядра
// карту занятых сэмплов, позицию, строку и полифонию. На горячем пути:
//   - без выделений памяти, логов и ожиданий: карта 512 бит (16 слов),
//     позиция, строка и три счётчика полифонии, всё атомиками;
//   - карта собирается локально и публикуется целиком, читатель не видит
//     половину;
//   - g_order_pos пишется после карты: читатель, увидевший новую позицию,
//     видит и её карту (обратный порядок позволил бы вытеснить сэмпл,
//     только что зазвучавший в этом тике).
// Сэмплы с индексом >= kSamplesInUseBits в карту не попадают, читатель
// считает их всегда занятыми.
void SOUNDSINTH_HOT_PATH(publish_tick_state)(void* user, uint16_t order_pos, const uint16_t* sample_indices, uint8_t count) {
    uint32_t bits[shared::kSamplesInUseWords] = {};
    for (uint8_t i = 0; i < count; ++i) {
        const uint16_t idx = sample_indices[i];
        if (idx < shared::kSamplesInUseBits) bits[idx / 32u] |= (1u << (idx % 32u));
    }
    for (uint16_t w = 0; w < shared::kSamplesInUseWords; ++w) {
        shared::g_samples_in_use[w].store(bits[w], std::memory_order_relaxed);
    }
    shared::g_order_pos.store(order_pos, std::memory_order_release);
    // Строка паттерна для команд #61/#62 эмуляции GS и полифония - от движка:
    // он приходит сюда пользовательским указателем. Наблюдателя ставит одна
    // строка и всегда с движком, поэтому проверки на пустой указатель нет.
    // count - длина карты сэмплов, она бывает больше числа звучащих голосов.
    const auto* engine = static_cast<const soundsinth::engine::TrackerEngine*>(user);
    shared::g_row_pos.store(engine->current_row(), std::memory_order_relaxed);
    const uint32_t voices = engine->active_voice_count();

    // Средняя полифония за период опроса телеметрии. Копятся сумма и число
    // тиков: период задаёт читатель. Счётчики монотонные, сбрасывать их
    // отсюда нельзя - гонка с читателем.
    // Писатель один - load и store, без ldrex/strex.
    shared::g_voice_count_ticks.store(shared::g_voice_count_ticks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    shared::g_voice_count_sq_sum.store(shared::g_voice_count_sq_sum.load(std::memory_order_relaxed) + voices * voices, std::memory_order_relaxed);
    // Пик: "поднять, если больше", без цикла compare-exchange: гонка безобидна,
    // а цикл на горячем пути не нужен.
    if (voices > shared::g_voice_count_peak.load(std::memory_order_relaxed)) {
        shared::g_voice_count_peak.store(voices, std::memory_order_relaxed);
    }
}

// Цепочка синтезатора. Пул, микшер и вывод живут всё время работы, движок и
// задача рендера пересоздаются на каждый трек.
// Индикатор сброса голосов (cull_level): последний счётчик движка, время
// последнего сброса, был ли сброс вообще.
struct CullIndicator {
    uint32_t last_culled  = 0;
    uint32_t last_cull_ms = 0;
    bool ever_culled      = false;
};

struct AudioChain {
    player::audio::BufferPool* pool              = nullptr;
    soundsinth::mixbus::MixBus* mix              = nullptr;
    soundsinth::engine::TrackerEngine* engine    = nullptr;
    player::audio::RenderTask* render_task       = nullptr;
    player::audio::SequencerTask* sequencer_task = nullptr;
    uint32_t seen_generation                     = 0;
    uint32_t underrun_at_build                   = 0; // андерраны с запуска на сборке движка - база итога трека
    hal::ReadyStats ready_at_build{};                 // запас очереди готовых на сборке - база итога трека
    bool out_started = false;                         // вывод настраивается один раз за работу прошивки
    CullIndicator cull;
};

// Затухший буфер встаёт в канал вывода не позже чем через min(D + 2, N - 1)
// окончаний буфера: доигрывающий, заряженный, очередь готовых и буфер в
// руках RenderTask. Запас - ещё буфер.
constexpr uint32_t kFadeWaitMs = 30;
constexpr uint32_t kFadeBuffersAhead =
    SOUNDSINTH_RENDERED_QUEUE_DEPTH + 2u < SOUNDSINTH_AUDIO_BUFFER_COUNT - 1u ? SOUNDSINTH_RENDERED_QUEUE_DEPTH + 2u : SOUNDSINTH_AUDIO_BUFFER_COUNT - 1u;
static_assert((kFadeBuffersAhead + 1u) * SOUNDSINTH_AUDIO_BUFFER_FRAMES * 1000000u / soundsinth::engine::kSampleRateHz <= kFadeWaitMs * 1000u,
              "faded buffer does not reach the output before silence: raise kFadeWaitMs");

// Паузы между проходами цикла. Потолок холостой задаёт кольцо отвода AY:
// поток начинается под играющим треком, и пауза длиннее его ёмкости теряет
// начало разговора.
constexpr uint32_t kPollBriskMs = 5;
constexpr uint32_t kPollIdleMs  = 25;
static_assert(kPollIdleMs < player::live::kAyTapSpanMs, "the idle pause is longer than the AY tap ring: the first writes of the stream will be lost");

// Диагностика тайминга тиков, голосов и памяти (active_voices, underrun,
// max_tick_us, ns_per_voice_sample, decode_per_1k_vs,
// raw8_jumps_per_1k_vs, step_clamp). Включается SOUNDSINTH_AUDIO_DIAG
// (уровни 0/1/2).
//
// Уровень 2 включает уровень 1 (>= 1): об андерране нужна немедленная
// строка, а не через период строки diag.
#if SOUNDSINTH_AUDIO_DIAG >= 1
// debug_log() пишет в кольцо без блокировок и прерывания не глушит.
void log_underruns(const soundsinth::engine::TrackerEngine& engine) {
    static uint32_t s_last_underrun = 0;
    static uint32_t s_last_late     = 0;
    const uint32_t underrun_total   = hal::audio_out_underruns();
    const uint32_t late             = hal::audio_out_late();
    if (underrun_total == s_last_underrun && late == s_last_late) return;
    char u[128];
    // bg - сколько андерранов из всех случилось, пока сэмпл догружался
    // фоном. Все под догрузкой - конкуренция за память, а не битые
    // сэмплы; вне догрузки - дело в данных или в полифонии. late -
    // опоздания вывода больше чем на буфер: пул тут ни при чём.
    snprintf(u, sizeof(u), "underrun: total=%" PRIu32 " (+%" PRIu32 ") bg=%" PRIu32 " late=%" PRIu32 " active_voices=%" PRIu32 "\n", underrun_total,
             underrun_total - s_last_underrun, hal::audio_out_underruns_bg(), late, engine.active_voice_count());
    debug_log(u);
    s_last_underrun = underrun_total;
    s_last_late     = late;
}
#endif // SOUNDSINTH_AUDIO_DIAG >= 1

#if SOUNDSINTH_AUDIO_DIAG == 2
// Строка diag. head - начало строки: "diag:" раз в период, "diag итог:" с
// кадрами трека на сносе. Печать не глушит прерывания (кольца debug_log):
// вывод и вытеснение этой задачи задачей RenderTask
// (SOUNDSINTH_RENDER_TASK_PRIORITY 2 против 1) работают во время печати. Не
// встраивается: буфер строки - в её кадре, а не в постоянном кадре задачи.
SOUNDSINTH_NOINLINE void print_diag(const char* head, const soundsinth::engine::TrackerEngine& engine, const soundsinth::mixbus::MixBus& mix,
                                    uint32_t underrun_base, const hal::ReadyStats& ready_base) {
    const uint32_t underrun_total = hal::audio_out_underruns();
    // Ключи строки: underrun - с момента старта, +N - от underrun_base;
    // active_voices - снимок, peak - максимум за трек; max_tick_us - самый
    // дорогой advance_tick(), at_worst - голоса и триггеры в нём, max_trig -
    // максимум триггеров за любой тик; ns/voice-сэмпл - voice_render+mix на
    // голосо-сэмпл; decode_per_1k_vs - взятий родного сэмпла на 1000
    // голосо-сэмплов, растёт с питчем нот.
    const uint64_t voice_samples = engine.voice_sample_count();
    const uint32_t ns_per_voice_sample =
        voice_samples > 0 ? static_cast<uint32_t>((static_cast<uint64_t>(engine.voice_loop_total_us()) * 1000u) / voice_samples) : 0;
    const soundsinth::engine::VoiceDebugCounters vdc = soundsinth::engine::voice_debug_counters();
    const uint32_t decode_calls                      = vdc.decode_calls;
    const uint64_t decode_per_1k_vs                  = voice_samples > 0 ? (1000ull * decode_calls) / voice_samples : 0;
    // Прыжок прямых кодеков обходит decode_and_advance() и в decode_per_1k_vs
    // не виден: у него свой счётчик той же нормировки.
    const uint32_t raw8_jumps           = vdc.direct_jumps;
    const uint64_t raw8_jumps_per_1k_vs = voice_samples > 0 ? (1000ull * raw8_jumps) / voice_samples : 0;
    // step_clamp - сколько раз voice.step прижат к потолку в
    // step_from_double(). Должно быть 0: ненулевое означает портаменто,
    // уводящее linear_pitch за диапазон приведения double -> uint32_t.
    const uint32_t step_clamp_count = vdc.step_clamps;

    // Загрузка рендера сейчас, в процентах (Q8 -> %): остальное в строке
    // накопительное с начала трека. Этой величиной управляется сброс голосов.
    const uint32_t load_pct = (engine.render_load_q8() * 100u) >> 8;

    // Клиппинг на выходе микшера здесь же: на слух перегруз и андерраны
    // неразличимы, а лечатся противоположно.
    struct DiagValues {
        uint32_t active_voices, peak, underrun, underrun_delta;
        uint32_t load_pct, budget, culled, nna_steals, no_sample, unlisted, demand_peak;
        uint32_t queue_peak, queue_full; // запас кольца команд голосам
        uint32_t clip, clip_run, jumps, max_jump;
        uint32_t max_tick_us, worst_voices, worst_trig, worst_row0, max_trig;
        // Тиков сделано задачей секвенсора. Стоящий секвенсор и стоящий
        // рендер по остальным числам одинаковы; за трек это число равно
        // ticks из строки duration.
        uint32_t seq_ticks;
        uint32_t ns_per_voice_sample, decode_per_1k_vs, raw8_jumps_per_1k_vs, step_clamp;
    };
    DiagValues v{};
    v.active_voices        = engine.active_voice_count();
    v.peak                 = engine.peak_active_voice_count();
    v.underrun             = underrun_total;
    v.underrun_delta       = underrun_total - underrun_base;
    v.load_pct             = load_pct;
    v.budget               = engine.voice_budget();
    v.culled               = engine.voices_culled();
    v.nna_steals           = engine.nna_steals();
    v.no_sample            = engine.triggers_without_sample();
    v.unlisted             = engine.voices_unlisted();
    v.demand_peak          = engine.voice_demand_peak();
    v.queue_peak           = engine.voice_queue_peak();
    v.queue_full           = engine.voice_queue_full();
    v.clip                 = mix.clipped_frames();
    v.clip_run             = mix.longest_clip_run();
    v.jumps                = mix.jumps();
    v.max_jump             = mix.max_jump();
    v.max_tick_us          = engine.max_tick_duration_us();
    v.seq_ticks            = s_sequencer_task_for_log != nullptr ? s_sequencer_task_for_log->ticks() : 0;
    v.worst_voices         = engine.active_voice_count_at_worst_tick();
    v.worst_trig           = engine.voice_triggers_at_worst_tick();
    v.worst_row0           = engine.worst_tick_at_row_start() ? 1u : 0u;
    v.max_trig             = engine.max_voice_triggers_per_tick();
    v.ns_per_voice_sample  = ns_per_voice_sample;
    v.decode_per_1k_vs     = static_cast<uint32_t>(decode_per_1k_vs);
    v.raw8_jumps_per_1k_vs = static_cast<uint32_t>(raw8_jumps_per_1k_vs);
    v.step_clamp           = step_clamp_count;

    // Буферы строк статические, а не на стеке: их почти килобайт, и лежат
    // они в самом глубоком кадре задачи - через snprintf с плавающей
    // точкой. Зовут print_diag только отсюда, из одной задачи.
    //
    // Запас очереди готовых за период или трек: сколько раз, забирая буфер,
    // вывод застал в очереди 0/1/.. готовых. 0 - заминка.
    static char ready[48];
    {
        const hal::ReadyStats now = hal::audio_out_ready_stats();
        uint32_t at               = 0;
        for (uint32_t k = 0; k < hal::kReadyBins && at < sizeof(ready); ++k) {
            const int n = snprintf(ready + at, sizeof(ready) - at, "%s%" PRIu32, k ? "/" : "", now.ready[k] - ready_base.ready[k]);
            if (n < 0) break;
            at += static_cast<uint32_t>(n);
        }
    }

    // Худший случай формата - около 600 байт.
    static char d[640];
    snprintf(d, sizeof(d),
             "%s active_voices=%" PRIu32 " peak=%" PRIu32 " underrun=%" PRIu32 " (+%" PRIu32 ") "
             "load=%" PRIu32 "%% budget=%" PRIu32 " culled=%" PRIu32 " nna_steals=%" PRIu32 " no_sample=%" PRIu32 " unlisted=%" PRIu32 " demand_peak=%" PRIu32
             " "
             "clipped=%" PRIu32 " (run %" PRIu32 ") jumps=%" PRIu32 " (max %" PRIu32 ") "
             "max_tick_us=%" PRIu32 " ticks=%" PRIu32 " "
             "at_worst(voices=%" PRIu32 ",trig=%" PRIu32 ",row0=%" PRIu32 ") max_trig=%" PRIu32 " ns_per_voice_sample=%" PRIu32 " "
             "decode_per_1k_vs=%" PRIu32 " raw8_jumps_per_1k_vs=%" PRIu32 " step_clamp=%" PRIu32 " "
             "ready_at_pull=%s queue=%" PRIu32 "/%u full=%" PRIu32 "\n",
             head, v.active_voices, v.peak, v.underrun, v.underrun_delta, v.load_pct, v.budget, v.culled, v.nna_steals, v.no_sample, v.unlisted, v.demand_peak,
             v.clip, v.clip_run, v.jumps, v.max_jump, v.max_tick_us, v.seq_ticks, v.worst_voices, v.worst_trig, v.worst_row0, v.max_trig, v.ns_per_voice_sample,
             v.decode_per_1k_vs, v.raw8_jumps_per_1k_vs, v.step_clamp, ready, v.queue_peak, static_cast<unsigned>(soundsinth::engine::kVoiceQueueCapacity),
             v.queue_full);
    debug_log(d);
    // Из чего сложился самый большой промежуток между готовыми буферами.
    // Очередь готовых - передышка убежавшего вперёд рендера, а не заминка;
    // заминка - это отрисовка длиннее буфера и ожидание ядра.
    if (s_render_task_for_log != nullptr) {
        const uint32_t gap   = s_render_task_for_log->worst_gap_us();
        const uint32_t wait  = s_render_task_for_log->worst_gap_wait_us();
        const uint32_t rend  = s_render_task_for_log->worst_gap_render_us();
        const uint32_t push  = s_render_task_for_log->worst_gap_push_us();
        const uint32_t spent = wait + rend + push;
        // Свой буфер: строка длиннее 192 байт буфера debug_logf.
        static char r[288];
        snprintf(r, sizeof(r),
                 "render: buffer max %" PRIu32 " us (buffer lasts %" PRIu32 "); gap %" PRIu32 " = ready queue %" PRIu32 " + free buffer %" PRIu32
                 " + render %" PRIu32 " + core %" PRIu32 "\n",
                 s_render_task_for_log->worst_render_us(), SOUNDSINTH_AUDIO_BUFFER_FRAMES * 1000000u / soundsinth::engine::kSampleRateHz, gap, push, wait, rend,
                 gap > spent ? gap - spent : 0u);
        debug_log(r);
    }
}

// Раз в 3000 x SOUNDSINTH_LOG_RARITY мс из цикла задачи (50 мс; не
// аудиокритичный).
void log_diag(const soundsinth::engine::TrackerEngine& engine, const soundsinth::mixbus::MixBus& mix) {
    static uint32_t s_last_diag_ms          = 0;
    static uint32_t s_underrun_at_last_diag = 0;
    static hal::ReadyStats s_ready_at_last_diag{};
    const uint32_t ms = now_ms();
    if (ms - s_last_diag_ms < 3000u * SOUNDSINTH_LOG_RARITY) return;
    const uint32_t underrun_total     = hal::audio_out_underruns();
    const hal::ReadyStats ready_total = hal::audio_out_ready_stats();
    print_diag("diag:", engine, mix, s_underrun_at_last_diag, s_ready_at_last_diag);
    s_last_diag_ms          = ms;
    s_underrun_at_last_diag = underrun_total;
    s_ready_at_last_diag    = ready_total;
}

// Итог трека на сносе: последние до 24 с трека в периодическую строку не
// попадают. +N у underrun и готовые при заборе - за трек.
void log_diag_track_end(const soundsinth::engine::TrackerEngine& engine, const soundsinth::mixbus::MixBus& mix, uint32_t underrun_at_build,
                        const hal::ReadyStats& ready_at_build) {
    char head[48];
    snprintf(head, sizeof(head), "diag total: frames=%" PRIu32, engine.elapsed_frames());
    print_diag(head, engine, mix, underrun_at_build, ready_at_build);
}
#endif // SOUNDSINTH_AUDIO_DIAG == 2

// Счётчики микшера за трек. "на_шкале" - кадры, где сумма ушла за int16;
// "мягко" - из них прошедшие кривую насыщения (у трекеров она выключена, там
// полка). Лимитер работает до шкалы и в эти счётчики не попадает.
void log_mix_track_stats(const soundsinth::mixbus::MixBus& mix) {
    debug_logf("bus: clipped=%" PRIu32 " run=%" PRIu32 " jumps=%" PRIu32 " max=%" PRIu32 " soft=%" PRIu32 "\n", mix.clipped_frames(), mix.longest_clip_run(),
               mix.jumps(), mix.max_jump(), mix.soft_clipped_frames());
    if (mix.limited_frames() != 0) {
        debug_logf("limiter: frames under gain reduction %" PRIu32 ", gain fell to %" PRIu32 "/32768\n", mix.limited_frames(), mix.min_gain_q15());
    }
}

// Снос движка по смене трека. shared::g_teardown_requested взводится на
// каждый 0x01 с шины, безусловно: ESC тоже должен заглушить звук.
// RenderTask останавливается до remove_source() и добавления нового
// источника: MixBus::render() из фоновой задачи не синхронизирован с
// изменением списка источников. Вывод, BufferPool и MixBus живут всё время
// работы.
void teardown_engine(AudioChain& chain) {
    const uint32_t started_us = platform::mono_us();
    // Погасить выход за буфер вместо ступеньки в ноль посреди звука. Пауза,
    // а не ожидание события: при вставшем прерывании снос идёт дальше.
    chain.mix->fade_out();
    // Ждать факт затухания, а не отмерять время: на смене трека рендер и
    // не успевает - опоздал, и DMA повторяет прежний полногромкий буфер, а
    // тишина ложится поверх полной амплитуды. kFadeWaitMs остаётся
    // потолком: вставший рендер снос не подвешивает.
    //
    // Между шагами разбирается отвод AY: вход в живой режим идёт с
    // играющего трека, и всё время сноса кольцо не читает никто. На выходе
    // из режима разбор молчит сам - там очередь пишет тик рендера.
    uint32_t waited = 0;
    while (waited < kFadeWaitMs && !chain.mix->faded()) {
        platform::os_task_delay_ms(kPollBriskMs);
        waited += kPollBriskMs;
        player::live::pump_stream(now_ms());
    }
    if (!chain.mix->faded()) {
        debug_logf("teardown: fade did not finish in %" PRIu32 " ms\n", waited);
    } else {
        // Затухший буфер ещё едет к ЦАП: доигрывающий, заряженный, очередь
        // готовых и буфер в руках рендера.
        for (uint32_t i = 0; i < kFadeBuffersAhead && waited < kFadeWaitMs; ++i) {
            platform::os_task_delay_ms(kPollBriskMs);
            waited += kPollBriskMs;
            player::live::pump_stream(now_ms());
        }
    }
    // Затем заглушить выход, иначе он повторяет последние буферы: хрип на
    // последней ноте до старта нового трека, а на выходе из плагина -
    // бесконечно.
    hal::audio_out_set_silent(true);
    // set_silent(true) до stop() безопасен только потому, что режим тишины
    // продолжает прокручивать буферы пула. Перестанет - RenderTask повиснет
    // в begin_write(), stop() будет вечно ждать семафор done_, задача
    // встанет.
    const uint32_t stop_started_us = platform::mono_us();
    chain.render_task->stop(
        [](uint32_t waited_ms) { debug_logf("render_task: stop waiting %" PRIu32 " ms, underrun=%" PRIu32 "\n", waited_ms, hal::audio_out_underruns()); });
    const uint32_t stop_us           = platform::mono_us() - stop_started_us;
    const uint32_t render_stack_free = chain.render_task->stack_unused_bytes();
    // Готовые буферы прошлого трека не должны достаться новому.
    chain.pool->drain_rendered();
    log_mix_track_stats(*chain.mix);
#if SOUNDSINTH_AUDIO_DIAG == 2
    log_diag_track_end(*chain.engine, *chain.mix, chain.underrun_at_build, chain.ready_at_build);
#endif
    chain.mix->remove_source(chain.engine->as_sound_source());
    // Явные деструкторы вместо delete: память статическая (s_engine_storage).
    s_render_task_for_log = nullptr; // до разрушения: лог печатает из другой задачи
    chain.render_task->~RenderTask();
    // Секвенсор - после рендера: пока тот жив, он ждёт от него тика.
    uint32_t sequencer_stack_free = 0;
    uint32_t sequencer_ticks      = 0;
    if (chain.sequencer_task != nullptr) {
        chain.engine->set_tick_runner(nullptr, nullptr);
        chain.sequencer_task->stop();
        sequencer_stack_free     = chain.sequencer_task->stack_unused_bytes();
        sequencer_ticks          = chain.sequencer_task->ticks();
        s_sequencer_task_for_log = nullptr; // до разрушения: лог печатает из другой задачи
        chain.sequencer_task->~SequencerTask();
        chain.sequencer_task = nullptr;
    }
    chain.engine->~TrackerEngine();
    chain.render_task = nullptr;
    chain.engine      = nullptr;
    // Смена поколения при живом движке гасится вместе с ним: иначе он
    // собрался бы заново из песни в памяти, возможно недогруженной.
    // Поколение читается до объявления движка мёртвым: другое ядро поднимает
    // его, только увидев false, а между двумя записями задача вытесняема.
    chain.seen_generation = shared::g_song_generation.load(std::memory_order_acquire);
    shared::g_engine_alive.store(false, std::memory_order_release);
    // Время сноса и метка от старта: промежуток от конца трека до звука
    // следующего складывается из строк обоих ядер.
    // Строка у предела буфера debug_logf (192 байта): новое поле - только
    // взамен старого.
    debug_logf("app_task: track torn down (reset from bus) in %" PRIu32 " ms (stop %" PRIu32 " ms), stack free render %" PRIu32 " seq %" PRIu32
               ", ticks %" PRIu32 ", t=%" PRIu32 " ms\n",
               (platform::mono_us() - started_us) / 1000u, stop_us / 1000u, render_stack_free, sequencer_stack_free, sequencer_ticks, now_ms());
}

// Живой MIDI: движок поверх песни, которая рождается из потока. От файлового
// пути отличается тем, что грузить нечего - строки даёт поток, а сэмплы
// тянутся по заказам, - и тем, что конца у такого трека нет.
bool build_live_engine(AudioChain& chain, uint32_t ms) {
    if (!shared::try_mark_engine_alive()) return false; // идёт загрузка с шины
    if (!player::live::session_begin(ms)) {
        shared::g_engine_alive.store(false, std::memory_order_release);
        return false;
    }
    chain.underrun_at_build = hal::audio_out_underruns();
    chain.ready_at_build    = hal::audio_out_ready_stats();
    chain.engine            = new (s_engine_storage) soundsinth::engine::TrackerEngine(shared::g_song, shared::g_track_memory);
    chain.engine->set_tick_observer(&publish_tick_state, chain.engine);
    chain.engine->set_voice_cull_enabled(SOUNDSINTH_VOICE_CULL_ON_OVERLOAD != 0);
    chain.engine->set_live_row_source(&player::live::live_row, player::live::row_user());
    chain.mix->start_track();
    chain.mix->set_end_frame(0); // живой поток не кончается
    chain.mix->add_source(chain.engine->as_sound_source());
    chain.mix->set_limiter(shared::g_song.limiter_enabled);
    chain.mix->set_soft_clip(shared::g_song.soft_clip_enabled);
    hal::audio_out_set_silent(false);
    chain.sequencer_task = new (s_sequencer_task_storage) player::audio::SequencerTask(*chain.engine);
    chain.engine->set_tick_runner(&player::audio::SequencerTask::run_tick, chain.sequencer_task);
    chain.render_task        = new (s_render_task_storage) player::audio::RenderTask(*chain.mix, *chain.pool, &shared::g_render_busy_us);
    s_render_task_for_log    = chain.render_task;
    s_sequencer_task_for_log = chain.sequencer_task;
    if (!chain.out_started) {
        hal::audio_out_start(*chain.pool, soundsinth::engine::kSampleRateHz);
        chain.out_started = true;
    }
    return true;
}

// Живой режим за проход задачи: вход по потоку, заказы сэмплов и выход по
// тишине. Трек из файла и живой поток - разные приложения: пошёл поток -
// файловое воспроизведение сносится, началась загрузка с шины - сносится
// живое.
void poll_live(AudioChain& chain) {
    // Выключен настройками - поток не слушаем вовсе. Отвод AY при этом
    // копится и теряется: разбирать его ради выброса дороже.
    if (devices::config::settings().live_midi == static_cast<uint8_t>(soundsinth::config::LiveMidi::None)) return;
    uint32_t ms = now_ms();
    if (player::live::session_active()) {
        // Разбор молчащих нот - до подгрузки: после неё сэмпл, дочитанный
        // через миллисекунды после ноты, выглядел бы успевшим.
        if (chain.engine != nullptr) player::live::note_missing(*chain.engine);
        // Подгрузка живёт в цикле другого ядра: здесь её вытеснял бы рендер.
        ms                 = now_ms();
        const bool loading = shared::g_song_generation.load(std::memory_order_acquire) != chain.seen_generation;
        const bool silent  = player::live::silent_ms(ms) >= SOUNDSINTH_LIVE_MIDI_TIMEOUT_MS;
        if (loading || silent) {
            debug_logf("live: output - %s\n", loading ? "track from bus" : "silence");
            if (chain.engine) teardown_engine(chain);
            player::live::session_end();
        }
        return;
    }
    if (!player::live::stream_started(ms)) return;
    // Идёт загрузка - вход запрещён, и сносить движок нельзя: собрать его
    // обратно всё равно не дадут, а трек уже не вернётся. Ждём следующего
    // прохода; счётчик событий снимается, чтобы порог брался заново.
    if (shared::g_load_in_progress.load(std::memory_order_acquire)) {
        player::live::session_end();
        return;
    }
    // Память трека режет begin(), а наполняет её фоновая догрузка на другом
    // ядре. Поэтому сначала просьба, и только по подтверждению Core1 - снос
    // движка и вход. То же рукопожатие, что у эмуляции GS.
    if (!player::live::track_memory_granted()) {
        player::live::request_track_memory();
        return;
    }
    debug_log("live: MIDI stream started\n");
    // Снос и сборка - одним проходом: задачи звука не снимаются, а
    // паркуются, ждать освобождения их стеков больше нечего.
    if (chain.engine) {
        teardown_engine(chain);
        ms = now_ms(); // затухание сноса заняло время
    }
    if (!build_live_engine(chain, ms)) player::live::session_end();
}

// Сборка движка поверх готового трека (новое поколение, движка нет).
void build_engine_if_ready(AudioChain& chain) {
    const uint32_t gen = shared::g_song_generation.load(std::memory_order_acquire);
    if (gen == chain.seen_generation || chain.engine) return;
    if (!shared::try_mark_engine_alive()) return; // идёт загрузка

    // Песню этого поколения бросил сброс хоста или переписала загрузка,
    // закончившаяся отказом: собирать нечего до следующего поколения.
    if (gen == shared::g_stale_generation.load(std::memory_order_relaxed)) {
        shared::g_engine_alive.store(false, std::memory_order_release);
        chain.seen_generation = gen;
        debug_log("app_task: track generation dropped by reset or load, engine not built\n");
        return;
    }
    chain.seen_generation   = gen;
    chain.underrun_at_build = hal::audio_out_underruns();
    chain.ready_at_build    = hal::audio_out_ready_stats();
    shared::g_engine_generation.store(gen, std::memory_order_relaxed);
    chain.engine = new (s_engine_storage) soundsinth::engine::TrackerEngine(shared::g_song, shared::g_track_memory);
    chain.engine->set_tick_observer(&publish_tick_state, chain.engine);
    // Сброс лишних голосов при перегрузке включается здесь, а не в движке:
    // TrackerEngine стартует с выключенным, чтобы тесты и pc_player оставались
    // побитово воспроизводимыми.
    chain.engine->set_voice_cull_enabled(SOUNDSINTH_VOICE_CULL_ON_OVERLOAD != 0);
    chain.mix->start_track();
    chain.mix->set_end_frame(shared::g_track_end_frame.load(std::memory_order_relaxed));
    chain.mix->add_source(chain.engine->as_sound_source());
    // Лимитер и мягкое насыщение включает формат трека.
    chain.mix->set_limiter(shared::g_song.limiter_enabled);
    chain.mix->set_soft_clip(shared::g_song.soft_clip_enabled);
    // Тишина снимается до RenderTask: задача рендерит первые буферы, не
    // дожидаясь возврата сюда, а режим тишины их выбрасывал бы - трек
    // начинался бы с кадра 256 со ступенькой. Пока готовых нет, выход играет
    // тишину без счёта андерранов.
    hal::audio_out_set_silent(false);
    // Счётчик занятости в shared_state, а не в задаче: RenderTask
    // пересоздаётся на каждый трек, а число должно пережить смену и быть видно
    // другому ядру.
    // Тик - своей задачей, и она заводится до рендера: тот с первого же
    // буфера станет её будить.
    chain.sequencer_task = new (s_sequencer_task_storage) player::audio::SequencerTask(*chain.engine);
    chain.engine->set_tick_runner(&player::audio::SequencerTask::run_tick, chain.sequencer_task);
    chain.render_task =
        new (s_render_task_storage) player::audio::RenderTask(*chain.mix, *chain.pool, &shared::g_render_busy_us); // сама создаёт фоновую задачу рендера
    s_render_task_for_log    = chain.render_task;
    s_sequencer_task_for_log = chain.sequencer_task;

    if (!chain.out_started) {
        // Вывод стартует один раз за работу прошивки, на первом готовом треке.
        hal::audio_out_start(*chain.pool, soundsinth::engine::kSampleRateHz);
        chain.out_started = true;
        debug_log("app_task: first track ready, audio out created+started\n");
    } else {
        debug_logf("app_task: next track ready, t=%" PRIu32 " ms\n", now_ms());
    }
}

// Уровень сброса голосов: 0 - не режем, 1..3 - насколько просела полифония
// против потолка. Держится две секунды после последнего сброса.
uint32_t cull_level(CullIndicator& ind, const soundsinth::engine::TrackerEngine& engine, uint32_t ms) {
    const uint32_t culled = engine.voices_culled();
    if (culled > ind.last_culled) {
        ind.last_cull_ms = ms;
        ind.ever_culled  = true;
    }
    // Новый трек обнуляет счётчик движка, базу тоже.
    ind.last_culled = culled;

    if (!ind.ever_culled || (ms - ind.last_cull_ms) >= 2000u) return 0;
    const uint32_t budget = engine.voice_budget();
    if (budget >= SOUNDSINTH_MAX_VOICES * 3u / 4u) return 1;
    if (budget >= SOUNDSINTH_MAX_VOICES / 2u) return 2;
    return shared::kVoiceCullLevelMax;
}

// Состояние воспроизведения для другого ядра: позиция, уровень сброса
// голосов, конец песни; глушит выход, когда трек доиграл.
// Рукопожатие перемотки: отметка счётчика тихих проходов микшера и то,
// что она взята. Только эта задача, другого писателя нет.
uint32_t s_seek_silent_mark = 0;
bool s_seek_silent_seen     = false;
// Проходов прождано тишины. Рукопожатие - единственное доказательство, что
// рендер уже не внутри движка, и ломается оно молча, щелчком на перемотке.
// Нуль - ждать не пришлось ни разу.
uint32_t s_seek_waits = 0;

void publish_playback_state(AudioChain& chain) {
    const soundsinth::engine::TrackerEngine& engine = *chain.engine;
    shared::g_playback_frames.store(engine.elapsed_frames(), std::memory_order_relaxed);
    shared::g_voice_cull_level.store(cull_level(chain.cull, engine, now_ms()), std::memory_order_relaxed);

    if (engine.song_ended()) {
        shared::g_song_ended.store(true, std::memory_order_relaxed);
    }
    // Трек доиграл (решает другое ядро) - выход гаснет затуханием, не
    // тишиной в прерывании: та выбросила бы и уже затухший буфер. Движок
    // крутится дальше, на restart_position.
    const bool finished = shared::g_playback_finished.load(std::memory_order_relaxed);
    if (finished) chain.mix->fade_out();

    // Пауза: решает другое ядро, исполняет микшер. Доигравший трек паузе не
    // поддаётся - его выход уже погашен навсегда. Перемотка гасит выход
    // сама, своей паузой поверх пользовательской.
    //
    // Доигравшему паузу не исполняем вовсе. Снятие паузы выходит из неё
    // погашенным только по end_frame_, а при неизвестной длительности (проход
    // не влез в память или конец объявил сам секвенсор) он нулевой - и в
    // выходе появлялся всплеск начала песни со второго прохода.
    const uint32_t seek = shared::g_seek_frames.load(std::memory_order_relaxed);
    if (!finished) {
        chain.mix->set_paused(shared::g_playback_paused.load(std::memory_order_relaxed) || seek != 0);
    }
    if (seek == 0) {
        s_seek_silent_seen = false;
        s_seek_waits       = 0;
        return;
    }
    // Ждём, пока рендер уйдёт в тишину: до этого он может быть внутри
    // вызова движка, и перематывать под ним нельзя. Счётчик тихих проходов
    // - это и есть доказательство, что источники он уже не трогает.
    const uint32_t silent = chain.mix->silent_renders();
    if (!s_seek_silent_seen) {
        s_seek_silent_mark = silent;
        s_seek_silent_seen = true;
        return;
    }
    if (silent == s_seek_silent_mark) {
        ++s_seek_waits;
        return;
    }

    const uint32_t from = engine.elapsed_frames();
    // Дальше конца трека прыгать некуда: там уже второй проход песни, и
    // прогон тиков туда только тратит время на то, что не прозвучит.
    const uint32_t end = shared::g_track_end_frame.load(std::memory_order_relaxed);
    uint32_t target    = from + seek;
    if (end != 0 && (target > end || target < from)) target = end;
    const bool moved = chain.engine->seek_to_frame(target);
    // Прыжок - шине: кадр конца трека она считает своим счётчиком, а
    // пропущенное мимо неё прошло.
    if (moved) chain.mix->skip_frames(chain.engine->elapsed_frames() - from);
    debug_logf("seek: %" PRIu32 " -> %" PRIu32 " frame%s, waited for silence %" PRIu32 " pass%s\n", from, chain.engine->elapsed_frames(),
               moved ? "" : " (refused)", s_seek_waits, s_seek_waits == 1u ? "" : "es");
    shared::g_seek_frames.store(0, std::memory_order_relaxed);
    s_seek_silent_seen = false;
    s_seek_waits       = 0;
}

} // namespace

void audio_chain_task(void* /*arg*/) {
    debug_log("app_task: entered\n");

    AudioChain chain;
    chain.pool = new (s_pool_storage) player::audio::BufferPool();
    debug_log("app_task: BufferPool ready\n");

    soundsinth::mixbus::MixBus& mix = *new (s_mix_storage) soundsinth::mixbus::MixBus();
    chain.mix                       = &mix;
    debug_log("app_task: MixBus ready, waiting for first track\n");

    for (;;) {
        shared::g_app_task_loops.store(shared::g_app_task_loops.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        // Просьба гасится и без движка: иначе снесла бы собранный позже.
        const bool teardown = shared::g_teardown_requested.exchange(false, std::memory_order_acq_rel);
        if (teardown && chain.engine) {
            teardown_engine(chain);
            player::live::session_end(); // сброс с шины гасит и живое
        } else {
            // Поток слушается и под играющий трек: пошёл живой MIDI - трек
            // сносится. Сборка трека с шины - только пока живого нет.
            if (!player::live::session_active()) build_engine_if_ready(chain);
            poll_live(chain);
        }
        if (chain.engine) {
            publish_playback_state(chain);
#if SOUNDSINTH_AUDIO_DIAG >= 1
            log_underruns(*chain.engine);
#endif
#if SOUNDSINTH_AUDIO_DIAG == 2
            log_diag(*chain.engine, mix);
#endif
        }
        // Под живой режим и в ожидании потока - чаще: заказанные сэмплы ждут
        // этот же проход. С играющим треком спешить некуда, но и зевать
        // нельзя: поток начинается, пока трек ещё звучит, и первые записи
        // ждут в кольце отвода.
        const bool brisk = player::live::session_active() || chain.engine == nullptr;
        platform::os_task_delay_ms(brisk ? kPollBriskMs : kPollIdleMs);
    }
}

} // namespace player
