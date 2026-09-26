#pragma once

// Диспетчер эффектов: разбор строки и потиковые эффекты поверх секвенсора.
// Работает только с ChannelState и PlayState; Voice, PSRAM и TrackerEngine не
// видит.

#include <cstdint>

#include "core/engine/channel_state.h"
#include "core/engine/sequencer.h"
#include "core/model/quirks.h"
#include "core/model/song.h"

namespace soundsinth::engine {

// NNA (только IT): зовётся перед тем, как Note-Trigger перезапишет канал
// новой нотой, пока в нём старый голос.
using NnaTriggerCallback = void (*)(void* user, uint8_t channel, uint16_t new_instrument_1based,
                                    uint16_t new_sample_index, uint8_t new_resolved_note);

struct DispatchContext {
    ChannelState* channels = nullptr;
    // Песня. Note-Trigger читает из неё keymap инструмента
    // (resolve_sample_index); у сэмпла length_samples, default_volume,
    // global_volume, default_panning, relative_note, finetune; у инструмента
    // четыре огибающие, filter_cutoff и filter_resonance с битом 0x80,
    // velocity_to_cutoff, fadeout_rate, global_volume, instrument_panning,
    // pitch_pan_separation, pitch_pan_center; у песни quirks, frequency_model,
    // filter_follows_note, filter_units_per_octave. Поле, которое начинает
    // читать триггер, заполняет каждый источник Song.
    const soundsinth::model::Song* song = nullptr;
    // SetGlobalVolume/GlobalVolumeSlide и speed в SetPitchOffset. Не nullptr.
    PlayState* ps = nullptr;

    // NNA: nna_user - указатель вызывающего, не DispatchContext*. Оба поля
    // nullptr - NNA нет.
    void* nna_user = nullptr;
    NnaTriggerCallback on_note_trigger_nna = nullptr;
};

// Сброс каналов, панорама и громкость из заголовка - до sequencer_init: он
// разбирает строку 0, и её Xxx, S8x, Mxx и панорама сэмпла ложатся поверх
// заголовка (строка 0 с Mxx, Xxx или S8x - около 8% файлов IT).
void channels_init(const soundsinth::model::Song& song, ChannelState* channels);

// Разбор строки: у каждого канала - перехват NoteDelay, сброс флагов
// строки, ячейка (Note-Trigger: сэмпл по keymap, громкость сэмпла; затем
// команды строки). Тонкие слайды - разово здесь, на тике 0; обычные взводят
// *_active.
void dispatch_row_effects(DispatchContext* ctx, const soundsinth::model::PatternCell* cells, uint8_t channel_count);

// Вызывать каждый тик после dispatch_row_effects/sequencer_tick: для
// каналов, у которых ChannelState::delayed_cell_tick совпал с
// tick_in_row, доигрывает отложенную ячейку - сброс строки канала и ячейка,
// как у прохода строки.
void dispatch_delayed_notes(DispatchContext* ctx, uint16_t tick_in_row, uint8_t channel_count);

// Вызывать после каждого sequencer_tick()/sequencer_init(): применяет
// потиковые эффекты активных на строке каналов. Слайды и портаменто
// стартуют с тика 1, как в MOD/S3M/XM/IT; Vibrato, Tremolo, Tremor,
// Panbrello, Arpeggio, бенд и огибающие работают и на тике 0.
// frequency_model - шкала period/linear_pitch в этой песне: по ней ветвятся
// портаменто, вибрато, арпеджио и огибающая питча. ps не const:
// GlobalVolumeSlide двигает ps.global_volume здесь же.
// envelope_time_step_q8 - тиков огибающих на тик движка, Q8.8.
// tick_slides - идут ли слайды, которые у трекера начинаются с тика 1
// (скольжение высоты, слайды громкости и панорамы, счётчики Retrigger и
// NoteCut). У файла это tick_in_row != 0. У живого MIDI строка равна тику, и
// такой тик не наступает никогда - ему нужен свой ответ, а не выдуманный
// номер тика: по номеру сверяются отложенные ноты и фаза арпеджио.
void apply_continuous_effects(PlayState& ps, ChannelState* channels, uint8_t channel_count,
                               soundsinth::model::QuirkFlags quirks,
                               soundsinth::model::FrequencyModel frequency_model, uint32_t envelope_time_step_q8,
                               bool tick_slides);

// Огибающие громкости, панорамы, питча, фильтра и затухание одного канала;
// её же зовут для хвостов NNA. amiga_pitch: тик огибающей питча идёт,
// смещение не применяется. Из quirks - правила: kQuirkFadeoutExponential
// (.mid: затухание - множитель Q16 на тик, огибающая громкости отпущенной
// ноты стоит), kQuirkItEnvelopeSustainLoop (петля удержания IT, конец
// огибающей громкости включает затухание), kQuirkItSilentEnvelopeEndStops
// (голос снимается за нулевой последней точкой). envelope_time_step_q8:
// тиков огибающих на тик, Q8.8 (у .mid default_tempo / текущий темп).
void advance_envelope_and_fadeout(ChannelState& cs, bool amiga_pitch, soundsinth::model::QuirkFlags quirks,
                                  uint32_t envelope_time_step_q8);

// Звучащая высота при glissando (ChannelState::glissando_porta): period
// или linear_pitch канала, прижатые к ноте сетки полутонов. Сетка сдвинута
// на finetune ноты - тот же, что на Note-Trigger: подмена S2x/E5x или
// sample_finetune. nearest == false - первая нота не ниже звучащей высоты,
// true - ближайшая (XM). Во флеше, не в
// SRAM: горячий цикл TrackerEngine зовёт их только на каналах с glissando
// (около 0.2% файлов архива).
uint16_t glissando_amiga_period(const ChannelState& cs, int8_t sample_finetune, bool nearest);
int32_t glissando_linear_pitch(const ChannelState& cs, int8_t sample_finetune, bool nearest);

} // namespace soundsinth::engine
