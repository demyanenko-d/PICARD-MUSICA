// SPDX-License-Identifier: MIT
#pragma once

// mixbus::SoundSource, играющий Song: секвенсор, диспетчер эффектов и
// голоса. Один голос на канал (channel_count) плюс пул фоновых голосов NNA
// в свободном конце тех же массивов, всего до SOUNDSINTH_MAX_SLOTS
// записей; звучащих одновременно не больше SOUNDSINTH_MAX_VOICES.
// Общий код для прошивки и PC.

#include <cstdint>
#include <type_traits>

#include "platform/compiler.h"
#include "core/config.h"
#include "core/engine/effect_dispatch.h"
#include "core/engine/resonant_filter.h"
#include "core/engine/reverb.h"
#include "core/engine/sequencer.h"
#include "core/engine/voice.h"
#include "core/engine/voice_arbiter.h"
#include "core/engine/voice_link.h"
#include "core/engine/voice_mixer.h"
#include "core/model/song.h"
#include "core/memory/track_memory.h"
#include "core/audio/sound_source.h"

namespace soundsinth::engine {

class TrackerEngine {
public:
    // song и mem хранятся по ссылке и должны жить не меньше TrackerEngine.
    TrackerEngine(const soundsinth::model::Song& song, memory::TrackMemory& mem);

    mixbus::SoundSource* as_sound_source() { return &iface_; }

    bool song_ended() const { return ps_.song_ended; }

    // Отрендерено выходных кадров с момента создания. Сам не сбрасывается;
    // uint32_t при 44100 Гц переполняется через 27 часов.
    uint32_t elapsed_frames() const { return frames_rendered_; }

    // Диагностика: поканальный экспорт для сверки с другими плеерами. ch < 0 -
    // все каналы (умолчание); ch >= 0 - в сведение идут только голоса этого
    // канала. Голоса остальных каналов считаются как обычно и сводятся в
    // discard (2 x discard_frames отсчётов), в реверберацию не посылают;
    // сумма поканальных рендеров равна полному. Вызов render_add длиннее
    // discard_frames режется на куски. Без discard solo не включается.
    void set_solo_channel(int32_t ch, int32_t* discard, uint32_t discard_frames) {
        const bool ok        = ch >= 0 && discard != nullptr && discard_frames > 0;
        solo_channel_        = ok ? ch : -1;
        solo_discard_        = ok ? discard : nullptr;
        solo_discard_frames_ = ok ? discard_frames : 0;
        mixer_.discard_l     = solo_discard_;
        mixer_.discard_r     = ok ? discard + discard_frames : nullptr;
    }

    // Сколько слотов сводится сейчас, по rebuild_active_indices(): звучащие
    // голоса каналов и пула NNA плюс гаснущие. Больше потолка полифонии
    // бывает: потолок про звучащих, а гаснущие доигрывают сверх него. Звать с
    // того же ядра, что и render.
    uint32_t active_voice_count() const { return voice_count_; }

    // Максимум active_voice_count() с создания движка, с учётом
    // фоновых голосов NNA. Обновляется раз за тик в rebuild_active_indices().
    uint32_t peak_active_voice_count() const { return peak_active_count_; }

    // Упор в потолок SOUNDSINTH_MAX_VOICES: слотов, которые читают PSRAM
    // (живой голос, фоновый NNA, волновое гашение), бывает до 96. Фоновый NNA
    // сверх потолка гасится (tails_dropped - сколько за трек); не влезшее
    // волновое гашение .mid ждёт места. unlisted - сумма по тикам слотов вне
    // списка, demand_peak - наибольшее число читающих слотов за тик.
    uint32_t voices_unlisted() const { return voices_unlisted_; }
    uint32_t voice_demand_peak() const { return voice_demand_peak_; }
    uint32_t tails_dropped() const { return arbiter_.tails_dropped(); }
    // Волновое гашение .mid занимает слот умершего голоса на ramp_samples
    // отсчётов. Понадобился слот раньше - хвост обрывается: started сколько
    // начато, cut_note сколько убито новой нотой слота, cut_move переездом
    // голоса NNA.
    uint32_t wave_tail_started() const { return mixer_.wave_tail_started; }
    uint32_t wave_tail_cut_note() const { return mixer_.wave_tail_cut_note; }
    uint32_t wave_tail_cut_move() const { return mixer_.wave_tail_cut_move; }
    // Краж слота NNA при полном пуле за трек.
    uint32_t nna_steals() const { return arbiter_.steals(); }
    // Нот, чей сэмпл не в памяти (не догружен или вытеснен), за трек: такая
    // нота молчит.
    uint32_t triggers_without_sample() const { return triggers_without_sample_; }
    // Сэмплы последних таких нот: по ним живой режим поднимает историю записи
    // и говорит, почему PCM не оказалось. Кольцо, а не одно число: ноты
    // пропадают пачками, а разбор идёт из другой задачи и видел бы одну.
    static constexpr uint32_t kMissingRing = 16;
    uint16_t missing_sample(uint32_t n) const { return missing_ring_[n % kMissingRing]; }

