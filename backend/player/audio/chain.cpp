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

#include "player/config/config_service.h"

#include "player/hal/audio_out.h"
#include "player/hal/flash_cache.h"
#include "player/shared_state.h"
#include "player/audio/buffer_pool.h"
#include "player/audio/render_task.h"
#include "player/gs/bridge.h"
#include "player/gs/fx_player.h"
#include "player/audio/sequencer_task.h"
#include "player/config.h"
#include "player/live/ay_live.h"
#include "player/live/dac_live.h"
#include "player/live/live_clock.h"
#include "player/live/saa_live.h"
#include "player/live/live_tap.h"
#include "player/live/session.h"
#include "player/vgm/vgm.h"

namespace player {
namespace {

// --- Движок и задача рендера без кучи ---
//
// Оба пересоздаются на каждый трек. Хранилище статическое: объект строится
// размещающим new и разрушается явным вызовом деструктора.
// sizeof(TrackerEngine) считает компилятор: рост движка (массивы по
// SOUNDSINTH_MAX_VOICES) даёт ошибку линковки, а не панику malloc; что у
// движка нет членов с кучей, проверяет static_assert у класса.
alignas(soundsinth::engine::TrackerEngine) uint8_t s_engine_storage[sizeof(soundsinth::engine::TrackerEngine)];
alignas(audio::RenderTask) uint8_t s_render_task_storage[sizeof(audio::RenderTask)];
alignas(audio::SequencerTask) uint8_t s_sequencer_task_storage[sizeof(audio::SequencerTask)];
// Пул буферов вывода живёт всё время работы, но строится в задаче: его
// очереди - объекты ОС. Локальная переменная задачи не годится, буферы
// внутри объекта (6 КБ).
alignas(audio::BufferPool) uint8_t s_pool_storage[sizeof(audio::BufferPool)];
// Микшер - тоже не локальная переменная задачи: два с лишним килобайта в
// кадре, а кадр этот самый глубокий у app_task. Живёт он всё время работы,
// как и пул.
alignas(soundsinth::mixbus::MixBus) uint8_t s_mix_storage[sizeof(soundsinth::mixbus::MixBus)];
// Измеритель выхода шины: пики и снимок под спектр. Считается, когда хост
// заказал VU или спектр.
soundsinth::mixbus::Meter s_meter;

// Средняя полифония за период опроса телеметрии. Копятся сумма и число
// замеров: период задаёт читатель. Счётчики монотонные, сбрасывать их
// отсюда нельзя - гонка с читателем. Замер - тик движка или, у VGM, вызов
// источника шины; голоса живых чипов шины - вместе с ними (у VGM живые
// чипы выключены). Писатель один - load и store, без ldrex/strex. Во
// флеше: зовётся раз в тик, место в SRAM дороже.
SOUNDSINTH_NOINLINE void count_voices(uint32_t voices) {
    voices += live::ay_live_heard() + live::saa_live_heard();
    shared::g_voice_count_ticks.store(shared::g_voice_count_ticks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    shared::g_voice_count_sq_sum.store(shared::g_voice_count_sq_sum.load(std::memory_order_relaxed) + voices * voices, std::memory_order_relaxed);
    // Пик: "поднять, если больше", без цикла compare-exchange: гонка безобидна,
    // а цикл на горячем пути не нужен.
    if (voices > shared::g_voice_count_peak.load(std::memory_order_relaxed)) {
        shared::g_voice_count_peak.store(voices, std::memory_order_relaxed);
    }
}

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
    // Поканальная телеметрия: её показывают экраны плееров. Берутся первые
    // четыре канала - столько у MOD, и столько же просит система команд GS.
    shared::g_speed.store(engine->current_speed(), std::memory_order_relaxed);
    const uint8_t tele_ch = engine->channel_count() < shared::kTelemetryChannels ? engine->channel_count() : shared::kTelemetryChannels;
    for (uint8_t c = 0; c < shared::kTelemetryChannels; ++c) {
        const bool have = c < tele_ch;
        shared::g_channel_note[c].store(have ? engine->channel_note(c) : 0u, std::memory_order_relaxed);
        shared::g_channel_volume[c].store(have ? engine->channel_volume(c) : 0u, std::memory_order_relaxed);
    }
    count_voices(engine->active_voice_count());
}

// Индикатор сброса голосов (cull_level): последний счётчик движка, время
// последнего сброса, был ли сброс вообще.
struct CullIndicator {
    uint32_t last_culled  = 0;
    uint32_t last_cull_ms = 0;
    bool ever_culled      = false;
};

// Рукопожатие перемотки: отметка счётчика тихих проходов микшера и то, что
// она взята. waits - проходов прождано тишины: рукопожатие - единственное
// доказательство, что рендер уже не внутри источника, и ломается оно молча,
// щелчком на перемотке. Нуль - ждать не пришлось ни разу.
struct SeekWait {
    uint32_t mark  = 0;
    bool seen      = false;
    uint32_t waits = 0;
};

// Что играет: ничего, рендер без трека для живых чипов, трек движка (из
// файла или живой MIDI), журнал VGM.
enum class Track : uint8_t { None, Idle, Engine, Vgm };

struct AudioChain {
    audio::BufferPool* pool                   = nullptr;
    soundsinth::mixbus::MixBus* mix           = nullptr;
    soundsinth::engine::TrackerEngine* engine = nullptr;
    audio::RenderTask* render_task            = nullptr;
    audio::SequencerTask* sequencer_task      = nullptr;
    uint32_t seen_generation                  = 0;
    uint32_t underrun_at_build                = 0; // андерраны с запуска на сборке движка - база итога трека
    hal::ReadyStats ready_at_build{};              // запас очереди готовых на сборке - база итога трека
    bool out_started = false;                      // вывод настраивается один раз за работу прошивки
    Track track      = Track::None;
    CullIndicator cull;
    SeekWait seek; // пишет только эта задача
    uint32_t held_ms = 0; // последний проход, когда шина стояла (меню NMI, пауза) - таймер тишины живого MIDI от него
};

// Трек-журнал VGM: плеер из арены трека, источник шины и счёт отыгранного.
// Поля пишет рендер; задача цепочки читает их выровненным словом.
struct VgmTrack {
    vgm::Player* player = nullptr;
    uint32_t frames     = 0;
    bool log_ended      = false;
    bool live_back      = false; // живые чипы и MIDI возвращены по концу журнала
    soundsinth::mixbus::SoundSource source{};
    uint32_t built_us = 0; // момент сборки и занятость рендера на нём - для доли ядра за трек
    uint32_t busy_us  = 0;
};
VgmTrack s_vgm;

void vgm_render_add(void*, int32_t* mix_l, int32_t* mix_r, uint32_t n) {
    if (!s_vgm.player->render_add(mix_l, mix_r, n)) s_vgm.log_ended = true;
    s_vgm.frames += n;
    count_voices(s_vgm.player->heard());
}

bool has_track(const AudioChain& chain) {
    return chain.track == Track::Engine || chain.track == Track::Vgm;
}

// Уровень трека на шине: лимитер и насыщение - по формату, авторегулировка -
// по настройке AUTO_VOLUME (пики после неё держит лимитер; компрессор
// остаётся только у формата с лимитером), кадр конца и затухание перед ним.
void set_track_levels(soundsinth::mixbus::MixBus& mix, bool limiter, bool soft_clip, bool auto_volume, uint32_t end_frame, uint32_t end_fade) {
    mix.set_end_frame(end_frame);
    mix.set_end_fade(end_fade);
    mix.set_auto_volume(auto_volume);
    mix.set_limiter(limiter || auto_volume);
    mix.set_compressor(SOUNDSINTH_MIDI_COMPRESSOR_THRESHOLD, limiter ? SOUNDSINTH_MIDI_COMPRESSOR_AMOUNT : 0);
    mix.set_soft_clip(soft_clip);
    mix.set_highpass_hz(soundsinth::mixbus::MixBus::kHighpassHz, soundsinth::engine::kSampleRateHz);
}

// Уровень трека из файла: конец и затухание - из загрузки, авторегулировка -
// из настроек.
void set_file_track_levels(soundsinth::mixbus::MixBus& mix, bool limiter, bool soft_clip) {
    set_track_levels(mix, limiter, soft_clip, player::config::settings().auto_volume != 0, shared::g_track_end_frame.load(std::memory_order_relaxed),
                     shared::g_track_end_fade.load(std::memory_order_relaxed));
}

// Паузы между проходами цикла. Потолок холостой задаёт кольцо отвода AY:
// поток начинается под играющим треком, и пауза длиннее его ёмкости теряет
// начало потока.
constexpr uint32_t kPollBriskMs = 5;
constexpr uint32_t kPollIdleMs  = 25;
static_assert(kPollIdleMs < live::kTapSpanMs, "the idle pause is longer than the AY tap ring: the first writes of the stream will be lost");
// VU и спектр хосту - раз в кадр экрана Спектрума. Спектр считается раз в
// 10 мс по окну 11.6 мс - без пропусков; атаку, замеченную рендером, счёт
// берёт ещё окном с ней в центре. Хосту уходит максимум полос за период.
// Задача цикла младше рендера и звук не задерживает.
constexpr uint32_t kMeterPeriodMs = 20;
constexpr uint32_t kSpectrumHopMs = 10;

// Пришёл ли срок due; если да - следующий на step дальше. Ровный шаг
// держится от прошлого срока, отставший на шаг и больше - от ms.
bool step_due(uint32_t& due, uint32_t ms, uint32_t step) {
    const int32_t late = static_cast<int32_t>(ms - due);
    if (late < 0) return false;
    due = late < static_cast<int32_t>(step) ? due + step : ms + step;
    return true;
}

// VU и максимум полос за период - другому ядру.
void publish_meter_values() {
    uint8_t l = 0, r = 0;
    s_meter.take_vu(l, r);
    shared::g_vu.store(l | (static_cast<uint32_t>(r) << 8), std::memory_order_relaxed);
    uint8_t bands[soundsinth::mixbus::kSpectrumBands];
    if (s_meter.take_spectrum(bands)) {
        for (uint32_t w = 0; w < 4; ++w) {
            shared::g_spectrum[w].store(bands[4 * w] | (bands[4 * w + 1] << 8) | (bands[4 * w + 2] << 16) | (static_cast<uint32_t>(bands[4 * w + 3]) << 24),
                                        std::memory_order_relaxed);
        }
    }
    // Ядро 1 отправляет хосту по смене счётчика, без своего таймера.
    shared::g_meter_seq.store(shared::g_meter_seq.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
}

// Измерители выхода - другому ядру, пока их заказывает хост. Возвращает,
// через сколько мс следующий срок.
uint32_t publish_meters(uint32_t ms) {
    static uint32_t s_due_ms = 0, s_hop_ms = 0;
    const bool wanted = shared::g_meter_wanted.load(std::memory_order_relaxed);
    s_meter.enable(wanted);
    if (!wanted) return kPollIdleMs;
    if (step_due(s_hop_ms, ms, kSpectrumHopMs)) s_meter.analyze();
    if (step_due(s_due_ms, ms, kMeterPeriodMs)) publish_meter_values();
    const uint32_t hop = s_hop_ms - ms, due = s_due_ms - ms;
    return hop < due ? hop : due;
}

// Цена одного спектра на этой плате - в журнал при старте: снимки тишины,
// 20 прогонов. Минимум - сам расчёт, максимум - с прерываниями и промахами
// кэша флеша (его делит PSRAM).
void log_spectrum_cost() {
    uint8_t bands[soundsinth::mixbus::kSpectrumBands];
    uint32_t lo = UINT32_MAX, hi = 0, sum = 0;
    for (int pass = 0; pass < 20; ++pass) {
        const int16_t silence[2 * 64] = {};
        for (uint32_t k = 0; k < soundsinth::mixbus::Meter::kFft / 64u; ++k) {
            s_meter.feed(silence, 64);
        }
        const uint32_t started = platform::mono_us();
        s_meter.take_spectrum(bands);
        const uint32_t us  = platform::mono_us() - started;
        lo                 = us < lo ? us : lo;
        hi                 = us > hi ? us : hi;
        sum               += us;
    }
    debug_logf("meter: spectrum of %u samples takes %" PRIu32 "..%" PRIu32 " us, mean %" PRIu32 "\n", static_cast<unsigned>(soundsinth::mixbus::Meter::kFft),
               lo, hi, sum / 20u);
}

// Затухший буфер встаёт в канал вывода не позже чем через min(D + 2, N - 1)
// окончаний буфера: доигрывающий, заряженный, очередь готовых и буфер в
// руках RenderTask. Запас - ещё буфер.
constexpr uint32_t kFadeWaitMs = 30;
constexpr uint32_t kFadeBuffersAhead =
    SOUNDSINTH_RENDERED_QUEUE_DEPTH + 2u < SOUNDSINTH_AUDIO_BUFFER_COUNT - 1u ? SOUNDSINTH_RENDERED_QUEUE_DEPTH + 2u : SOUNDSINTH_AUDIO_BUFFER_COUNT - 1u;
static_assert((kFadeBuffersAhead + 1u) * SOUNDSINTH_AUDIO_BUFFER_FRAMES * 1000000u / soundsinth::engine::kSampleRateHz <= kFadeWaitMs * 1000u,
              "faded buffer does not reach the output before silence: raise kFadeWaitMs");

// Диагностика тайминга тиков, голосов и памяти (active_voices, underrun,
// max_tick_us, ns_per_voice_sample, decode_per_1k_vs,
// raw8_jumps_per_1k_vs, step_clamp). Включается SOUNDSINTH_AUDIO_DIAG
// (уровни 0/1/2).
//
// Уровень 2 включает уровень 1 (>= 1): об андерране нужна немедленная
// строка, а не через период строки diag.
#if SOUNDSINTH_AUDIO_DIAG >= 1
// debug_log() пишет в кольцо без блокировок и прерывания не глушит.
void log_underruns(uint32_t voices) {
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
    std::snprintf(u, sizeof(u), "underrun: total=%" PRIu32 " (+%" PRIu32 ") bg=%" PRIu32 " late=%" PRIu32 " active_voices=%" PRIu32 "\n", underrun_total,
                  underrun_total - s_last_underrun, hal::audio_out_underruns_bg(), late, voices);
    debug_log(u);
    s_last_underrun = underrun_total;
    s_last_late     = late;
}
#endif // SOUNDSINTH_AUDIO_DIAG >= 1

#if SOUNDSINTH_AUDIO_DIAG == 2
// Строка diag. head - начало строки: "diag:" раз в период, "diag total:" с
// кадрами трека на сносе. Печать не глушит прерывания (кольца debug_log):
// вывод и вытеснение этой задачи задачей RenderTask
// (SOUNDSINTH_RENDER_TASK_PRIORITY 2 против 1) работают во время печати.
SOUNDSINTH_NOINLINE void print_diag(const char* head, const AudioChain& chain, uint32_t underrun_base, const hal::ReadyStats& ready_base) {
    const soundsinth::engine::TrackerEngine& engine = *chain.engine;
    const soundsinth::mixbus::MixBus& mix           = *chain.mix;
    const uint32_t underrun_total                   = hal::audio_out_underruns();
    const uint64_t voice_samples                    = engine.voice_sample_count();
    auto per_1k_vs = [voice_samples](uint64_t n) { return voice_samples > 0 ? static_cast<uint32_t>(1000ull * n / voice_samples) : 0u; };
    const soundsinth::engine::VoiceDebugCounters vdc = soundsinth::engine::voice_debug_counters();
    // Счётчик тиков пишет задача секвенсора, читается без атомика:
    // выровненное 32-битное слово читается одной командой.
    const uint32_t seq_ticks = chain.sequencer_task != nullptr ? chain.sequencer_task->ticks() : 0;

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
            const int n = std::snprintf(ready + at, sizeof(ready) - at, "%s%" PRIu32, k ? "/" : "", now.ready[k] - ready_base.ready[k]);
            if (n < 0) break;
            at += static_cast<uint32_t>(n);
        }
    }

