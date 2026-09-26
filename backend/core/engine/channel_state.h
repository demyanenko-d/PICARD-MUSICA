#pragma once

// Состояние канала диспетчера эффектов: пишут channels_init,
// dispatch_row_effects и apply_continuous_effects, читает TrackerEngine при
// рендере. Секвенсор его не видит.

#include <cstdint>

#include "core/engine/engine_defs.h"
#include "core/model/song.h"

namespace soundsinth::engine {

struct ChannelState {
    uint8_t last_note = soundsinth::model::kNoteNone;
    // Нота с уже применённым SampleDescriptor::relative_note, та же, из которой
    // на Note-Trigger посчитан period. Нужна Amiga-арпеджио: apply_continuous_effects
    // каждый тик пересчитывает период для note+смещение через amiga_note_to_period,
    // а Song не видит.
    uint8_t effective_note = 0;
    uint16_t last_instrument = 0; // 1-based индекс в Song::instruments, 0 = нет

    uint8_t volume = 0; // текущая громкость канала 0..64, значима только пока voice_active
    uint8_t volume_slide_memory = 0; // память VolumeSlide и комбинаций: 00 повторяет последний ненулевой параметр
    bool volume_slide_active = false; // слайд активен на этой строке, сбрасывается на каждой новой строке

    // SetChannelVolume/ChannelVolumeSlide (IT Mxx/Nxy): множитель 0..64
    // громкости канала поверх cs.volume, применяется в render_add. Начальное
    // значение ставит channels_init из Song::channel_volume. Note-Trigger его
    // не трогает, держится до явной установки.
    uint8_t channel_volume = kVolumeMax;
    uint8_t channel_volume_slide_memory = 0;
    bool channel_volume_slide_active = false;

    // Instrument::global_volume (IT, 0..128): статический множитель
    // инструмента поверх cs.volume и огибающей, кэш на Note-Trigger. 128
    // (нейтраль) у MOD/S3M/XM и у IT-инструментов без значения.
    uint8_t instrument_global_volume = kGlobalVolumeMax;

    // SampleDescriptor::global_volume (IT GvL, 0..64) взятого сэмпла, второй
    // статический множитель рядом с инструментальным. 64 (нейтраль) у форматов
    // без такого поля.
    uint8_t sample_global_volume = kVolumeMax;
    // Звучит ли голос на канале. Ссылка на пустой сэмпл (length_samples == 0)
    // молчит, и SetVolume/VolumeSlide на канале без голоса не действуют.
    bool voice_active = false;
    // true только на строке, где Note-Trigger действительно (пере)запустил
    // голос; сбрасывается в начале каждой строки. По нему TrackerEngine после
    // dispatch_row_effects решает, звать ли voice_trigger.
    bool triggered_this_row = false;
    // Индекс в Song::samples, значим только пока voice_active. Пишет
    // dispatch_row_effects на Note-Trigger, читает TrackerEngine для
    // voice_trigger.
    uint16_t sample_index = 0;

    // Питч. Модель Amiga: period и tone_porta_target, модель Linear:
    // linear_pitch и linear_tone_porta_target. porta_memory, porta_active,
    // porta_delta и tone_porta_active общие: porta_delta - период (Amiga) или
    // 1/64 полутона (Linear) по Song::frequency_model, модель одна на песню.
    uint16_t period = 0; // текущий Amiga-период, двигается портаменто тик за тиком
    // Память скорости PortaUp/PortaDown/TonePorta, 00 повторяет последний
    // ненулевой; uint16_t, потому что Linear хранит param*4. Раздельная память
    // FT2 (kQuirkXmFt2SeparatePortaMemory) не реализована.
    uint16_t porta_memory = 0;
    int32_t linear_pitch = 0; // аналог period в модели Linear: 1/64 полутона от опорной ноты
    // S3M/IT: сырой байт последнего PortaUp/PortaDown. Тонкий (xF?) и
    // сверхтонкий (xE?) вариант спрятан в параметре, и решается это уже после
    // памяти: E00 после EF3 - снова тонкий слайд.
    uint8_t porta_raw_memory = 0;
    bool porta_active = false; // портаменто активно на этой строке, сбрасывается на каждой новой строке
    int16_t porta_delta =
        0; // дельта period/linear_pitch за тик. Amiga: минус - PortaUp, плюс - PortaDown; Linear - наоборот
    // Цель TonePorta (Amiga); построчно не сбрасывается, держится до новой ноты
    // на строке с TonePorta.
    uint16_t tone_porta_target = 0;
    bool tone_porta_active = false; // TonePorta активно на этой строке, сбрасывается на каждой новой строке
    // GlissandoControl (MOD E3x, S3M/IT S1x): режим, построчно не
    // сбрасывается. С ним TonePorta звучит ступенями по полутонам.
    bool glissando_enabled = false;
    int32_t linear_tone_porta_target = 0; // цель TonePorta (Linear), держится так же
    // На этой ноте было тон-портаменто: с ним и glissando_enabled движок
    // прижимает звучащую высоту к полутонам,
    // period/linear_pitch при этом едут плавно. Сбрасывается новой нотой.
    bool glissando_porta = false;