    // Диагностика всплесков на плотных строках. max_tick_duration_us() - самый
    // дорогой один тик advance_tick() (только RP2350, на PC всегда 0).
    // *_at_worst_tick() - активных голосов после него и voice_trigger() внутри
    // него. max_voice_triggers_per_tick() - максимум триггеров за любой тик,
    // может не совпадать с самым долгим.
    uint32_t max_tick_duration_us() const { return max_tick_duration_us_; }
    uint32_t active_voice_count_at_worst_tick() const { return worst_tick_active_count_; }
    uint32_t voice_triggers_at_worst_tick() const { return worst_tick_triggers_; }
    // Самый дорогой тик - тик 0 строки: разбор строки (флеш) против потиковых
    // эффектов (SRAM).
    bool worst_tick_at_row_start() const { return worst_tick_row_start_; }
    uint32_t max_voice_triggers_per_tick() const { return max_voice_triggers_per_tick_; }

    // Время цикла по голосам (на PC 0) и число голос-отсчётов; среднее на
    // голос-отсчёт в нс: voice_loop_total_us() * 1000 / voice_sample_count().
    uint32_t voice_loop_total_us() const { return voice_loop_total_us_; }
    uint64_t voice_sample_count() const { return voice_sample_count_; }

    // Загрузка рендера в Q8 (256 - буфер считался столько, сколько играет),
    // скользящее среднее за ~8 буферов (46 мс). Меряется весь render_add:
    // перегрузку дают и секвенсор, и фильтр. На PC 0.
    uint32_t render_load_q8() const { return load_ewma_q8_; }

    // Наибольшее заполнение кольца команд за трек и сколько раз оно
    // переполнялось (тогда команды разбираются прямо на записи).
    uint32_t voice_queue_peak() const { return link_.queue_peak(); }
    uint32_t voice_queue_full() const { return link_.queue_full(); }
    // ВРЕМЕННО: расхождения модели звучания с микшером.

    // Потолок полифонии и число сброшенных голосов за трек.
    uint8_t voice_budget() const { return arbiter_.budget(); }
    uint32_t voices_culled() const { return arbiter_.culled(); }

    // Сброс лишних голосов при перегрузке. По умолчанию выключен; прошивка
    // включает его по SOUNDSINTH_VOICE_CULL_ON_OVERLOAD, на PC он выключен
    // ради побитово воспроизводимого выхода.
    void set_voice_cull_enabled(bool on) { arbiter_.set_cull_enabled(on); }

    // Без часов (ПК) загрузку можно считать по модели цены голоса
    // (SOUNDSINTH_VOICE_COST_*): сумма цены звучащих голосов против
    // длительности буфера. Так --voice-cull режет голоса, как плата. На плате
    // ветки нет.
    void set_model_load_enabled(bool on) { model_load_enabled_ = on; }

    // Только для тестов. На PC загрузку не измерить, логика бюджета и сброса
    // иначе не проверяется: задаёт load_ewma_q8_ напрямую.
    void set_overload_hint_pct(uint32_t percent) { load_ewma_q8_ = pct_to_q8(percent); }