    // Ключи строки: underrun - с момента старта, +N - от underrun_base;
    // active_voices - снимок, peak - максимум за трек; load - загрузка
    // рендера сейчас (Q8 -> %), ею управляется сброс голосов, остальное
    // накопительное с начала трека; clipped - клиппинг на выходе микшера: на
    // слух перегруз и андерраны неразличимы, а лечатся противоположно;
    // max_tick_us - самый дорогой advance_tick(), at_worst - голоса и
    // триггеры в нём, max_trig - максимум триггеров за любой тик; ticks -
    // тиков сделано задачей секвенсора: стоящий секвенсор и стоящий рендер по
    // остальным числам одинаковы, за трек это ticks из строки duration;
    // ns_per_voice_sample - voice_render+mix на голосо-сэмпл;
    // decode_per_1k_vs - взятий родного сэмпла на 1000 голосо-сэмплов, растёт
    // с питчем нот; raw8_jumps_per_1k_vs - прыжки прямых кодеков той же
    // нормировки: они обходят decode_and_advance() и в decode_per_1k_vs не
    // видны; step_clamp - сколько раз voice.step прижат к потолку в
    // step_from_double(), должно быть 0: ненулевое - портаменто, уводящее
    // linear_pitch за диапазон приведения double -> uint32_t; queue - запас
    // кольца команд голосам. Худший случай формата - около 600 байт.
    static char d[640];
    std::snprintf(d, sizeof(d),
                  "%s active_voices=%" PRIu32 " peak=%" PRIu32 " underrun=%" PRIu32 " (+%" PRIu32 ") "
                  "load=%" PRIu32 "%% budget=%" PRIu32 " culled=%" PRIu32 " nna_steals=%" PRIu32 " no_sample=%" PRIu32 " unlisted=%" PRIu32
                  " demand_peak=%" PRIu32 " "
                  "clipped=%" PRIu32 " (run %" PRIu32 ") jumps=%" PRIu32 " (max %" PRIu32 ") "
                  "max_tick_us=%" PRIu32 " ticks=%" PRIu32 " "
                  "at_worst(voices=%" PRIu32 ",trig=%" PRIu32 ",row0=%" PRIu32 ") max_trig=%" PRIu32 " ns_per_voice_sample=%" PRIu32 " "
                  "decode_per_1k_vs=%" PRIu32 " raw8_jumps_per_1k_vs=%" PRIu32 " step_clamp=%" PRIu32 " "
                  "ready_at_pull=%s queue=%" PRIu32 "/%u full=%" PRIu32 "\n",
                  head, engine.active_voice_count(), engine.peak_active_voice_count(), underrun_total, underrun_total - underrun_base,
                  (engine.render_load_q8() * 100u) >> 8, uint32_t{engine.voice_budget()}, engine.voices_culled(), engine.nna_steals(),
                  engine.triggers_without_sample(), engine.voices_unlisted(), engine.voice_demand_peak(), mix.clipped_frames(), mix.longest_clip_run(),
                  mix.jumps(), mix.max_jump(), engine.max_tick_duration_us(), seq_ticks, engine.active_voice_count_at_worst_tick(),
                  engine.voice_triggers_at_worst_tick(), uint32_t{engine.worst_tick_at_row_start() ? 1u : 0u}, engine.max_voice_triggers_per_tick(),
                  per_1k_vs(engine.voice_loop_total_us()), per_1k_vs(vdc.decode_calls), per_1k_vs(vdc.direct_jumps), vdc.step_clamps, ready,
                  engine.voice_queue_peak(), static_cast<unsigned>(soundsinth::engine::kVoiceQueueCapacity), engine.voice_queue_full());
    debug_log(d);
    // Доли времени рендера с начала трека, в промилле: тик (секвенсор,
    // эффекты, арбитр), голоса (цикл голосов и сведение), остальное (шины
    // эффектов, ревербератор, хорус).
    const uint32_t render_us = engine.render_total_us();
    if (render_us > 0) {
        const uint32_t tick_pm  = static_cast<uint32_t>((static_cast<uint64_t>(engine.tick_total_us()) * 1000u) / render_us);
        const uint32_t voice_pm = static_cast<uint32_t>((static_cast<uint64_t>(engine.voice_loop_total_us()) * 1000u) / render_us);
        debug_logf("%s render_us=%" PRIu32 " tick_permille=%" PRIu32 " voices_permille=%" PRIu32 " rest_permille=%" PRIu32 "\n", head, render_us, tick_pm,
                   voice_pm, tick_pm + voice_pm < 1000u ? 1000u - tick_pm - voice_pm : 0u);
    }
    // Из чего сложился самый большой промежуток между готовыми буферами.
    // Очередь готовых - передышка убежавшего вперёд рендера, а не заминка;
    // заминка - это отрисовка длиннее буфера и ожидание ядра.
    if (chain.render_task != nullptr) {
        const audio::RenderTask& rt = *chain.render_task;
        const uint32_t gap          = rt.worst_gap_us();
        const uint32_t wait         = rt.worst_gap_wait_us();
        const uint32_t rend         = rt.worst_gap_render_us();
        const uint32_t push         = rt.worst_gap_push_us();
        const uint32_t spent        = wait + rend + push;
        // Буфер строки diag: строка длиннее 192 байт буфера debug_logf, а
        // debug_log уже скопировал прошлую в кольцо.
        std::snprintf(d, sizeof(d),
                      "render: buffer max %" PRIu32 " us (buffer lasts %" PRIu32 "); gap %" PRIu32 " = ready queue %" PRIu32 " + free buffer %" PRIu32
                      " + render %" PRIu32 " + core %" PRIu32 "\n",
                      rt.worst_render_us(), SOUNDSINTH_AUDIO_BUFFER_FRAMES * 1000000u / soundsinth::engine::kSampleRateHz, gap, push, wait, rend,
                      gap > spent ? gap - spent : 0u);
        debug_log(d);
    }
}

// Раз в 3000 x SOUNDSINTH_LOG_RARITY мс из цикла app_task.
void log_diag(const AudioChain& chain) {
    static uint32_t s_last_diag_ms          = 0;
    static uint32_t s_underrun_at_last_diag = 0;
    static hal::ReadyStats s_ready_at_last_diag{};
    const uint32_t ms = platform::mono_ms();
    if (ms - s_last_diag_ms < 3000u * SOUNDSINTH_LOG_RARITY) return;
    const uint32_t underrun_total     = hal::audio_out_underruns();
    const hal::ReadyStats ready_total = hal::audio_out_ready_stats();
    print_diag("diag:", chain, s_underrun_at_last_diag, s_ready_at_last_diag);
    s_last_diag_ms          = ms;
    s_underrun_at_last_diag = underrun_total;
    s_ready_at_last_diag    = ready_total;
}

// Итог трека на сносе: последние до 24 с трека в периодическую строку не
// попадают. +N у underrun и готовые при заборе - за трек.
void log_diag_track_end(const AudioChain& chain) {
    char head[48];
    std::snprintf(head, sizeof(head), "diag total: frames=%" PRIu32, chain.engine->elapsed_frames());
    print_diag(head, chain, chain.underrun_at_build, chain.ready_at_build);
}
#endif // SOUNDSINTH_AUDIO_DIAG == 2

// Счётчики микшера за трек. "clipped" - кадры, где сумма ушла за int16;
// "soft" - из них прошедшие кривую насыщения (у трекеров она выключена, там
// полка). Лимитер работает до шкалы и в эти счётчики не попадает.
void log_mix_track_stats(const soundsinth::mixbus::MixBus& mix) {
    debug_logf("bus: clipped=%" PRIu32 " run=%" PRIu32 " jumps=%" PRIu32 " max=%" PRIu32 " soft=%" PRIu32 "\n", mix.clipped_frames(), mix.longest_clip_run(),
               mix.jumps(), mix.max_jump(), mix.soft_clipped_frames());
    // Усиление авторегулировки к концу трека, десятые доли дБ (октава - 6.02 дБ).
    if (const int32_t tenths = mix.auto_gain_log() * 602 / 655360; tenths != 0) {
        debug_logf("auto volume: gain %s%d.%d dB at the track end\n", tenths < 0 ? "-" : "+", static_cast<int>(tenths < 0 ? -tenths : tenths) / 10,
                   static_cast<int>(tenths < 0 ? -tenths : tenths) % 10);
    }
    if (mix.limited_frames() != 0) {
        debug_logf("limiter: frames under gain reduction %" PRIu32 ", gain fell to %" PRIu32 "/32768\n", mix.limited_frames(), mix.min_gain_q15());
    }
}

// Запуск рендера собранного трека. Тишина снимается до RenderTask: задача
// рендерит первые буферы, не дожидаясь возврата сюда, а режим тишины их
// выбрасывал бы - трек начинался бы с кадра 256 со ступенькой. Пока готовых
// нет, выход играет тишину без счёта андерранов. Тик движка - своей
// задачей, и она заводится до рендера: тот с первого же буфера станет её
// будить. Вывод стартует один раз за работу прошивки; true - стартовал
// сейчас.
bool start_render(AudioChain& chain) {
    hal::audio_out_set_silent(false);
    if (chain.track == Track::Engine) {
        chain.sequencer_task = new (s_sequencer_task_storage) audio::SequencerTask(*chain.engine);
#if !SOUNDSINTH_TICK_IN_RENDER
        chain.engine->set_tick_runner(&audio::SequencerTask::run_tick, chain.sequencer_task);
#endif
    }
    chain.render_task = new (s_render_task_storage) audio::RenderTask(*chain.mix, *chain.pool, &shared::g_render_busy_us);
    if (chain.out_started) return false;
    hal::audio_out_start(*chain.pool, soundsinth::engine::kSampleRateHz);
    live::live_clock_set_out_rate(hal::audio_out_rate_mhz(), soundsinth::engine::kSampleRateHz);
    chain.out_started = true;
    return true;
}

// Движок поверх g_song - источником шины. Сброс лишних голосов при
// перегрузке включается здесь, а не в движке: TrackerEngine стартует с
// выключенным, чтобы тесты и pc_player оставались побитово
// воспроизводимыми. row - строки живого потока, у трека из файла nullptr.
void make_engine(AudioChain& chain, soundsinth::engine::TrackerEngine::LiveRowFn row, void* row_user) {
    const auto& s = player::config::settings();
    chain.engine  = new (s_engine_storage) soundsinth::engine::TrackerEngine(shared::g_song, shared::g_track_memory, s.mod_panning, s.mod_interpolation == 0);
    chain.track   = Track::Engine;
    chain.engine->set_tick_observer(&publish_tick_state, chain.engine);
    chain.engine->set_voice_cull_enabled(SOUNDSINTH_VOICE_CULL_ON_OVERLOAD != 0);
    if (row != nullptr) chain.engine->set_live_row_source(row, row_user);
    chain.mix->start_track();
    chain.mix->add_source(chain.engine->as_sound_source());
}

// Рендер без трека для источников машины (живые чипы, эффекты GS): без него
// им некуда играть, пока трека нет. Сборка движка его снимает, снос - снова
// заводит.
void idle_render_start(AudioChain& chain) {
    chain.mix->start_track();
    set_track_levels(*chain.mix, false, false, false, 0, 0);
    chain.track = Track::Idle;
    start_render(chain);
    debug_log("app_task: render without a track for the machine sources\n");
}

void idle_render_stop(AudioChain& chain) {
    if (chain.track != Track::Idle) return;
    hal::audio_out_set_silent(true);
    chain.render_task->stop();
    chain.render_task->~RenderTask();
    chain.render_task = nullptr;
    chain.pool->drain_rendered();
    chain.track = Track::None;
}

// Начало сборки трека: рендер без трека снят, база андерранов и запаса
// очереди готовых - для итога трека.
void begin_build(AudioChain& chain) {
    idle_render_stop(chain);
    chain.underrun_at_build = hal::audio_out_underruns();
    chain.ready_at_build    = hal::audio_out_ready_stats();
}

// Итог трека-журнала на сносе: отыгранные кадры, доля ядра на рендер,
// андерраны за трек и дробление рендера записями FM.
void log_vgm_track_end(const AudioChain& chain) {
    const uint32_t wall_us = platform::mono_us() - s_vgm.built_us;
    const uint32_t busy_us = shared::g_render_busy_us.load(std::memory_order_relaxed) - s_vgm.busy_us;
    // Кусков FM на 64 отсчёта, десятые: 10 - рендер не дробится записями.
    debug_logf("vgm: played %" PRIu32 " frames%s%s, render %" PRIu32 "%% of the core, underruns +%" PRIu32 ", FM pieces %" PRIu32 "/10 per 64\n", s_vgm.frames,
               s_vgm.log_ended ? ", the log ended" : "", s_vgm.player->broken() ? " at an unknown command" : "",
               static_cast<uint32_t>(static_cast<uint64_t>(busy_us) * 100u / (wall_us | 1u)), hal::audio_out_underruns() - chain.underrun_at_build,
               static_cast<uint32_t>(static_cast<uint64_t>(s_vgm.player->fm_pieces()) * 640u / (s_vgm.frames | 1u)));
    // Кэш флеша за трек: доля попаданий и промахи в секунду - мера того, что
    // горячему коду звука тесно во флеше.
    uint64_t acc = 0, hit = 0;
    hal::flash_cache_take(acc, hit);
    debug_logf("vgm: flash cache hits %" PRIu32 "/1000 of %" PRIu32 " K accesses, misses %" PRIu32 " K/s\n", static_cast<uint32_t>(hit * 1000u / (acc | 1u)),
               static_cast<uint32_t>(acc / 1000u), static_cast<uint32_t>((acc - hit) * 1000u / (wall_us | 1u)));
}

// Снос движка по смене трека. shared::g_teardown_requested взводят сброс от
// хоста и загрузка, которой мешает движок, безусловно: ESC тоже должен
// заглушить звук.
// RenderTask останавливается до remove_source() и добавления нового
// источника: MixBus::render() из фоновой задачи не синхронизирован с
// изменением списка источников. Вывод, BufferPool и MixBus живут всё время
// работы.
void teardown_engine(AudioChain& chain) {
    const uint32_t started_us = platform::mono_us();
    // Погасить выход за буфер вместо ступеньки в ноль посреди звука. Пауза,
    // а не ожидание события: при вставшем прерывании снос идёт дальше.
    chain.mix->fade_out();
    // И звук машины: ниже выход глушится целиком. Вернёт его set_muted
    // следующего трека или рендера без трека - нарастанием.
    chain.mix->set_muted(true);
    // Ждать факт затухания, а не отмерять время: на смене трека рендер и
    // не успевает - опоздал, и DMA повторяет прежний полногромкий буфер, а
    // тишина ложится поверх полной амплитуды. kFadeWaitMs остаётся
    // потолком: вставший рендер снос не подвешивает.
    //
    // Между шагами разбирается отвод AY: вход в живой режим идёт с
    // играющего трека, и всё время сноса кольцо не читает никто. На выходе
    // из режима разбор молчит сам - там очередь пишет тик движка.
    uint32_t waited = 0;
    auto wait_step  = [&waited] {
        platform::os_task_delay_ms(kPollBriskMs);
        waited += kPollBriskMs;
        live::feed_stream_before_session(platform::mono_ms());
    };
    while (waited < kFadeWaitMs && !chain.mix->faded()) {
        wait_step();
    }
    if (!chain.mix->faded()) {
        debug_logf("teardown: fade did not finish in %" PRIu32 " ms\n", waited);
    } else {
        // Затухший буфер ещё едет к ЦАП: доигрывающий, заряженный, очередь
        // готовых и буфер в руках рендера.
        for (uint32_t i = 0; i < kFadeBuffersAhead && waited < kFadeWaitMs; ++i) {
            wait_step();
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
    if (chain.track == Track::Vgm) log_vgm_track_end(chain);
#if SOUNDSINTH_AUDIO_DIAG == 2
    if (chain.track == Track::Engine) log_diag_track_end(chain);
#endif
    chain.mix->remove_source(chain.track == Track::Vgm ? &s_vgm.source : chain.engine->as_sound_source());
    if (chain.track == Track::Vgm && !s_vgm.live_back) live_chips_configure(true);
    // Явные деструкторы вместо delete: память статическая (s_engine_storage).
    chain.render_task->~RenderTask();
    // Секвенсор - после рендера: пока тот жив, он ждёт от него тика.
    uint32_t sequencer_stack_free = 0;
    uint32_t sequencer_ticks      = 0;
    if (chain.sequencer_task != nullptr) {
        chain.engine->set_tick_runner(nullptr, nullptr);
        chain.sequencer_task->stop();
        sequencer_stack_free = chain.sequencer_task->stack_unused_bytes();
        sequencer_ticks      = chain.sequencer_task->ticks();
        chain.sequencer_task->~SequencerTask();
        chain.sequencer_task = nullptr;
    }
    if (chain.engine != nullptr) chain.engine->~TrackerEngine();
    chain.render_task = nullptr;
    chain.engine      = nullptr;
    chain.track       = Track::None;
    // Рукопожатие перемотки - свойство снесённого трека: взятая отметка
    // тишины у нового трека дала бы второго писателя движка.
    chain.seek = SeekWait{};
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
               (platform::mono_us() - started_us) / 1000u, stop_us / 1000u, render_stack_free, sequencer_stack_free, sequencer_ticks, platform::mono_ms());
}

// Живой MIDI: движок поверх песни, которая рождается из потока. От файлового
// пути отличается тем, что грузить нечего - строки даёт поток, а сэмплы
// тянутся по заказам, - и тем, что конца у такого трека нет.
bool build_live_engine(AudioChain& chain, uint32_t ms) {
    if (!shared::try_mark_engine_alive()) return false; // идёт загрузка с шины
    if (!live::session_begin(ms)) {
        shared::g_engine_alive.store(false, std::memory_order_release);
        return false;
    }
    begin_build(chain);
    make_engine(chain, &live::live_row, live::row_user());
    // Живой поток не кончается; авторегулировка - только трекам из файла.
    set_track_levels(*chain.mix, shared::g_song.limiter_enabled, shared::g_song.soft_clip_enabled, false, 0, 0);
    start_render(chain);
    return true;
}

// Живой режим за проход задачи: вход по потоку, заказы сэмплов и выход по
// тишине. Трек из файла и живой поток - разные приложения: пошёл поток -
// файловое воспроизведение сносится, началась загрузка с шины - сносится
// живое.
void poll_live(AudioChain& chain) {
    // Выключен настройками - поток не слушаем вовсе. Отвод AY при этом
    // копится и теряется: разбирать его ради выброса дороже.
    if (player::config::settings().live_midi == static_cast<uint8_t>(soundsinth::config::LiveMidi::None)) return;
    // Под VGM живой MIDI стоит целиком, до конца журнала: процессор - плееру.
    if (chain.track == Track::Vgm && !s_vgm.live_back) return;
    uint32_t ms = platform::mono_ms();
    if (live::session_active()) {
        // Разбор молчащих нот - до подгрузки: после неё сэмпл, дочитанный
        // через миллисекунды после ноты, выглядел бы успевшим.
        if (chain.track == Track::Engine) live::note_missing(*chain.engine);
        // Подгрузка живёт в цикле другого ядра: здесь её вытеснял бы рендер.
        ms = platform::mono_ms();
        // Меню NMI и пауза стоят шину: события не выходят, и тишина
        // считается только после них - иначе меню дольше таймаута снесло бы
        // сеанс вместе с программами каналов.
        if (shared::g_nmi_mute.load(std::memory_order_relaxed) || shared::g_playback_paused.load(std::memory_order_relaxed)) chain.held_ms = ms;
        const bool loading = shared::g_song_generation.load(std::memory_order_acquire) != chain.seen_generation;
        const bool silent  = live::silent_ms(ms) >= SOUNDSINTH_LIVE_MIDI_TIMEOUT_MS && ms - chain.held_ms >= SOUNDSINTH_LIVE_MIDI_TIMEOUT_MS;
        if (loading || silent) {
            debug_logf("live: output - %s\n", loading ? "track from bus" : "silence");
            if (has_track(chain)) teardown_engine(chain);
            live::session_end();
        }
        return;
    }
    live::scan_stream_before_session(ms);
    if (!live::stream_detected()) return;
    // Идёт загрузка - вход запрещён, и сносить движок нельзя: собрать его
    // обратно всё равно не дадут, а трек уже не вернётся. Ждём следующего
    // прохода; счётчик событий снимается, чтобы порог брался заново.
    if (shared::g_load_in_progress.load(std::memory_order_acquire)) {
        live::session_end();
        return;
    }
    // Память трека режет begin(), а наполняет её фоновая догрузка на другом
    // ядре. Поэтому сначала просьба, и только по подтверждению Core1 - снос
    // движка и вход.
    if (!live::track_memory_granted()) {
        live::request_track_memory();
        return;
    }
    debug_log("live: MIDI stream started\n");
    // Снос и сборка - одним проходом: задачи звука не снимаются, а
    // паркуются, ждать освобождения их стеков больше нечего.
    if (has_track(chain)) {
        teardown_engine(chain);
        ms = platform::mono_ms(); // затухание сноса заняло время
    }
    if (!build_live_engine(chain, ms)) live::session_end();
}

// Трек-журнал VGM: источником шины встаёт плеер, движок и секвенсор не
// нужны. Лимитер и насыщение выключены, как у трекерных форматов.
void build_vgm(AudioChain& chain, vgm::Player* player) {
    s_vgm                   = VgmTrack{};
    s_vgm.player            = player;
    s_vgm.source.render_add = &vgm_render_add;
    s_vgm.built_us          = platform::mono_us();
    s_vgm.busy_us           = shared::g_render_busy_us.load(std::memory_order_relaxed);
    chain.track             = Track::Vgm;
    uint64_t acc = 0, hit = 0;
    hal::flash_cache_take(acc, hit); // итог трека - с нуля
    // Живые чипы под журнал стоят: нужные журналу плеер заводит свои.
    live_chips_configure(false);
    chain.mix->start_track();
    chain.mix->add_source(&s_vgm.source);
    set_file_track_levels(*chain.mix, false, false);
    start_render(chain);
    debug_logf("app_task: VGM track ready, t=%" PRIu32 " ms\n", platform::mono_ms());
}

// Перемотка на seek кадров ждёт, пока рендер уйдёт в тишину: до этого он
// может быть внутри вызова источника, и перематывать под ним нельзя. Счётчик
// тихих проходов - доказательство, что источники он уже не трогает. true -
// можно перематывать; после этого - seek_done().
bool seek_due(AudioChain& chain, uint32_t seek) {
    SeekWait& w = chain.seek;
    if (seek == 0) {
        w = SeekWait{};
        return false;
    }
    const uint32_t silent = chain.mix->silent_renders();
    if (!w.seen) {
        w.mark = silent;
        w.seen = true;
        return false;
    }
    if (silent == w.mark) {
        ++w.waits;
        return false;
    }
    return true;
}

// Гасится ровно взятое: нажатие, пришедшее во время прокрутки, останется и
// уйдёт следующим проходом.
void seek_done(AudioChain& chain, uint32_t seek) {
    shared::g_seek_frames.fetch_sub(seek, std::memory_order_acq_rel);
    chain.seek = SeekWait{};
}

// Кадр, до которого перематывать с кадра from: дальше конца трека некуда.
uint32_t seek_target(uint32_t from, uint32_t seek) {
    const uint32_t end = shared::g_track_end_frame.load(std::memory_order_relaxed);
    const uint32_t to  = from + seek;
    return end != 0 && (to > end || to < from) ? end : to;
}

// Транспорт - микшеру, решает другое ядро. Трек доиграл - выход гаснет
// затуханием, не тишиной в прерывании: та выбросила бы и уже затухший
// буфер; с концом затухания шина источник больше не зовёт. Доигравшему
// паузу не исполняем: при неизвестной длительности end_frame_ нулевой, и
// снятие паузы выпускает начало второго прохода. Меню NMI гасит выход, как
// пауза; перемотка ставит свою паузу поверх. Возвращает кадры перемотки,
// когда перематывать можно, иначе 0; после перемотки - seek_done().
uint32_t apply_transport(AudioChain& chain) {
    const uint32_t seek = shared::g_seek_frames.load(std::memory_order_relaxed);
    if (shared::g_playback_finished.load(std::memory_order_relaxed)) {
        chain.mix->finish();
    } else {
        chain.mix->set_paused(shared::g_playback_paused.load(std::memory_order_relaxed) || seek != 0);
    }
    chain.mix->set_muted(shared::g_nmi_mute.load(std::memory_order_relaxed));
    return seek_due(chain, seek) ? seek : 0;
}

// Состояние трека-журнала для другого ядра: позиция и конец.
void publish_vgm_state(AudioChain& chain) {
    shared::g_playback_frames.store(s_vgm.frames, std::memory_order_relaxed);
    // Длины в заголовке нет - трек кончается с журналом.
    if (s_vgm.log_ended && shared::g_track_end_frame.load(std::memory_order_relaxed) == 0) shared::g_song_ended.store(true, std::memory_order_relaxed);
    // Журнал кончился, а трек стоит до сноса (последний файл, повтор "один
    // раз"): живые чипы и MIDI - как в настройках. Накопленное в отводе за
    // трек - не поток, сеанс начинается с чистого.
    if (s_vgm.log_ended && !s_vgm.live_back) {
        s_vgm.live_back = true;
        live_chips_configure(true);
        live::session_reset();
        debug_log("vgm: log ended, live chips and MIDI are back\n");
    }
    const uint32_t seek = apply_transport(chain);
    if (seek == 0) return;
    const uint32_t from = s_vgm.frames, n = seek_target(from, seek) - from;
    s_vgm.player->skip(n);
    s_vgm.frames += n;
    chain.mix->skip_frames(n);
    debug_logf("seek: %" PRIu32 " -> %" PRIu32 " frame, waited for silence %" PRIu32 " pass%s\n", from, s_vgm.frames, chain.seek.waits,
               chain.seek.waits == 1u ? "" : "es");
    seek_done(chain, seek);
}

// Сборка движка поверх готового трека (новое поколение, движка нет).
void build_engine_if_ready(AudioChain& chain) {
    const uint32_t gen = shared::g_song_generation.load(std::memory_order_acquire);
    if (gen == chain.seen_generation || has_track(chain)) return;
    if (!shared::try_mark_engine_alive()) return; // идёт загрузка

    // Песню этого поколения бросил сброс хоста или переписала загрузка,
    // закончившаяся отказом: собирать нечего до следующего поколения.
    if (gen == shared::g_stale_generation.load(std::memory_order_relaxed)) {
        shared::g_engine_alive.store(false, std::memory_order_release);
        chain.seen_generation = gen;
        debug_log("app_task: track generation dropped by reset or load, engine not built\n");
        return;
    }
    chain.seen_generation = gen;
    begin_build(chain);
    shared::g_engine_generation.store(gen, std::memory_order_relaxed);
    if (vgm::Player* const log = shared::g_vgm.load(std::memory_order_relaxed); log != nullptr) {
        build_vgm(chain, log);
        return;
    }
    make_engine(chain, nullptr, nullptr);
    // Лимитер и мягкое насыщение включает формат трека.
    set_file_track_levels(*chain.mix, shared::g_song.limiter_enabled, shared::g_song.soft_clip_enabled);
    if (start_render(chain)) {
        debug_log("app_task: first track ready, audio out created+started\n");
    } else {
        debug_logf("app_task: next track ready, t=%" PRIu32 " ms\n", platform::mono_ms());
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
// голосов, конец песни.
void publish_playback_state(AudioChain& chain) {
    const soundsinth::engine::TrackerEngine& engine = *chain.engine;
    shared::g_playback_frames.store(engine.elapsed_frames(), std::memory_order_relaxed);
    shared::g_voice_cull_level.store(cull_level(chain.cull, engine, platform::mono_ms()), std::memory_order_relaxed);

    if (engine.song_ended()) {
        shared::g_song_ended.store(true, std::memory_order_relaxed);
    }
    const uint32_t seek = apply_transport(chain);
    if (seek == 0) return;

    const uint32_t from = engine.elapsed_frames();
    // Дальше конца трека прыгать некуда: там уже второй проход песни, и
    // прогон тиков туда только тратит время на то, что не прозвучит.
    const bool moved = chain.engine->seek_to_frame(seek_target(from, seek));
    // Прыжок - шине: кадр конца трека она считает своим счётчиком, а
    // пропущенное мимо неё прошло.
    if (moved) chain.mix->skip_frames(chain.engine->elapsed_frames() - from);
    debug_logf("seek: %" PRIu32 " -> %" PRIu32 " frame%s, waited for silence %" PRIu32 " pass%s\n", from, chain.engine->elapsed_frames(),
               moved ? "" : " (refused)", chain.seek.waits, chain.seek.waits == 1u ? "" : "es");
    seek_done(chain, seek);
}

} // namespace

void live_chips_configure(bool on) {
    const auto& s = player::config::settings();
    live::ay_live_configure(on ? s.ay_emulation : 0, soundsinth::config::kAyClockHz, s.ay_stereo);
    live::saa_live_configure(on && s.saa1099 != 0);
    live::dac_live_configure(on && s.covox != 0, on && s.soundrive != 0, on ? s.beeper : 0u);
}

void audio_chain_task(void* /*arg*/) {
    debug_log("app_task: entered\n");

    AudioChain chain;
    chain.pool = new (s_pool_storage) audio::BufferPool();
    debug_log("app_task: BufferPool ready\n");

    soundsinth::mixbus::MixBus& mix = *new (s_mix_storage) soundsinth::mixbus::MixBus();
    chain.mix                       = &mix;
    // Эффекты GS - свой источник, и он заводится один раз на всю работу:
    // список источников меняется только при снятом рендере, а сэмплы
    // переживают смену трека. Пока эффектов нет, источник молчит.
    // Звук машины - мимо паузы, конца и уровня трека.
    mix.add_machine_source(gs::fx_player().as_sound_source());
    mix.add_machine_source(live::ay_live_source());
    mix.add_machine_source(live::saa_live_source());
    mix.add_machine_source(live::dac_live_source());
    mix.set_meter(&s_meter);
    log_spectrum_cost();
    debug_log("app_task: MixBus ready, waiting for first track\n");

    for (;;) {
        shared::g_app_task_loops.store(shared::g_app_task_loops.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        hal::flash_cache_poll();
        // Просьба гасится и без движка: иначе снесла бы собранный позже.
        const bool teardown = shared::g_teardown_requested.exchange(false, std::memory_order_acq_rel);
        if (teardown && has_track(chain)) {
            teardown_engine(chain);
            live::session_reset(); // сброс с шины гасит и живое
        } else {
            if (teardown) live::session_reset(); // трека нет: поток до сброса забыт
            // Поток слушается и под играющий трек: пошёл живой MIDI - трек
            // сносится. Сборка трека с шины - только пока живого нет.
            if (!live::session_active()) build_engine_if_ready(chain);
            poll_live(chain);
        }
        if (chain.track == Track::Engine) {
            publish_playback_state(chain);
            // Эффекты General Sound отнимают канал у модуля, как у карты:
            // маску занятых каналов держит мост, а глушит их движок.
            chain.engine->set_channel_mute(gs::bridge_fx_mute_mask());
            // Громкость модуля GS (#2A) - усилением трека, как у карты.
            chain.mix->set_track_gain(gs::bridge_track_gain());
#if SOUNDSINTH_AUDIO_DIAG >= 1
            log_underruns(chain.engine->active_voice_count());
#endif
#if SOUNDSINTH_AUDIO_DIAG == 2
            log_diag(chain);
#endif
        }
        const uint32_t meter_wait = publish_meters(platform::mono_ms());
        if (chain.track == Track::Vgm) {
            publish_vgm_state(chain);
#if SOUNDSINTH_AUDIO_DIAG >= 1
            log_underruns(0);
#endif
        }
        // Под живой режим и в ожидании потока - чаще: заказанные сэмплы ждут
        // этот же проход. С играющим треком реже, но не дольше ёмкости
        // отвода: поток начинается, пока трек ещё звучит, и первые записи
        // ждут в кольце отвода.
        const bool machine = live::ay_live_enabled() || live::saa_live_enabled() || live::dac_live_enabled() || gs::fx_player().wants_render();
        // Заведённый остаётся до трека: эффекты идут подряд, заводить на каждый
        // накладно.
        if (chain.track == Track::None && machine) idle_render_start(chain);
        // Меню NMI: звук машины без трека смолкает так же, как с треком.
        if (chain.track == Track::Idle) chain.mix->set_muted(shared::g_nmi_mute.load(std::memory_order_relaxed));
        const bool brisk     = live::session_active() || !has_track(chain);
        const uint32_t pause = brisk ? kPollBriskMs : kPollIdleMs;
        platform::os_task_delay_ms(meter_wait < pause ? meter_wait : pause);
    }
}

} // namespace player