    // SetFinetune (MOD E5x, S3M/IT S2x): finetune канала вместо finetune
    // сэмпла (128 единиц на полутон). Сбрасывается на каждом реальном
    // Note-Trigger, S2x той же строки действует после сброса. На Note-Trigger
    // - непрерывная поправка к period/linear_pitch, не таблица Paula.
    int8_t finetune_override = 0;
    bool finetune_override_active = false;

    // Vibrato и Arpeggio: временное отклонение pitch_offset, period и
    // linear_pitch не меняются. Поле общее: у ячейки один эффект. speed и
    // depth - раздельная память по нибблам. phase 0..63, сбрасывается на
    // реальном Note-Trigger (не подавленном TonePorta) и дальше бежит, даже
    // если Vibrato на следующей строке не указан.
    uint8_t vibrato_speed = 0;
    uint8_t vibrato_depth = 0; // у Vibrato уже умножена на 4 при декоде, у FineVibrato без множителя
    uint8_t vibrato_phase = 0;
    // Форма волны (SetVibratoWaveform: S3M/IT S3x, MOD E4x): 0 синус, 1 пила
    // вниз, 2 прямоугольник, 3 случайная (lfo_waveform_value); бит 0x04 -
    // фаза на новой ноте не сбрасывается. Не сбрасывается ни построчно, ни на
    // Note-Trigger.
    uint8_t vibrato_waveform = 0;
    bool vibrato_active = false; // Vibrato/FineVibrato активно на этой строке, сбрасывается на каждой новой строке
    int32_t pitch_offset =
        0; // смещение period (Amiga) или linear_pitch (Linear) от Vibrato/Arpeggio; TrackerEngine прибавляет при расчёте Voice::step

    // Питч-бенд MIDI (SetPitchOffset): смещение linear_pitch, отдельное от
    // pitch_offset. pitch_offset обнуляется на строке без вибрато и арпеджио,
    // а бенд держится до следующего события. У трекерных форматов ноль.
    //
    // Бенд доезжает до цели за строку, а не прыгает: события бенда бывают
    // чаще строк, и ступенька слышна на выдержанной ноте. Шаг считается при
    // получении команды, применяется каждый тик. Цель - (param - 128) << 0..6,
    // то есть -8192..8128; шаг - разность двух таких, с перелётом за цель до
    // тика прижима сумма в int16 влезает.
    int16_t bend_offset = 0;
    int16_t bend_target = 0;
    int16_t bend_step = 0;
    // XM: память E1x/E2x и X1x/X2x, своя у каждой пары, старший нибл - вверх,
    // младший - вниз; с 1xx/2xx/3xx не общая. Здесь - на месте, освобождённом
    // bend_step.
    uint8_t xm_fine_porta_memory = 0;
    uint8_t xm_extra_fine_porta_memory = 0;

    // Arpeggio: нота по кругу из трёх тиков: база, +arpeggio_x, +arpeggio_y
    // полутонов. Пишет тот же pitch_offset, что Vibrato.
    uint8_t arpeggio_x = 0;
    uint8_t arpeggio_y = 0;
    // Amiga: периоды нот +x и +y с finetune канала, считаются на строке.
    uint16_t arpeggio_period_x = 0;
    uint16_t arpeggio_period_y = 0;
    bool arpeggio_active = false; // сбрасывается на каждой новой строке

    // Tremolo и Tremor: временное отклонение громкости, как Vibrato у питча.
    // От frequency_model не зависят.
    uint8_t tremolo_speed = 0;
    uint8_t tremolo_depth = 0; // без множителя 4, в отличие от vibrato_depth
    uint8_t tremolo_phase = 0;
    uint8_t tremolo_waveform = 0; // значения как у vibrato_waveform, память отдельная
    bool tremolo_active = false; // сбрасывается на каждой новой строке
    // Tremor: память - целый байт (не по нибблам, в отличие от Vibrato и
    // Tremolo); чередование звучит/молчит по счётчику тиков.
    uint8_t tremor_memory = 0;
    uint8_t tremor_on_ticks = 1;
    uint8_t tremor_off_ticks = 1;
    uint8_t tremor_counter = 0; // 0 в начале: первый тик переключает в фазу "звучит"
    bool tremor_on_phase = false;
    bool tremor_active = false; // сбрасывается на каждой новой строке; счётчик и фаза текут дальше
    int16_t volume_offset = 0; // отклонение громкости от Tremolo; TrackerEngine прибавляет к volume при рендере
    bool tremor_muted = false; // Tremor глушит канал на этом тике