    // Раз за тик в конце advance_tick(), в потоке рендера: позиция в order и
    // сэмплы звучащих голосов - по ним загрузка решает, что можно вытеснить из
    // PSRAM. sample_indices - все слоты, читающие PSRAM (их может быть больше
    // active_voice_count()), живут только на время вызова.
    using TickObserver = void (*)(void* user, uint16_t order_pos, const uint16_t* sample_indices, uint8_t count);
    // Строка паттерна для команды GS #61 (Get Pattern Position).
    uint16_t current_row() const { return ps_.row; }
    // Где стоит секвенсор целиком. Только для чтения: этим сверяется
    // перемотка против обычной игры.
    const PlayState& play_state() const { return ps_; }

    // Тик: строка, эффекты, арбитр и команды голосам. Обычно его делает сам
    // рендер между батчами; можно вынести в свою задачу - тогда рендер зовёт
    // её через tick_runner и ждёт здесь же. Очередь строгая: забегать вперёд
    // нельзя, арбитр судит о голосах по тени, а её публикует микшер раз в
    // батч. Замер самого дорогого тика идёт внутри, поэтому ожидание задачи в
    // него не попадает.
    void run_tick();

    // Перемотка вперёд: тики прогоняются без сведения, поэтому состояние
    // каналов (инструменты, громкости, панорама, бенд, темп) приходит в
    // точку перемотки тем же путём, что при обычной игре - прыжком по
    // строкам его не восстановить.
    //
    // Голоса после прогона снимаются: запущенные по дороге ноты не
    // рендерились и стоят в начале сэмпла, без снятия они ударили бы разом.
    //
    // false - перематывать некуда: точка позади текущей, песня кончилась
    // или строки идут из живого потока. Назад не умеет: секвенсор к нулю не
    // возвращается, для этого движок пересоздают.
    //
    // Тик полноценный, на минуту .mid их тысячи. Звать при погашенном выходе.
    bool seek_to_frame(uint32_t target_frames);

    using TickRunner = void (*)(void* user);
    void set_tick_runner(TickRunner fn, void* user) {
        tick_runner_      = fn;
        tick_runner_user_ = user;
    }

    // Живой источник строк: строку на каждый тик даёт он, а не паттерны.
    // Порядка, паттернов и длины трека у такой песни нет. nullptr в ответе -
    // строка без событий. Ставится до первого рендера.
    using LiveRowFn = const soundsinth::model::PatternCell* (*)(void* user);
    void set_live_row_source(LiveRowFn fn, void* user) {
        live_row_      = fn;
        live_row_user_ = user;
        // У живой песни нет ни одной воспроизводимой позиции order, и
        // sequencer_init объявил её законченной. Живой режим играет
        // бесконечно: строки идут из потока.
        if (fn != nullptr) ps_.song_ended = false;
    }

    void set_tick_observer(TickObserver fn, void* user) {
        tick_observer_      = fn;
        tick_observer_user_ = user;
    }

private:
    static void render_add(void* self, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames);
    static void row_callback(void* user, const soundsinth::model::PatternCell* cells, uint8_t channel_count);
    // DispatchContext::on_note_trigger_nna: static-обёртка над
    // handle_note_trigger_nna, self приходит через void* user (как у
    // row_callback и render_add).
    static void on_note_trigger_nna(void* user, uint8_t channel, uint16_t new_instrument_1based, uint16_t new_sample_index, uint8_t new_resolved_note);

    void advance_tick();
    // Шаги тика после непрерывных эффектов, по порядку: огибающие фоновых
    // голосов NNA, Retrigger и NoteCut, перенос высоты в голоса.
    void advance_nna_slot_envelopes(uint32_t envelope_time_step_q8);
    void apply_retrigger_and_note_cut();
    void sync_voice_pitch();
    // Общий шаг после dispatch_row_effects и NoteDelay, раз за тик из
    // advance_tick. Диспетчер про Voice не знает и только выставляет
    // ChannelState::triggered_this_row/stop_voice_pending; здесь по ним
    // запускается голос (trigger_voice) или останавливается mixer_.voices[ch].
    void sync_voices_after_dispatch();
    // Запуск голоса канала ch с отсчёта start_offset: правило выключенного
    // канала, сэмпл из каталога, voice_trigger, счётчик триггеров, антиклик,
    // сброс фильтра. Одна точка для нот строки, NoteDelay и Retrigger.
    // note - запуск ноты (строка, NoteDelay), а не Retrigger: фильтр IT с
    // открытым срезом снимается только на ноте.
    void trigger_voice(uint8_t ch, uint32_t start_offset, bool note);
    // Реализация DispatchContext::on_note_trigger_nna: поиск DCT/DCA (только
    // среди хвостов этого же канала) и решение, уводить ли старый голос канала
    // в фон, по NewNoteAction его инструмента. Вызывается из
    // dispatch_row_effects до того, как новая нота перезапишет
    // channels_[channel]/mixer_.voices[channel].
    void handle_note_trigger_nna(uint8_t channel, uint16_t new_instrument_1based, uint16_t new_sample_index, uint8_t new_resolved_note);

