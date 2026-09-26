#include "core/engine/tracker_engine.h"

#include <cstdlib> // std::abs
#include <cstring>
#include <new>

#include "platform/clock.h"
#include "platform/hot_path.h"
#include "core/engine/engine_defs.h"
#include "core/engine/envelope_gain.h"
#include "core/model/amiga_period.h"
#include "core/engine/voice_mixer.h"


namespace soundsinth::engine {

namespace {

// Q0.16 (65536 = 1.0) - без float на каждый голос и каждый отсчёт.
// int64_t в промежуточном умножении: громкость канала Q0.16 на общую в
// масштабе шины (kGainQ24Bits) доходит до 2^41.
// Округление - усечение: прибавка 0x8000 до сдвига даёт UMLAL+ADC вместо
// одного UMULL.

// Шкалы громкости в Q0.16, оба множителя точные.
constexpr uint32_t kVol64ToQ16 = kQ16One / 64; // 0..64
constexpr uint32_t kVol128ToQ16 = kQ16One / 128; // 0..128

// Сглаживание загрузки рендера: новому замеру - 1/8.
constexpr uint32_t kLoadEwmaShift = 3;

// Жилец Scratch::Play: ревербератор и за ним его шина. Смещение шины
// считает компилятор.
struct PlayScratch {
    Reverb reverb;
    int32_t bus[SOUNDSINTH_AUDIO_BUFFER_FRAMES];
};
static_assert(alignof(PlayScratch) <= 8, "буфер сценариев выровнен на 8");
static_assert(sizeof(PlayScratch) <= memory::kPlayScratchBytes, "ревербератор с шиной не влезает в свой сценарий");

uint32_t q16_mul(uint32_t a_q16, uint32_t b_q16) {
    return static_cast<uint32_t>((static_cast<uint64_t>(a_q16) * b_q16) >> kQ16Bits);
}

// Glissando: звучащая высота ступенями по полутонам, а period/linear_pitch
// едут плавно, как в OpenMPT.
// Действует при glissando_enabled и тон-портаменто на этой ноте
// (glissando_porta); у ProTracker (kQuirkGlissandoPtMode) - только на
// строке с тон-портаменто и не на её первом тике.
bool glissando_snaps(const ChannelState& cs, soundsinth::model::QuirkFlags quirks, uint16_t tick_in_row) {
    if (!cs.glissando_enabled) return false;
    if (quirks & soundsinth::model::kQuirkGlissandoPtMode) return cs.tone_porta_active && tick_in_row != 0;
    return cs.glissando_porta;
}

// Панорама по корню, как у FastTracker II (PanLaw): 8192*sqrt(q) для
// q = 0..64 - кривая панорамы FT2 в шкале 0..64 вместо 0..256.
constexpr uint32_t kFt2PanTable[65] = {
    0,     8192,  11585, 14189, 16384, 18318, 20066, 21674, 23170, 24576, 25905, 27170, 28378,
    29537, 30652, 31727, 32768, 33776, 34756, 35708, 36636, 37540, 38424, 39287, 40132, 40960,
    41771, 42567, 43348, 44115, 44869, 45611, 46341, 47059, 47767, 48465, 49152, 49830, 50499,
    51159, 51811, 52454, 53090, 53719, 54340, 54954, 55561, 56162, 56756, 57344, 57926, 58503,
    59073, 59639, 60199, 60753, 61303, 61848, 62388, 62924, 63455, 63982, 64504, 65022, 65536,
};

// Множители L/R одного голоса (единица 1 << kGainQ24Bits) - живого канала или
// фонового NNA, формула одна. Все входы меняются не чаще раза в тик: считаются
// раз за тик из rebuild_active_indices() в mixer_.gain_l_q24/gain_r_q24.
void SOUNDSINTH_HOT_PATH(compute_mix_gains)(const ChannelState& cs, uint32_t global_vol_q24,
                                            soundsinth::model::Song::PanLaw pan_law, bool envelope_db,
                                            int32_t* gain_l_q24, int32_t* gain_r_q24) {
    uint32_t channel_vol_q16 = 0;
    if (!cs.tremor_muted) { // Tremor глушит канал на этом тике целиком, поверх остальных множителей
        int32_t vol =
            static_cast<int32_t>(cs.volume) + cs.volume_offset; // Tremolo - временное отклонение, cs.volume не тронут
        if (vol < 0) vol = 0;
        if (vol > 64) vol = 64;
        // Множители Q0.16: громкость, огибающая, громкость канала и сэмпла -
        // 0..64, затухание - Q16.16, громкость инструмента IT - 0..128; огибающая
        // .mid - в децибелах (таблица).
        channel_vol_q16 = q16_mul(static_cast<uint32_t>(vol) * kVol64ToQ16, envelope_gain_q16(cs, envelope_db));
        channel_vol_q16 = q16_mul(channel_vol_q16, static_cast<uint32_t>(cs.channel_volume) * kVol64ToQ16);
        channel_vol_q16 = q16_mul(channel_vol_q16, cs.fadeout_level);
        channel_vol_q16 = q16_mul(channel_vol_q16, static_cast<uint32_t>(cs.instrument_global_volume) * kVol128ToQ16);
        channel_vol_q16 = q16_mul(channel_vol_q16, static_cast<uint32_t>(cs.sample_global_volume) * kVol64ToQ16);
    }
    const uint32_t combined_vol_q24 = q16_mul(channel_vol_q16, global_vol_q24);
    // Панорама 0..64, 32 - центр. Законы: линейный (IT, MOD, S3M), корень FT2
    // (почти все XM), корень без особого правого края (.mid).
    // Огибающая панорамы (pan_envelope_value, 0..64, 32 - нейтрально) -
    // смещение, а не множитель, с глубиной, убывающей к краям:
    // finalpan = pan + (env - 32) * depth / 32, depth максимальна в центре и
    // нулевая на краях - иначе огибающая выталкивала бы панораму за 0..64.
    // Panbrello (pan_offset) - ещё одно временное смещение, без глубины к
    // краям. Арифметика целая: промежуточно
    // *32, в конце >>5.
    const int32_t pan_depth_x32 = 32 - std::abs(static_cast<int32_t>(cs.pan) - 32); // 0..32
    const int32_t pan_envelope_offset_x32 = (static_cast<int32_t>(cs.pan_envelope_value) - 32) * pan_depth_x32;
    int32_t effective_pan =
        static_cast<int32_t>(cs.pan) + (pan_envelope_offset_x32 >> 5) + static_cast<int32_t>(cs.pan_offset);
    if (effective_pan < 0) effective_pan = 0;
    if (effective_pan > 64) effective_pan = 64;
    uint32_t pan_r_q16 = static_cast<uint32_t>(effective_pan) * kVol64ToQ16;
    uint32_t pan_l_q16 = kQ16One - pan_r_q16;
    switch (pan_law) {
        case soundsinth::model::Song::PanLaw::Sqrt:
            // Постоянная мощность без квирка FT2: край есть край, та же таблица без
            // особого случая на правом конце.
            pan_r_q16 = kFt2PanTable[effective_pan];
            pan_l_q16 = kFt2PanTable[64 - effective_pan];
            break;
        case soundsinth::model::Song::PanLaw::Ft2Sqrt:
            if (effective_pan >= 64) {
                // FT2 до конца вправо не доводит: его панорама 0..255, и крайнее значение
                // оставляет слева ненулевой остаток.
                pan_l_q16 = 4096;
                pan_r_q16 = 65408;
            } else {
                pan_r_q16 = kFt2PanTable[effective_pan];
                pan_l_q16 = kFt2PanTable[64 - effective_pan];
            }
            break;
        case soundsinth::model::Song::PanLaw::Linear:
            break;
    }
    // Громкость и панорама свёрнуты в одно число здесь, раз за тик: на отсчёт
    // остаётся одно умножение на канал.
    *gain_l_q24 = static_cast<int32_t>(q16_mul(combined_vol_q24, pan_l_q16));
    *gain_r_q24 = static_cast<int32_t>(q16_mul(combined_vol_q24, pan_r_q16));

    // Surround (S91): правый канал в противофазе, как в OpenMPT. Панорама такого
    // канала центральная (её ставит S91), левое и правое усиления равны по
    // модулю. На отсчёт это бесплатно:
    // SMLAWB знаковый, множитель просто отрицательный.
    if (cs.surround) {
        *gain_r_q24 = -*gain_l_q24;
    }
}


// Скользящее среднее загрузки: новому замеру - 1/2^kLoadEwmaShift.
// Знаковое: разность бывает отрицательной, а сдвиг отрицательного uint32 дал
// бы мусор.
void smooth_load(uint32_t& load_ewma_q8, uint32_t load_q8) {
    const int32_t prev = static_cast<int32_t>(load_ewma_q8);
    const int32_t next = prev + ((static_cast<int32_t>(load_q8) - prev) >> kLoadEwmaShift);
    load_ewma_q8 = static_cast<uint32_t>(next < 0 ? 0 : next);
}

// Загрузка рендера: время на буфер против времени его звучания. По всему
// рендеру, а не по циклу голосов: перегрузку дают и секвенсор на границе
// тика, и фильтр, и сведение. Порядок в числителе - без переполнения и
// потери точности: dt (мкс, для буфера в 256 кадров ~5000) * 256 * 44100
// (11.3 млн) влезает в uint64. Постоянная среднего ~8 буферов (~46 мс):
// замечает плотный кусок и не дёргается от одного дорогого буфера.
void update_render_load(uint32_t& load_ewma_q8, uint32_t render_t0_us, uint32_t n_frames) {
    const uint32_t dt_us = platform::time_us() - render_t0_us;
    smooth_load(load_ewma_q8, static_cast<uint32_t>((static_cast<uint64_t>(dt_us) * kQ8One * kSampleRateHz) /
                                                    (static_cast<uint64_t>(n_frames) * 1000000u)));
}

} // namespace

TrackerEngine::TrackerEngine(const soundsinth::model::Song& song, memory::TrackMemory& mem)
    : song_(song)
    , mem_(mem)
    , channel_count_(song.channel_count > SOUNDSINTH_MAX_VOICES ? SOUNDSINTH_MAX_VOICES : song.channel_count) {
    ramp_samples_ =
        song.volume_ramp_samples ? song.volume_ramp_samples : static_cast<uint32_t>(SOUNDSINTH_VOLUME_RAMP_SAMPLES);
    if (ramp_samples_ > kMaxRampSamples) ramp_samples_ = kMaxRampSamples;
    // Волновое гашение включает только формат, который сам задал длину
    // сглаживания, то есть .mid. У трекерных форматов на кону побитовая сверка с
    // эталонными плеерами.
    wave_tail_ = song.volume_ramp_samples != 0;
    envelope_db_ = (song.quirks & soundsinth::model::kQuirkEnvelopeDecibel) != 0;
    filter_prepare(song.filter_units_per_octave);
    // Линии ревербератора - в буфере сценариев: от сборки движка до его
    // сноса туда никто не пишет (загрузка ждёт сноса, сэмплы .mid идут из
    // банка мимо него). Трекерным форматам 13.3 КБ SRAM не нужны вовсе.
    if (song.reverb_enabled) {
        // Не влезло - трек играет без ревербератора: тишина хуже, чем сухой
        // звук, а место кончилось не по его вине.
        uint8_t* buf = memory::scratch_take(mem.scratch, memory::Scratch::Play, memory::kPlayScratchBytes);
        if (buf != nullptr) {
            auto* ps = new (buf) PlayScratch{};
            reverb_ = &ps->reverb;
            reverb_bus_ = ps->bus;
        }
    }
    mixer_.ramp_samples = ramp_samples_;
    mixer_.reverb_bus = reverb_bus_;
    link_.init(&mixer_, &mem_.psram);
    arbiter_.init(channels_, &link_, channel_count_, wave_tail_, envelope_db_);
    iface_.self = this;
    iface_.render_add = &TrackerEngine::render_add;

    dispatch_ctx_.channels = channels_;
    dispatch_ctx_.song = &song_;
    dispatch_ctx_.ps = &ps_;
    dispatch_ctx_.nna_user = this;
    dispatch_ctx_.on_note_trigger_nna = &TrackerEngine::on_note_trigger_nna;

    for (uint32_t& m : filter_memo_) {
        m = kFilterMemoNone;
    }
    // Счётчики декодера глобальные, знаменатель сбрасывается здесь: без сброса
    // decode_per_1k_vs на втором треке - десятки миллионов.
    voice_reset_debug_counters();
    // Начальные панорама и громкость каналов (заголовок файла, разводка Paula) -
    // до sequencer_init: он отыгрывает строку 0.
    channels_init(song_, channels_);
    sequencer_init(song_, mem_.psram, ps_, &TrackerEngine::row_callback, this);

    // Сведение ModPlug Tracker (Song::mix_levels): поверх обычной цепочки -
    // ослабление по числу каналов и на три бита более глубокое итоговое
    // ослабление (4 бита против 1), вместе PreAmpTable[каналов/2] / 64, таблица
    // как в OpenMPT. Предусилитель плеера при умолчании (128) сокращается.
    if (song_.mix_levels == soundsinth::model::Song::MixLevels::Original) {
        static constexpr uint8_t kPreAmpTable[16] = {
            0x60, 0x60, 0x60, 0x70, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC,
        };
        uint8_t nchn = channel_count_ == 0 ? 1 : channel_count_;
        if (nchn > 31) nchn = 31; // 1..31, как в OpenMPT
        master_gain_q16_ = (64u << kQ16Bits) / kPreAmpTable[nchn / 2u];
    } else {
        master_gain_q16_ = kQ16One;
    }
    // Строку 0 уже прочитал sequencer_init, её тик 0 отыгрывает первый
    // advance_tick() (first_tick_pending_).
}

// Фоновые голоса NNA паттерном не адресуются: Vibrato/Tremolo/Tremor/Panbrello/
// Porta/Retrigger им не положены, только продолжение взведённой огибающей
// и затухания (advance_envelope_and_fadeout, общая с живыми каналами).
// Слот снимается, когда голос кончился (Voice::active == false -
// незацикленный сэмпл доигран) или огибающая и затухание его остановили
// (ChannelState::voice_active == false) - до переноса высоты, чтобы тот не
// трогал мёртвый слот.
SOUNDSINTH_ALWAYS_INLINE void TrackerEngine::advance_nna_slot_envelopes(uint32_t envelope_time_step_q8) {
    const bool amiga_pitch_now = song_.frequency_model == soundsinth::model::FrequencyModel::Amiga;
    for (uint8_t i = 0; i < kNnaPool; ++i) {
        if (!arbiter_.slot_active(i)) continue;
        const uint8_t bg = static_cast<uint8_t>(channel_count_ + i);
        ChannelState& cs = channels_[bg];
        advance_envelope_and_fadeout(cs, amiga_pitch_now, song_.quirks, envelope_time_step_q8);
        // Огибающая доиграла до нулевой последней точки: слот освобождается.
        if (cs.stop_voice_pending) {
            cs.stop_voice_pending = false;
            link_.stop(bg);
        }
        if (!cs.voice_active || !link_.playing(bg)) arbiter_.stop_slot(bg);
    }
}

// Retrigger и NoteCut взводятся каждый тик. Смещение 0: SampleOffset - только
// у ноты строки. До переноса высоты: voice_trigger ставит высоту ноты, перенос
// возвращает высоту канала. У MOD и XM период сохраняется, как у ProTracker и
// libxmp (OpenMPT ставит период ноты заново).
SOUNDSINTH_ALWAYS_INLINE void TrackerEngine::apply_retrigger_and_note_cut() {
    for (uint8_t ch = 0; ch < channel_count_; ++ch) {
        ChannelState& cs = channels_[ch];
        if (cs.retrig_pending) {
            cs.retrig_pending = false;
            trigger_voice(ch, /*start_offset=*/0, /*note=*/false);
        }
        if (cs.stop_voice_pending) { // NoteCut или конец огибающей и затухания на этом тике
            cs.stop_voice_pending = false;
            link_.stop(ch);
        }
    }
}

// Высоту из канала в голос переносит только этот шаг: диспетчер эффектов
// Voice не знает. При той же высоте voice_set_* выходят сразу. Слоты NNA - по
// тем же флагам, pitch_offset у них 0.
SOUNDSINTH_ALWAYS_INLINE void TrackerEngine::sync_voice_pitch() {
    const uint8_t pitch_sync_limit = static_cast<uint8_t>(channel_count_ + kNnaPool);
    const bool glissando_nearest = (song_.quirks & soundsinth::model::kQuirkGlissandoNearest) != 0;
    const bool amiga_limits = (song_.quirks & soundsinth::model::kQuirkAmigaLimits) != 0;
    if (song_.frequency_model == soundsinth::model::FrequencyModel::Amiga) {
        for (uint8_t ch = 0; ch < pitch_sync_limit; ++ch) {
            const ChannelState& cs = channels_[ch];
            if (cs.voice_active && link_.playing(ch)) {
                uint16_t period = cs.period;
                if (glissando_snaps(cs, song_.quirks, ps_.tick_in_row)) {
                    period = glissando_amiga_period(cs, song_.samples[cs.sample_index].finetune, glissando_nearest);
                }
                const uint16_t effective_period =
                    soundsinth::model::clamp_amiga_period(static_cast<int32_t>(period) + cs.pitch_offset, amiga_limits);
                link_.set_pitch_amiga(ch, effective_period, song_.samples[cs.sample_index].c5_speed);
            }
        }
    } else {
        for (uint8_t ch = 0; ch < pitch_sync_limit; ++ch) {
            const ChannelState& cs = channels_[ch];
            if (cs.voice_active && link_.playing(ch)) {
                // Смещения высоты складываются: эффект, огибающая IT, бенд .mid.
                int32_t pitch = cs.linear_pitch;
                if (glissando_snaps(cs, song_.quirks, ps_.tick_in_row)) {
                    pitch = glissando_linear_pitch(cs, song_.samples[cs.sample_index].finetune, glissando_nearest);
                }
                link_.set_pitch_linear(ch, pitch + cs.pitch_offset + cs.pitch_envelope_offset + cs.bend_offset,
                                       song_.samples[cs.sample_index].c5_speed);
            }
        }
    }
}

// Горячий путь каждого тика. _ATTR, а не SOUNDSINTH_HOT_PATH: метод класса.
void TrackerEngine::run_tick() {
    const uint32_t t0_us = platform::time_us();
    advance_tick();
    const uint32_t dt_us = platform::time_us() - t0_us;
    if (dt_us > max_tick_duration_us_) {
        max_tick_duration_us_ = dt_us;
        worst_tick_active_count_ = voice_count_; // после rebuild_active_indices() внутри advance_tick()
        worst_tick_triggers_ = voice_triggers_this_tick_;
        worst_tick_row_start_ = ps_.tick_in_row == 0;
    }
}

bool TrackerEngine::seek_to_frame(uint32_t target_frames) {
    if (live_row_ != nullptr || ps_.song_ended || target_frames <= frames_rendered_) return false;

    // Тот же порядок, что в render_add: тик на границе, дальше батч до конца
    // тика. Разница одна - батч не сводится.
    while (frames_rendered_ < target_frames && !ps_.song_ended) {
        if (samples_until_next_tick_ == 0) {
            run_tick();
            samples_until_next_tick_ = ps_.last_tick_samples > 0 ? ps_.last_tick_samples : 1;
        }
        uint32_t batch = target_frames - frames_rendered_;
        if (samples_until_next_tick_ < batch) batch = samples_until_next_tick_;
        samples_until_next_tick_ -= batch;
        frames_rendered_ += batch;
        // Команды тика - в микшер, как это делает рендер: без них состояние
        // голосов разъедется с тем, что насчитал арбитр.
        link_.flush();
    }

    for (uint16_t s = 0; s < SOUNDSINTH_MAX_SLOTS; ++s) {
        link_.stop(static_cast<uint8_t>(s));
    }
    link_.flush();
    return true;
}

SOUNDSINTH_HOT_PATH_ATTR("te_advance_tick")
void TrackerEngine::advance_tick() {
    if (ps_.song_ended) return;
    // Тень состояния голосов - из опубликованного звуковой частью: что случилось
    // само (конец сэмпла, доигранное гашение), тик узнаёт здесь.
    // Кадров с прошлого тика - столько микшер и рендерил: по ним модель
    // звучания двигает позиции голосов.
    link_.begin_tick(ps_.last_tick_samples);
    voice_triggers_this_tick_ = 0; // считает все voice_trigger() ниже, включая row_callback и NoteDelay
    if (live_row_ != nullptr) {
        // Живая песня: строку на этот тик даёт источник, порядка и паттернов нет.
        first_tick_pending_ = false;
        sequencer_live_tick(ps_);
        if (const soundsinth::model::PatternCell* cells = live_row_(live_row_user_)) {
            dispatch_row_effects(&dispatch_ctx_, cells, channel_count_);
        }
    } else if (first_tick_pending_) {
        // Тик 0 строки 0: строку уже применил sequencer_init, двигать секвенсор
        // нечем, но NoteDelay, непрерывные эффекты и огибающие этому тику положены.
        first_tick_pending_ = false;
    } else {
        sequencer_tick(song_, mem_.psram, ps_, &TrackerEngine::row_callback, this);
    }
    // NoteDelay: доигрывает ячейки, отложенные на этой строке до текущего
    // тика, - до apply_continuous_effects этого же тика, чтобы слайды и
    // вибрато доигранной ячейки применились сейчас, а не со следующего тика.
    dispatch_delayed_notes(&dispatch_ctx_, ps_.tick_in_row, channel_count_);
    // Запуск и остановка голосов по разобранной строке и по NoteDelay - один
    // проход за тик.
    sync_voices_after_dispatch();
    // .mid: огибающие и затухание переведены в тики по default_tempo - при
    // смене темпа они продвигаются на отношение темпов (Song::envelopes_in_real_time).
    const uint32_t envelope_time_step_q8 =
        song_.envelopes_in_real_time && ps_.tempo != 0
            ? (static_cast<uint32_t>(song_.default_tempo) * kQ8One + ps_.tempo / 2u) / ps_.tempo
            : kQ8One;
    // У живой песни строка равна тику: тика 1 не бывает, и слайды без этого
    // не шли бы вовсе - скольжение высоты оставляло бы ноту на высоте
    // предыдущей.
    apply_continuous_effects(ps_, channels_, channel_count_, song_.quirks, song_.frequency_model,
                             envelope_time_step_q8, live_row_ != nullptr || ps_.tick_in_row != 0);
    advance_nna_slot_envelopes(envelope_time_step_q8);
    apply_retrigger_and_note_cut();
    sync_voice_pitch();

    rebuild_active_indices(); // раз за тик, не за отсчёт
    link_.end_tick();
    if (voice_triggers_this_tick_ > max_voice_triggers_per_tick_) {
        max_voice_triggers_per_tick_ = voice_triggers_this_tick_;
    }

    // Наблюдатель тика - после rebuild_active_indices(): список активных
    // голосов уже соответствует тику.
    if (tick_observer_ != nullptr) {
        tick_observer_(tick_observer_user_, ps_.order_pos, sample_map_, sample_map_count_);
    }
}

SOUNDSINTH_ALWAYS_INLINE void TrackerEngine::update_voice_mix(uint8_t idx, uint32_t global_vol_q24) {
    int32_t gain_l = 0, gain_r = 0;
    compute_mix_gains(channels_[idx], global_vol_q24, song_.pan_law, envelope_db_, &gain_l, &gain_r);
    link_.set_gains_ramp(idx, gain_l, gain_r);

    // Коэффициенты фильтра - раз в тик, рядом с громкостями: входы (срез,
    // резонанс, модификатор огибающей) меняются не чаще, на отсчёт остаётся
    // арифметика. У голосов без фильтра active = false, и посэмпловый цикл для
    // них не запускается.
    const ChannelState& fcs = channels_[idx];
    uint32_t& memo = filter_memo_[idx];
    // IT: фильтр, у которого срез стал открытым (резонанса нет, срез с
    // огибающей до верха шкалы), снимается только на ноте; посреди ноты
    // он работает прежними коэффициентами с той же памятью - как у
    // OpenMPT (SetupChannelFilter не трогает коэффициенты, снимает фильтр
    // только triggerNote). У .mid (filter_sf2_response) снимается сразу.
    const bool keep_open = memo != kFilterMemoNoteTrigger && !song_.filter_sf2_response && link_.filter_on(idx);
    const uint32_t key = static_cast<uint32_t>(fcs.filter_cutoff) | (static_cast<uint32_t>(fcs.filter_resonance) << 8) |
                         (static_cast<uint32_t>(static_cast<uint16_t>(fcs.filter_env_modifier)) << 16);
    if (fcs.filter_cutoff >= 127 && fcs.filter_resonance == 0 && fcs.filter_envelope == nullptr) {
        if (!keep_open) link_.filter_off(idx); // самый частый случай - не считаем
        memo = kFilterMemoNone; // мимо filter_compute - запомненному верить нельзя
    } else if (memo != key) {
        const FilterCoeffs computed = filter_compute(fcs.filter_cutoff, fcs.filter_resonance, fcs.filter_env_modifier,
                                                     song_.filter_units_per_octave, song_.filter_sf2_response);
        if (computed.active || !keep_open) link_.set_filter(idx, computed);
        memo = key;
    } // иначе входы те же - коэффициенты уже посчитаны
}

SOUNDSINTH_HOT_PATH_ATTR("te_rebuild_active_indices")
void TrackerEngine::rebuild_active_indices() {
    // Снятые с прошлого тика голоса гаснут (антиклик на снятии).
    link_.fade_stopped(wave_tail_);

    link_.list_clear();
    for (uint8_t ch = 0; ch < channel_count_; ++ch) {
        if (link_.playing(ch)) {
            push_active(ch);
        }
    }
    // Q0.16, оба поля 0..128. Считается раз за
    // тик вместе с усилениями голосов: global_volume и sample_preamp меняются
    // не чаще.
    //
    // Сдвиг на kMixFracBits переводит все усиления голоса разом в масштаб
    // шины Q24.8: SMLAWB (отсчёт * усиление >> 16) кладёт в накопитель 8 бит
    // дроби, и рампы, хвосты, отбор голосов по громкости берут усиления уже в
    // нём. Предел: global_volume 128, sample_preamp 255, master 1 - 130560,
    // после сдвига 33.4 млн; громкость сэмпла до 4 - усиление голоса до
    // 134 млн, в int32 входит.
    const uint32_t global_vol_q24 = q16_mul(q16_mul(static_cast<uint32_t>(ps_.global_volume) * kVol128ToQ16,
                                                    static_cast<uint32_t>(song_.sample_preamp) * kVol128ToQ16),
                                            master_gain_q16_)
                                    << mixbus::kMixFracBits;

    // Хвосты NNA - на остаток потолка SOUNDSINTH_MAX_VOICES, лишние гасятся.
    arbiter_.list_tails([&](uint8_t idx) {
        int32_t gain_l = 0, gain_r = 0;
        compute_mix_gains(channels_[idx], global_vol_q24, song_.pan_law, envelope_db_, &gain_l, &gain_r);
        link_.set_gains(idx, gain_l, gain_r);
    });

    for (uint8_t k = 0; k < link_.list_size(); ++k) {
        update_voice_mix(link_.list_at(k), global_vol_q24);
    }

    // Перегрузка: потолок полифонии и сброс лишних. Здесь: mixer_.gain_*_q24 уже
    // посчитаны, "самый тихий" определяется точно, а цикл ниже подхватит
    // погашенные голоса как гаснущие.
    arbiter_.update_budget(load_ewma_q8_);
    arbiter_.cull_over_budget();

    // Волновое гашение .mid читает сэмпл, как живой голос, хоть в списке тика
    // его и нет. Замороженные хвосты не в счёт: они стоят умножения.
    voice_count_ = static_cast<uint32_t>(link_.list_size()) + link_.fading_count();

    if (voice_count_ > peak_active_count_) peak_active_count_ = voice_count_;


    // Карта сэмплов для вытеснения - по всем слотам, которые читают цепочку
    // PSRAM, а не по списку: не влезший в список голос жив и вернётся в
    // него, освободить его сэмпл - доиграть чужие страницы. Строится после
    // сброса: погашенный голос цепочку уже не читает, как и гаснущий по
    // tail_remaining.
    uint32_t listed[(SOUNDSINTH_MAX_SLOTS + 31u) / 32u] = {};
    for (uint8_t k = 0; k < link_.list_size(); ++k) {
        listed[link_.list_at(k) / 32u] |= 1u << (link_.list_at(k) % 32u);
    }
    sample_map_count_ = 0;
    uint32_t unlisted = 0;
    for (uint8_t idx = 0; idx < SOUNDSINTH_MAX_SLOTS; ++idx) {
        // Гаснущий слот .mid продолжает читать сэмпл: его страницы вытеснять
        // нельзя, пока хвост не доиграл. У трекеров хвост - замороженный
        // отсчёт, данных ему не нужно.
        if (!link_.playing(idx) && !(wave_tail_ && link_.fading(idx))) continue;
        sample_map_[sample_map_count_++] = channels_[idx].sample_index;
        if ((listed[idx / 32u] & (1u << (idx % 32u))) == 0) ++unlisted;
    }
    voices_unlisted_ += unlisted;
    if (sample_map_count_ > voice_demand_peak_) voice_demand_peak_ = sample_map_count_;
    update_voice_routes();
}

// Solo: голос чужого канала считается как обычно, но сводится в discard -
// иначе он не двигался бы, и менялись бы список голосов, пул NNA и кража.
// Живой канал сравнивается по номеру, фоновый NNA - по origin_channel
// (канал до увода в фон). Посыл в ревербератор - от живого канала: фоновый
// NNA звучит посылом канала, с которого сорвался. Входы меняются только
// внутри тика.
void TrackerEngine::update_voice_routes() {
    for (uint8_t k = 0; k < link_.list_size(); ++k) {
        const uint8_t idx = link_.list_at(k);
        const bool is_live_channel = idx < channel_count_;
        const uint8_t origin = is_live_channel ? idx : arbiter_.origin_channel(static_cast<uint8_t>(idx - channel_count_));
        const bool to_discard = solo_channel_ >= 0 && origin != static_cast<uint8_t>(solo_channel_);
        // Посыл только при заведённых линиях: без них шины нет, и голосу некуда писать.
        link_.set_route(idx, to_discard, (reverb_bus_ != nullptr && !to_discard) ? channels_[origin].reverb_send : 0);
    }
}

void TrackerEngine::row_callback(void* user, const soundsinth::model::PatternCell* cells, uint8_t channel_count) {
    auto* self = static_cast<TrackerEngine*>(user);
    dispatch_row_effects(&self->dispatch_ctx_, cells, channel_count);
}

SOUNDSINTH_HOT_PATH_ATTR("te_trigger_voice")
void TrackerEngine::trigger_voice(uint8_t ch, uint32_t start_offset, bool note) {
    // Выключенный канал (Song::channel_muted): нота разобрана, состояние
    // канала живёт, но голос не запускается.
    if (soundsinth::model::channel_is_muted(song_, ch)) return;
    const ChannelState& cs = channels_[ch];
    memory::SampleCacheEntry* entry = memory::sample_cache_find(mem_.sample_cache, cs.sample_index);
    if (entry == nullptr && song_.samples[cs.sample_index].length_samples != 0) {
        missing_ring_[triggers_without_sample_ % kMissingRing] = cs.sample_index;
        ++triggers_without_sample_; // счётчик после записи: читатель берёт по нему
    }
    if (wave_tail_ && (entry == nullptr || song_.samples[cs.sample_index].length_samples == 0)) {
        link_.fade_before_missing(ch);
    }
    const uint16_t first_page = entry ? entry->first_page : memory::kPageChainEnd;
    const uint16_t checkpoint_first_page = entry ? entry->checkpoint_first_page : memory::kPageChainEnd;
    // Шаг здесь не считается: высоту голоса выставит sync_voice_pitch этого же
    // тика, до рендера, и с учётом смещений (эффект, огибающая, бенд).
    link_.trigger(ch, song_.samples[cs.sample_index], first_page, cs.last_note, song_.frequency_model,
                   start_offset, song_.quirks, checkpoint_first_page);
    ++voice_triggers_this_tick_;
    if (note) {
        // Коэффициенты фильтра считаются заново, даже если входы те же.
        filter_memo_[ch] = kFilterMemoNoteTrigger;
    }
}

void TrackerEngine::sync_voices_after_dispatch() {
    const uint8_t limit = channel_count_ < SOUNDSINTH_MAX_VOICES ? channel_count_ : SOUNDSINTH_MAX_VOICES;
    for (uint8_t ch = 0; ch < limit; ++ch) {
        ChannelState& cs = channels_[ch];
        if (cs.triggered_this_row) {
            // Потребляется сразу: из-за NoteDelay метод зовётся каждый тик, и иначе
            // голос ретриггерился бы на каждом тике строки. SampleOffset - только на
            // Note-Trigger этой строки.
            cs.triggered_this_row = false;
            trigger_voice(ch, cs.trigger_sample_offset, /*note=*/true);
        } else if (cs.stop_voice_pending) {
            // KeyOff без огибающей - только если строка не ретриггернула голос той же
            // строкой (KeyOff и нота на одной строке: новый триггер важнее).
            link_.stop(ch);
        }
        cs.stop_voice_pending = false;
    }
}

void TrackerEngine::on_note_trigger_nna(void* user, uint8_t channel, uint16_t new_instrument_1based,
                                        uint16_t new_sample_index, uint8_t new_resolved_note) {
    static_cast<TrackerEngine*>(user)->handle_note_trigger_nna(channel, new_instrument_1based, new_sample_index,
                                                               new_resolved_note);
}

void TrackerEngine::handle_note_trigger_nna(uint8_t channel, uint16_t new_instrument_1based, uint16_t new_sample_index,
                                            uint8_t new_resolved_note) {
    using soundsinth::model::DuplicateCheckAction;
    using soundsinth::model::DuplicateCheckType;
    using soundsinth::model::NewNoteAction;

    const soundsinth::model::Instrument& new_ins = song_.instruments[new_instrument_1based - 1];
    const bool it_rules = (song_.quirks & soundsinth::model::kQuirkItEnvelopeSustainLoop) != 0;

    // DCT/DCA обрывает или освобождает голоса этого канала (живой и его хвосты
    // NNA), у которых нота, сэмпл или инструмент совпадает с новым по правилам
    // DuplicateCheckType нового инструмента. Область - голоса этого канала, не
    // весь пул.
    // Упрощение: DCT=Note сравнивает ноту после keymap (last_note хранит её);
    // сырая нота паттерна до keymap (kQuirkItDctComparesPatternNote) отдельно
    // не хранится. На обычных файлах не сказывается.
    if (new_ins.dct != DuplicateCheckType::Off) {
        auto matches = [&](const ChannelState& cand) {
            switch (new_ins.dct) {
                case DuplicateCheckType::Note:
                    return cand.last_note == new_resolved_note;
                case DuplicateCheckType::Sample:
                    if ((song_.quirks & soundsinth::model::kQuirkItDctRequiresInstrumentMatch) &&
                        cand.last_instrument != new_instrument_1based) {
                        return false;
                    }
                    return cand.sample_index == new_sample_index;
                case DuplicateCheckType::Instrument:
                    return cand.last_instrument == new_instrument_1based;
                default:
                    return false;
            }
        };
        auto apply_dca = [&](uint8_t idx) {
            switch (new_ins.dca) {
                case DuplicateCheckAction::Cut:
                    arbiter_.stop_slot(idx);
                    break;
                case DuplicateCheckAction::Off:
                    // Как KeyOff у IT: огибающие в релиз, затухание - release_note (у
                    // инструмента без огибающей громкости при нулевом fadeout нота
                    // звучит дальше).
                    release_note(channels_[idx], it_rules);
                    break;
                case DuplicateCheckAction::Fade:
                    fade_note(channels_[idx], it_rules);
                    break;
            }
        };

        if (channels_[channel].voice_active && matches(channels_[channel])) apply_dca(channel);
        for (uint8_t i = 0; i < kNnaPool; ++i) {
            if (!arbiter_.is_tail_of(i, channel)) continue;
            const uint8_t idx = static_cast<uint8_t>(channel_count_ + i);
            if (channels_[idx].voice_active && matches(channels_[idx])) apply_dca(idx);
        }
    }

    // Увод в фон: живой голос канала, переживший DCT/DCA, уходит в фон, если у
    // его старого инструмента nna != Cut. channels_[channel] и mixer_.voices[channel]
    // сейчас перезапишет новая нота, поэтому фон получает свою копию здесь.
    ChannelState& live = channels_[channel];
    if (!live.voice_active) return; // голоса не было, либо DCT/DCA его только что оборвал
    // Незацикленный сэмпл доиграл: Voice::active снят, а ChannelState::
    // voice_active у живого канала с концом сэмпла не гаснет. Уводить нечего,
    // а увод погасил бы звучащий хвост этого канала или украл слот пула. У
    // OpenMPT то же: канал без играющего сэмпла в фон не уходит.
    if (!link_.playing(channel)) return;
    // Выключенный канал голоса не запускает: уводить нечего, а пустой хвост
    // занял бы слот пула, нужный слышимым каналам.
    if (soundsinth::model::channel_is_muted(song_, channel)) return;
    if (live.last_instrument == 0 || live.last_instrument > song_.instrument_count) {
        return; // защитно: при voice_active == true не бывает
    }
    const NewNoteAction nna = song_.instruments[live.last_instrument - 1].nna;
    if (nna == NewNoteAction::Cut) return; // Note-Trigger ниже просто перезапишет канал

    const uint8_t bg = arbiter_.to_background(channel);
    filter_memo_[bg] = filter_memo_[channel];
    // Временные модуляторы - в нейтраль: фоновые слоты Vibrato/Tremolo/Tremor/
    // Panbrello больше не получают, и активный в момент увода tremor_muted
    // заглушил бы голос навсегда.
    channels_[bg].tremor_muted = false;
    channels_[bg].glissando_porta = false; // фоновый голос звучит без ступеней glissando, как в OpenMPT
    channels_[bg].volume_offset = 0;
    channels_[bg].pan_offset = 0;
    channels_[bg].pitch_offset = 0;

    // Off - как KeyOff у IT (release_note), Fade - затухание (fade_note).
    // Continue - голос продолжает как играл.
    if (nna == NewNoteAction::Off) release_note(channels_[bg], it_rules);
    if (nna == NewNoteAction::Fade) fade_note(channels_[bg], it_rules);
}

SOUNDSINTH_HOT_PATH_ATTR("te_render_add")
void TrackerEngine::render_add(void* self_ptr, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames) {
    auto* self = static_cast<TrackerEngine*>(self_ptr);
    // Solo: голоса чужих каналов сводятся в discard, а он не длиннее
    // solo_discard_frames_.
    if (self->solo_channel_ >= 0 && n_frames > self->solo_discard_frames_) {
        for (uint32_t off = 0; off < n_frames;) {
            const uint32_t left = n_frames - off;
            const uint32_t n = left < self->solo_discard_frames_ ? left : self->solo_discard_frames_;
            render_add(self_ptr, mix_l + off, mix_r + off, n);
            off += n;
        }
        return;
    }
    self->frames_rendered_ += n_frames;

    // Шина реверберации обнуляется на блок. Только когда она вообще
    // нужна: у трекерных форматов ни memset, ни обработки не будет.
    // Признак - заведённые линии, а не запрос формата: места под них могло
    // не хватить, и трек тогда играет сухим.
    const bool want_reverb = self->reverb_bus_ != nullptr;
    if (want_reverb) {
        const uint32_t clear = n_frames < SOUNDSINTH_AUDIO_BUFFER_FRAMES ? n_frames : SOUNDSINTH_AUDIO_BUFFER_FRAMES;
        std::memset(self->reverb_bus_, 0, clear * sizeof(int32_t));
    }
    const uint32_t render_t0_us = platform::time_us();
    // Шина рассчитана на один блок; тесты зовут render_add напрямую с
    // произвольным n_frames, выходить за буфер нельзя.
    const uint32_t reverb_frames =
        n_frames < SOUNDSINTH_AUDIO_BUFFER_FRAMES ? n_frames : SOUNDSINTH_AUDIO_BUFFER_FRAMES;
    // Неполная группа прореживания ревербератора бывает только в последнем
    // блоке: иначе выход зависит от разбиения на блоки.
    static_assert(SOUNDSINTH_AUDIO_BUFFER_FRAMES % kReverbRateDiv == 0, "блок кратен kReverbRateDiv");
    uint32_t i = 0;
    while (i < n_frames) {
        if (self->samples_until_next_tick_ == 0) {
            // Тик делает либо сам рендер, либо отдельная задача - строго по
            // очереди, ожидая её здесь же. Забегать вперёд ей нельзя: арбитр
            // судит о голосах по тени, а её публикует микшер раз в батч.
            if (self->tick_runner_ != nullptr) {
                self->tick_runner_(self->tick_runner_user_);
            } else {
                self->run_tick();
            }
            // last_tick_samples == 0 - вырожденный темп или пустая песня; продвинуться
            // надо хотя бы на отсчёт, иначе следующий кадр снова упрётся в
            // advance_tick().
            self->samples_until_next_tick_ = self->ps_.last_tick_samples > 0 ? self->ps_.last_tick_samples : 1;
        }
        // Батч - до конца тика или буфера: внутри батча граница тика не проверяется.
        uint32_t batch = n_frames - i;
        if (self->samples_until_next_tick_ < batch) batch = self->samples_until_next_tick_;
        self->samples_until_next_tick_ -= batch;

        // Команды тика - в голоса: звуковая сторона разбирает кольцо перед
        // сведением батча.
        self->link_.flush();

        const uint32_t voice_loop_t0_us = platform::time_us();
        self->mixer_.mix(self->mem_.psram, mix_l, mix_r, i, batch, reverb_frames);
        // Раз на батч, а не на голос-отсчёт: иначе таймер искажал бы то, что
        // измеряет.
        self->voice_loop_total_us_ += platform::time_us() - voice_loop_t0_us;
        self->voice_sample_count_ += static_cast<uint64_t>(batch) * self->voice_count_;
        i += batch;
    }

    // Реверберация - после всех голосов, одним проходом на блок: ревербератор
    // один на движок.
    if (want_reverb) {
        reverb_process(*self->reverb_, self->reverb_bus_, mix_l, mix_r, reverb_frames);
    }

    // Без часов (ПК) загрузку задаёт тест через set_overload_hint_pct, пересчёт
    // её бы затёр.
    if constexpr (platform::kHasClock) {
        if (n_frames > 0) update_render_load(self->load_ewma_q8_, render_t0_us, n_frames);
    } else if (self->model_load_enabled_) {
        // Цена звучащих голосов на отсчёт против длительности отсчёта (1e9/44100 нс).
        // По самим голосам, а не по тени тика: считается после сведения батча,
        // а звуковая часть за батч успела и голоса кончить.
        const uint64_t cost_ns = self->mixer_.list_cost_ns();
        smooth_load(self->load_ewma_q8_, static_cast<uint32_t>((cost_ns * kQ8One * kSampleRateHz) / 1000000000u));
    }
}

} // namespace soundsinth::engine