    // Панорама 0..64, 32 - центр (шкала SampleDescriptor::default_panning).
    // Стартовое значение ставит channels_init: Song::channel_pan или
    // LRRL-разводка MOD (kQuirkModHardwarePanning).
    uint8_t pan = kPanCenter;
    // S91 (Set Surround): правый канал с обратным знаком, в моно канал гасит
    // сам себя. Снимается любой установкой панорамы (S90, S8x, Xxx, панорама
    // сэмпла или инструмента).
    bool surround = false;
    uint8_t pan_slide_memory = 0; // память PanningSlide, 00 повторяет последний ненулевой
    bool pan_slide_active = false; // слайд активен на этой строке, сбрасывается на каждой новой строке
    uint8_t pan_slide_step = 0;    // слайд строки: старший нибл - вправо, младший - влево

    // Panbrello (S3M/IT Yxy): depth = нибл << 4 (у Vibrato << 2), делитель
    // 2048 - под шкалу панорамы 0..64.
    uint8_t panbrello_speed = 0;
    uint8_t panbrello_depth = 0;
    uint8_t panbrello_phase = 0;
    uint8_t panbrello_waveform = 0; // значения как у vibrato_waveform, память отдельная
    bool panbrello_active = false; // сбрасывается на каждой новой строке
    uint8_t retrig_memory = 0; // IT/S3M: последний ненулевой Qxy, его повторяет Q00; здесь - в дырке выравнивания
    int16_t pan_offset = 0; // смещение панорамы от Panbrello; TrackerEngine прибавляет к pan при рендере

    // Посыл в ревербератор 0..127 (SetReverbSend). У трекерных форматов ноль,
    // шина тогда не накапливается.
    uint8_t reverb_send = 0;

    // SampleOffset действует только вместе с реальным Note-Trigger на этой
    // строке. sample_offset_memory - память (00 повторяет последний ненулевой)
    // в единицах параметра, по 256 отсчётов. trigger_sample_offset - уже в
    // отсчётах, сбрасывается каждую строку, читается TrackerEngine при
    // triggered_this_row.
    uint8_t sample_offset_memory = 0;
    uint32_t trigger_sample_offset = 0;
    // HighOffset (S3M/IT SAx): старший нибл смещения,
    // trigger_sample_offset = (high << 16) | (param << 8). Построчно не
    // сбрасывается, держится до следующего SAx.
    uint8_t sample_offset_high = 0;

    // Retrigger: перезапуск каждые retrig_interval тиков (тик 0 строки не
    // считается), громкость меняется по retrig_type (0..15, общая для всех
    // форматов таблица kRetrigVolumeTable). retrig_pending TrackerEngine
    // проверяет и гасит каждый тик; true - заново вызвать voice_trigger
    // (offset 0, та же нота).
    // retrig_type == kNoteCutRetrigType - NoteCut (ECx/SCx) на том же счётчике.
    uint8_t retrig_type = 0;
    uint8_t retrig_interval = 1;
    uint8_t retrig_counter = 0;
    bool retrig_active = false; // сбрасывается на каждой новой строке
    bool retrig_pending = false; // взводит apply_continuous_effects, гасит TrackerEngine

    // Отпускание и затухание ноты (release_note, fade_note). key_released -
    // огибающие идут за точку удержания. note_fading - fadeout_level (Q16.16,
    // kQ16One = 1.0) убывает на instrument_fadeout_rate (кэш
    // Instrument::fadeout_rate) за тик, на нуле взводится stop_voice_pending.
    // stop_voice_pending TrackerEngine гасит голос сразу после
    // dispatch_row_effects/apply_continuous_effects.
    bool stop_voice_pending = false;
    bool key_released = false;
    uint32_t fadeout_level = kQ16One;
    uint32_t instrument_fadeout_rate = 0;

    // Огибающая громкости: указатель из Song::instruments, ставится на
    // Note-Trigger (apply_continuous_effects Song не видит). envelope_tick -
    // позиция в тиках от начала ноты, растёт каждый тик, включая тик 0. На
    // sustain-точке держится, пока нота не отпущена. envelope_volume 0..64 -
    // множитель (не слагаемое, как volume_offset), TrackerEngine умножает на
    // него cs.volume.
    const soundsinth::model::Envelope* volume_envelope = nullptr;
    uint16_t envelope_tick = 0;
    uint8_t envelope_volume = kEnvelopeNeutral;
    // Дробная часть тика огибающих (Song::envelopes_in_real_time), Q0.8: сколько
    // тиков огибающих набежало сверх целых. Байт ложится в дырку выравнивания
    // перед указателем - ChannelState не растёт.
    uint8_t envelope_time_frac = 0;