    // Пересобирает mixer_.active/mixer_.active_count раз за тик (в конце
    // advance_tick()), не за отсчёт. render_add() ходит по этому списку, а не
    // по 0..channel_count_+pool_size_ на каждый отсчёт: иначе цена рендера
    // росла бы с числом объявленных в файле каналов, а не играющих голосов
    // (IT-файлы с формально большим channel_count).
    // Голос может умереть посреди тика (voice_render() гасит voice.active на
    // конце незацикленного сэмпла); список досрочно не пересобирается, мёртвая
    // запись до следующей пересборки обходится дешёвым холостым вызовом.
    // Активный голос список не пропускает: все voice_trigger происходят внутри
    // advance_tick(), до вызова этого метода.
    void rebuild_active_indices();

    mixbus::SoundSource iface_;
    const soundsinth::model::Song& song_;
    memory::TrackMemory& mem_;
    uint8_t channel_count_;

    PlayState ps_;
    ChannelState channels_[SOUNDSINTH_MAX_SLOTS];
    // Звуковая часть: голоса, сглаживание, фильтры, усиления и список на тик.
    VoiceMixer mixer_;
    // Связь с ней: команды вниз, наверх ничего.
    VoiceLink link_;
    // Кому звучать: пул NNA, хвосты сверх потолка, бюджет по загрузке.
    VoiceArbiter arbiter_;
    DispatchContext dispatch_ctx_;

    // Пул NNA - в конце тех же массивов, за каналами.
    static constexpr uint8_t kNnaPool = VoiceArbiter::kPool;

    // Сколько выходных отсчётов осталось до следующего advance_tick(). 0 после
    // конструктора: первый же отсчёт сначала вызывает advance_tick().
    uint32_t samples_until_next_tick_ = 0;

    // Тик 0 строки 0 идёт без сдвига секвенсора: строку уже прочитал
    // sequencer_init. Без флага строка 0 звучит speed-1 тиков, трек на тик впереди.
    bool first_tick_pending_ = true;

    // Постоянный множитель сведения, Q0.16: 1.0 у обычных треков и
    // 64/PreAmpTable[каналов/2] у сделанных в ModPlug Tracker (Song::mix_levels).
    // Считается один раз на старте: число каналов и режим по ходу трека не
    // меняются.
    uint32_t master_gain_q16_ = kQ16One;

    // Список играющих голосов - mixer_.active: 0..channel_count_-1 - каналы,
    // дальше слоты NNA. Причина и инварианты - у rebuild_active_indices().
    uint8_t voice_count_ = 0; // из них с voice_render, не больше SOUNDSINTH_MAX_VOICES
    // Сэмплы слотов, которые читают PSRAM на этом тике, - для TickObserver.
    uint16_t sample_map_[SOUNDSINTH_MAX_SLOTS];
    uint8_t sample_map_count_            = 0;
    uint32_t voices_unlisted_            = 0;
    uint32_t voice_demand_peak_          = 0;
    uint32_t triggers_without_sample_    = 0;
    uint16_t missing_ring_[kMissingRing] = {};
    TickObserver tick_observer_          = nullptr;
    LiveRowFn live_row_                  = nullptr;
    void* live_row_user_                 = nullptr;
    TickRunner tick_runner_              = nullptr;
    void* tick_runner_user_              = nullptr;
    void* tick_observer_user_            = nullptr;
    uint32_t peak_active_count_          = 0;