    // Огибающая панорамы: та же механика, своя позиция. pan_envelope_value -
    // сырое значение точки (0..64, 32 - нейтраль); TrackerEngine применяет его
    // в render_add как смещение cs.pan с глубиной, убывающей к краям панорамы.
    const soundsinth::model::Envelope* panning_envelope = nullptr;
    uint16_t pan_envelope_tick = 0;
    uint8_t pan_envelope_value = kEnvelopeCenter;
    bool note_fading = false; // затухание включено (release_note, fade_note); здесь - в дырке выравнивания

    // Огибающая питча (только IT). Хранение 0..64, 32 - нейтраль, смещение
    // linear_pitch = clamp((raw - 32) * 8, -255, 255) * 4, до +-16 полутонов.
    // Только модель Linear: в Amiga IT тоже гнёт питч линейно, это не сделано.
    const soundsinth::model::Envelope* pitch_envelope = nullptr;
    uint16_t pitch_envelope_tick = 0;
    int16_t pitch_envelope_offset = 0;

    // --- Резонансный фильтр (только IT) ---
    //
    // filter_envelope - та же ячейка данных файла, что и pitch_envelope, но с
    // битом "это фильтр": заполнено может быть только одно из двух.
    //
    // filter_cutoff/filter_resonance - текущие значения канала (0..127), а не
    // поля инструмента: на триггере берутся из инструмента (или 127/0, если
    // тот их не задаёт; у .mid - с поправкой на velocity и высоту ноты),
    // эффекты их не меняют: Zxx и MIDI-макросы не исполняются. 127 + 0 -
    // фильтра нет, коэффициенты не считаются.
    //
    // filter_env_modifier - вклад огибающей, -256..+256, +256 - нейтраль. Из
    // сырого 0..64 получается как value*8-256, поэтому
    // "нет огибающей" и "огибающая на максимуме" дают одно и то же - срез
    // канала без изменения.
    const soundsinth::model::Envelope* filter_envelope = nullptr;
    uint16_t filter_envelope_tick = 0;
    uint8_t filter_cutoff = kFilterCutoffOpen;
    uint8_t filter_resonance = 0;
    int16_t filter_env_modifier = kFilterEnvNeutral;

    // NoteDelay (MOD/XM EDx, S3M/IT SDx): вся ячейка (нота, инструмент,
    // громкость, эффект) откладывается до заданного тика строки, на тике 0
    // ячейка канала пропускается целиком. delayed_cell_tick == 0 - отложенной
    // ячейки нет: EDx с параметром 0 обрабатывается сразу.
    soundsinth::model::PatternCell delayed_cell;
    uint16_t delayed_cell_tick = 0;
};
// 96 слотов (SOUNDSINTH_MAX_SLOTS) в SRAM: рост ловит компилятор, а не
// замер. Поля внутри групп стоят так, чтобы дырок выравнивания было меньше
// (сейчас 2 байта). Только при 4-байтовом указателе - на ПК x64 размер свой.
static_assert(sizeof(void*) != 4 || sizeof(ChannelState) == 156, "ChannelState вырос: 96 слотов в SRAM");

// Отпускание ноты: Note-Off, NNA Off, DCA Off. У IT (it_rules) снимает
// удержание огибающих, а затухание включает сразу только без огибающей
// громкости или при её петле - иначе его включит конец огибающей. У
// остальных форматов отпускание и затухание вместе.
inline void release_note(ChannelState& cs, bool it_rules) {
    cs.key_released = true;
    const soundsinth::model::Envelope* env = cs.volume_envelope;
    if (!it_rules || env == nullptr || !env->enabled || env->loop_enabled) cs.note_fading = true;
}

// Затухание без отпускания: IT ^^^, NNA Fade, DCA Fade. У остальных
// форматов - вместе с отпусканием.
inline void fade_note(ChannelState& cs, bool it_rules) {
    cs.note_fading = true;
    if (!it_rules) cs.key_released = true;
}

// Мгновенная остановка: Voice гасит TrackerEngine на этом тике
// (stop_voice_pending), cs.voice_active Voice не трогает.
inline void stop_voice(ChannelState& cs) {
    cs.voice_active = false;
    cs.stop_voice_pending = true;
}

// Note-Off вне правил IT: с огибающей громкости - отпускание и затухание,
// без неё - мгновенная остановка (у XM "Key Off == Note Cut").
inline void key_off(ChannelState& cs) {
    if (cs.volume_envelope != nullptr) {
        release_note(cs, false);
    } else {
        stop_voice(cs);
    }
}

} // namespace soundsinth::engine