    // Счётчик тика обнуляется в начале advance_tick(); остальные - максимумы с
    // создания движка.
    uint32_t voice_triggers_this_tick_    = 0;
    uint32_t max_voice_triggers_per_tick_ = 0;
    uint32_t max_tick_duration_us_        = 0;
    uint32_t worst_tick_active_count_     = 0;
    uint32_t worst_tick_triggers_         = 0;

    uint32_t voice_loop_total_us_ = 0;
    uint64_t voice_sample_count_  = 0;

    uint32_t load_ewma_q8_ = 0;

    bool model_load_enabled_ = false; // только без часов: загрузка по модели цены голоса

    // Антиклик. Громкость считается раз в тик, ступенька на каждой строке
    // слышна как щелчок и видна в рендере разрывами по сетке строк. Здесь
    // громкость доезжает до нового значения за ramp_samples_ отсчётов.
    //
    // Сглаживаются только первые ramp_samples_ отсчётов после изменения,
    // остальной батч идёт с постоянной громкостью прежним кодом. При тике в
    // 1200 отсчётов это около 4% из них.
    // Длина сглаживания: из песни, если она задала свою (Song::volume_ramp_samples),
    // иначе SOUNDSINTH_VOLUME_RAMP_SAMPLES.
    uint32_t ramp_samples_ = 0;     // ставится в конструкторе: из Song или config.h
    bool wave_tail_        = false; // волновое гашение вместо замороженного отсчёта (только .mid)
    bool envelope_db_      = false; // огибающая громкости в децибелах (kQuirkEnvelopeDecibel, .mid)

    // Длина сглаживания и гашения не больше kMaxRampSamples: счётчики - байт.
    static constexpr uint32_t kMaxRampSamples = UINT8_MAX;
    static_assert(SOUNDSINTH_VOLUME_RAMP_SAMPLES <= kMaxRampSamples, "VoiceRamp counters are uint8_t");

    // Коэффициенты фильтра пересчитываются раз в тик вместе с громкостями
    // (rebuild_active_indices), состояние живёт в mixer_ и сбрасывается на
    // триггере ноты.
    // Входы filter_compute на прошлом тике одним ключом: cutoff | resonance << 8
    // | uint16(env_modifier) << 16. Совпал - коэффициенты верны, пересчёт не
    // нужен. cutoff и resonance 0..127, env_modifier -256..1784 и кратен 8:
    // маркеры ниже ключом не бывают.
    static constexpr uint32_t kFilterMemoNone        = 0xFFFFFFFFu;
    static constexpr uint32_t kFilterMemoNoteTrigger = 0xFFFFFFFEu; // нота запущена после прошлого расчёта (не Retrigger)
    uint32_t filter_memo_[SOUNDSINTH_MAX_SLOTS];

    // Голос idx в список играющих этого тика.
    SOUNDSINTH_ALWAYS_INLINE void push_active(uint8_t idx) { link_.list_push(idx); }
    // Усиления, сглаживание громкости и коэффициенты фильтра голоса idx на этот тик.
    void update_voice_mix(uint8_t idx, uint32_t global_vol_q24);
    // Посыл в ревербератор и маршрут (solo) голосов списка - для mixer_, раз в тик.
    void update_voice_routes();

    uint32_t frames_rendered_ = 0;

    // Общий ревербератор и его шина - только при song.reverb_enabled (.mid), в
    // буфере сценариев трека. У трекерных форматов nullptr: посылов
    // нет, и этот путь не выполняется.
    Reverb* reverb_      = nullptr;
    int32_t* reverb_bus_ = nullptr;

    int32_t solo_channel_         = -1;
    int32_t* solo_discard_        = nullptr;
    uint32_t solo_discard_frames_ = 0;
    // В конце: поля горячего пути не сдвигаются.
    bool worst_tick_row_start_ = false;
};

// Движок пересоздаётся в статическом хранилище размещающим new: член с
// кучей вернул бы malloc в прошивку.
static_assert(std::is_trivially_destructible_v<TrackerEngine>, "TrackerEngine has no memory of its own");

} // namespace soundsinth::engine
