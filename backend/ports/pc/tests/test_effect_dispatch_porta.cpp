// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cstdio>
#include <initializer_list>
#include <vector>

#include "core/engine/effect_dispatch.h"
#include "core/engine/voice.h"
#include "core/model/amiga_period.h"

// Синтетика для Effect::PortaUp/PortaDown/TonePorta/Vibrato/Tremolo/
// Tremor/SetPanning/PanningSlide/SampleOffset/HighOffset/Retrigger/
// NoteCut/KeyOff напрямую через dispatch_row_effects и
// apply_continuous_effects, без реального Song и секвенсора (облегчённый
// подход, который PlayState/ChannelState допускают; секвенсор целиком -
// в test_sequencer.cpp). С libxmp не сверено: при попытке сверки
// абсолютные периоды libxmp разошлись с нашими уже на тике 0, до первой
// команды порто. libxmp применяет MOD-таблицу finetune по сэмплу
// (16 тюнингов), а у нас тогда был только тюнинг 0, см.
// test_sequencer_libxmp.cpp. Абсолютная сверка периодов без своей
// реализации finetune смысла не имела, поэтому эта синтетика проверяет
// механику (знак, шаг, память, клэмп, гейт по частотной модели), а не
// конкретные значения периода в конкретном файле.

namespace {

using namespace soundsinth;
using soundsinth::model::Effect;
using soundsinth::model::FrequencyModel;
using soundsinth::model::PatternCell;
using soundsinth::model::SlideRate;
using soundsinth::model::VolumeColumnType;

// Минимальный Song на один канал, инструмент и сэмпл - достаточно, чтобы
// Note-Trigger резолвился и dispatch_row_effects взвёл voice_active (без
// этого PortaUp/PortaDown ничего не делают, см. .h).
struct Fixture {
    soundsinth::model::Instrument instrument;
    soundsinth::model::Envelope envelope; // не подключена (instrument.volume_envelope==nullptr), пока тест явно не привяжет
    soundsinth::model::Envelope pan_envelope;   // то же самое, для instrument.panning_envelope
    soundsinth::model::Envelope pitch_envelope; // то же самое, для instrument.pitch_envelope
    soundsinth::model::SampleDescriptor sample;
    soundsinth::model::Song song;
    engine::ChannelState channels[1];
    engine::PlayState
        ps; // персистентный: GlobalVolumeSlide (PlayState::global_volume_slide_*) должен пережить несколько tick()/dispatch_row() подряд, как в секвенсоре
    engine::DispatchContext ctx;

    explicit Fixture(FrequencyModel model) {
        sample.length_samples           = 1000;
        sample.c5_speed                 = 8363;
        sample.default_volume           = 64;
        instrument.default_sample_index = 0;

        song.samples          = &sample;
        song.sample_count     = 1;
        song.instruments      = &instrument;
        song.instrument_count = 1;
        song.frequency_model  = model;

        ctx.channels = channels;
        ctx.song     = &song;
        ctx.ps       = &ps;
    }

    PatternCell note_on(uint8_t note, Effect fx = Effect::None, uint8_t param = 0, SlideRate rate = SlideRate::PerTick) {
        PatternCell cell;
        cell.note         = note;
        cell.instrument   = 1;
        cell.effect.type  = fx;
        cell.effect.param = param;
        cell.effect.rate  = rate;
        return cell;
    }

    PatternCell empty_with_effect(Effect fx, uint8_t param, SlideRate rate = SlideRate::PerTick) {
        PatternCell cell;
        cell.effect.type  = fx;
        cell.effect.param = param;
        cell.effect.rate  = rate;
        return cell;
    }

    PatternCell empty_with_volcol(soundsinth::model::VolumeColumnType vc_type, uint8_t vc_param) {
        PatternCell cell;
        cell.volume.type  = vc_type;
        cell.volume.param = vc_param;
        return cell;
    }

    void dispatch_row(const PatternCell& cell) { engine::dispatch_row_effects(&ctx, &cell, 1); }

    void tick(uint8_t tick_in_row) {
        ps.tick_in_row = tick_in_row;
        engine::apply_continuous_effects(ps, channels, 1, song.quirks, song.frequency_model, engine::kQ8One, ps.tick_in_row != 0);
    }

    // Живой MIDI: строка равна тику, номер тика в строке всегда нулевой, а
    // слайды при этом обязаны идти.
    void live_tick() {
        ps.tick_in_row = 0;
        engine::apply_continuous_effects(ps, channels, 1, song.quirks, song.frequency_model, engine::kQ8One,
                                         /*tick_slides=*/true);
    }

    // NoteDelay (см. .cpp): доигрывает отложенную ячейку, если она есть и
    // delayed_cell_tick совпадает с tick_in_row. Отдельно от tick():
    // TrackerEngine::advance_tick зовёт оба, а синтетика идёт мимо
    // TrackerEngine и вызывает нужное явно.
    void dispatch_delayed(uint16_t tick_in_row) { engine::dispatch_delayed_notes(&ctx, tick_in_row, 1); }
};

void test_porta_up_per_tick_decreases_period_each_tick() {
    std::printf("test_porta_up_per_tick_decreases_period_each_tick\n");

    Fixture f(FrequencyModel::Amiga);
    const PatternCell row = f.note_on(48, Effect::PortaUp, 10, SlideRate::PerTick);
    f.dispatch_row(row);

    CHECK_EQ(f.channels[0].period, soundsinth::model::amiga_note_to_period(48)); // 428 - ещё не сдвинут, тик 0 применяет только dispatch_row_effects
    CHECK(f.channels[0].porta_active);

    f.tick(1);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 - 10));
    f.tick(2);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 - 20));
    f.tick(3);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 - 30));
}

void test_porta_down_per_tick_increases_period_each_tick() {
    std::printf("test_porta_down_per_tick_increases_period_each_tick\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::PortaDown, 15, SlideRate::PerTick));

    f.tick(1);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 15));
    f.tick(2);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 30));
}

void test_porta_fine_applies_once_immediately_not_per_tick() {
    std::printf("test_porta_fine_applies_once_immediately_not_per_tick\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::PortaDown, 5, SlideRate::Fine));

    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 5)); // применилось сразу, внутри dispatch_row_effects
    CHECK(!f.channels[0].porta_active);                             // Fine не взводит continuous-путь

    f.tick(1); // не должно ничего сдвинуть дальше
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 5));
}

void test_porta_memory_reuses_last_nonzero_param() {
    std::printf("test_porta_memory_reuses_last_nonzero_param\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::PortaUp, 12, SlideRate::PerTick)); // память=12
    f.tick(1);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 - 12));

    // Новая строка, тот же эффект с param=0 - повторяет запомненные 12, а не
    // ноту (второй раз нота в этом сценарии не триггерится).
    f.dispatch_row(f.empty_with_effect(Effect::PortaUp, 0, SlideRate::PerTick));
    CHECK(f.channels[0].porta_active);
    f.tick(1);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 - 12 - 12));
}

// С kQuirkAmigaLimits (MOD ProTracker, S3M с флагом amigaLimits) портаменто
// упирается в края таблицы 113..856; без него - только в пределы 16 бит,
// как у OpenMPT без SONG_AMIGALIMITS.
void test_porta_clamps_to_amiga_period_table_bounds() {
    std::printf("test_porta_clamps_to_amiga_period_table_bounds\n");

    // Вверх (период уменьшается) - клэмп на минимум таблицы (113).
    {
        Fixture f(FrequencyModel::Amiga);
        f.song.quirks |= soundsinth::model::kQuirkAmigaLimits;
        f.dispatch_row(f.note_on(48, Effect::PortaUp, 250, SlideRate::PerTick));
        for (int i = 0; i < 10; ++i)
            f.tick(1); // многократно - заведомо за границу
        CHECK_EQ(f.channels[0].period, soundsinth::model::kAmigaPeriodTable[35]);
    }
    // Вниз (период увеличивается) - клэмп на максимум таблицы (856).
    {
        Fixture f(FrequencyModel::Amiga);
        f.song.quirks |= soundsinth::model::kQuirkAmigaLimits;
        f.dispatch_row(f.note_on(48, Effect::PortaDown, 250, SlideRate::PerTick));
        for (int i = 0; i < 10; ++i)
            f.tick(1);
        CHECK_EQ(f.channels[0].period, soundsinth::model::kAmigaPeriodTable[0]);
    }
    // Без квирка - дальше таблицы: 428 + 10 * 250.
    {
        Fixture f(FrequencyModel::Amiga);
        f.dispatch_row(f.note_on(48, Effect::PortaDown, 250, SlideRate::PerTick));
        for (int i = 0; i < 10; ++i)
            f.tick(1);
        CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 10 * 250));
    }
    // Вверх без квирка - до 1.
    {
        Fixture f(FrequencyModel::Amiga);
        f.dispatch_row(f.note_on(48, Effect::PortaUp, 250, SlideRate::PerTick));
        for (int i = 0; i < 10; ++i)
            f.tick(1);
        CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(1));
    }
}

// Исходно тест проверял безопасность: пока Porta под Linear-моделью
// (XM, современный IT) не была реализована (см. .cpp), период должен был
// оставаться нетронутым, а не уходить в 0 или мусор - TrackerEngine
// подставляет cs.period в знаменатель (kSampleRateHz*period) при
// синхронизации Voice::step, и period==0 там означает деление на ноль.
// Теперь под Linear-моделью PortaUp/PortaDown двигают cs.linear_pitch
// (единицы "1/64 полутона", см. engine/voice.h), а не cs.period. Знак
// противоположен Amiga-периоду: linear_pitch растёт с высотой ноты,
// период - наоборот (см. комментарий у Effect::PortaUp/PortaDown в .cpp).
// note=48 без kQuirkItLinearC5Reference (Fixture квирков не выставляет):
// опорная нота тоже 48, поэтому linear_pitch стартует с 0.
void test_porta_up_per_tick_under_linear_frequency_model() {
    std::printf("test_porta_up_per_tick_under_linear_frequency_model\n");

    Fixture f(FrequencyModel::Linear);
    f.dispatch_row(f.note_on(48, Effect::PortaUp, 10, SlideRate::PerTick));

    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(0)); // тик 0 ещё не сдвинул
    CHECK(f.channels[0].porta_active);
    CHECK_EQ(f.channels[0].porta_memory, static_cast<uint16_t>(40)); // param*4 под Linear (10*4)

    f.tick(1);
    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(40)); // PortaUp -> выше, значит linear_pitch растёт (в отличие от period у Amiga)
}

// Живой MIDI: сетка строка = тик, поэтому тик 1 не наступает никогда. Без
// отдельного признака слайдов скольжение высоты стояло бы на месте - нота
// звучала бы высотой предыдущей и не уходила с неё.
void test_tone_porta_moves_on_live_grid_without_tick_one() {
    std::printf("test_tone_porta_moves_on_live_grid_without_tick_one\n");

    Fixture f(FrequencyModel::Linear);
    f.dispatch_row(f.note_on(48, Effect::PortaUp, 10, SlideRate::PerTick));
    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(0));

    // Тот же тик по номеру, что и строка, - и высота всё же едет.
    f.live_tick();
    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(40));
    f.live_tick();
    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(80));

    // Для файлов условие прежнее: на нулевом тике не едет.
    Fixture g(FrequencyModel::Linear);
    g.dispatch_row(g.note_on(48, Effect::PortaUp, 10, SlideRate::PerTick));
    g.tick(0);
    CHECK_EQ(g.channels[0].linear_pitch, static_cast<int32_t>(0));
}

void test_porta_down_per_tick_under_linear_frequency_model() {
    std::printf("test_porta_down_per_tick_under_linear_frequency_model\n");

    Fixture f(FrequencyModel::Linear);
    f.dispatch_row(f.note_on(48, Effect::PortaDown, 10, SlideRate::PerTick));
    f.tick(1);
    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(-40)); // PortaDown -> ниже, linear_pitch убывает
}

// TonePorta не ретриггерит уже играющий голос (позиция декодера не
// сбрасывается; здесь достаточно проверить инварианты
// triggered_this_row/voice_active, сам декод проверяется в
// test_voice.cpp), а плавно сводит period к цели со скоростью
// porta_memory, не перескакивая её (в отличие от PortaUp/PortaDown,
// которые прибавляют без учёта цели).
void test_tone_porta_does_not_retrigger_and_converges_to_target_without_overshoot() {
    std::printf("test_tone_porta_does_not_retrigger_and_converges_to_target_without_overshoot\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48)); // обычный триггер - period=428
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428));

    const uint16_t target = soundsinth::model::amiga_note_to_period(60); // на октаву выше - период меньше
    CHECK(target < 428);

    f.dispatch_row(f.note_on(60, Effect::TonePorta, 10));       // ретриггер должен быть подавлен
    CHECK(!f.channels[0].triggered_this_row);                   // не ретриггернуло
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428)); // сам тик 0 ещё не сдвинул период
    CHECK_EQ(f.channels[0].tone_porta_target, target);
    CHECK(f.channels[0].tone_porta_active);

    for (int i = 0; i < 30; ++i)
        f.tick(1);                          // заведомо больше, чем нужно для схождения
    CHECK_EQ(f.channels[0].period, target); // сошёлся на цели, не перепрыгнул её
}

// TonePortaVolSlide (Lxy): то же подавление ретриггера и схождение к
// цели, что у TonePorta (is_tone_porta_family, см. .cpp), плюс
// VolumeSlide тем же param (общая память volume_slide_memory). param
// целиком отдан под слайд, porta_memory и target им не переустанавливаются
// ("продолжить порто", как у VibratoVolSlide).
void test_tone_porta_vol_slide_converges_and_applies_volume_slide() {
    std::printf("test_tone_porta_vol_slide_converges_and_applies_volume_slide\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::TonePorta, 10)); // заводим порто-память (10) обычным TonePorta
    const uint16_t target = soundsinth::model::amiga_note_to_period(60);
    f.dispatch_row(f.note_on(60, Effect::TonePortaVolSlide, 0x05)); // volume slide down=5, без своей porta-скорости
    CHECK(!f.channels[0].triggered_this_row);                       // ретриггер подавлен, как у обычного TonePorta
    CHECK_EQ(f.channels[0].tone_porta_target, target);
    CHECK(f.channels[0].tone_porta_active);
    CHECK(f.channels[0].volume_slide_active);
    CHECK_EQ(f.channels[0].volume_slide_memory, static_cast<uint8_t>(0x05));

    f.tick(1); // одного тика достаточно, чтобы проверить, что оба слайда движутся
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(59)); // 64-5 - volume-слайд применился
    CHECK(f.channels[0].period != target); // период ещё сходится (скорость 10, разница явно больше)

    for (int i = 0; i < 30; ++i)
        f.tick(1);                          // заведомо больше, чем нужно для схождения периода
    CHECK_EQ(f.channels[0].period, target); // порто всё равно сошёлся: скорость (10) взята из памяти PortaUp/Down/TonePorta
}

// Баг из 2nd_pm.s3m (канал 1, паттерн 26): нота с инструментом под
// TonePorta не перезапускает сэмпл (позиция декодера держится), но
// громкость всё равно сбрасывается на громкость сэмпла, если на этой
// строке нет явной volume-колонки. Так в OpenMPT soundlib/Snd_fx.cpp
// InstrumentChange(..., bUpdVol=true): блок "chn.nVolume = pSmp->nVolume"
// там не защищён проверкой bPorta. У нас это было завязано на тот же
// suppress_retrigger_for_tone_porta, что и сам ретриггер, и задуманная
// автором "пульсация" громкости пропадала (строки note+instr без
// volume-колонки сбрасывают громкость на громкость сэмпла, строки с
// volume-колонкой её перебивают).
void test_tone_porta_resets_volume_to_sample_default_without_retriggering() {
    std::printf("test_tone_porta_resets_volume_to_sample_default_without_retriggering\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48)); // обычный триггер - volume=64 (громкость сэмпла, см. Fixture)
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(64));

    f.dispatch_row(f.empty_with_effect(Effect::SetVolume, 20)); // явно понижаем громкость
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(20));

    // Нота с инструментом под TonePorta, без volume-колонки на этой строке:
    // ретриггер подавлен, но громкость должна вернуться к громкости сэмпла
    // (64), а не остаться на 20.
    f.dispatch_row(f.note_on(60, Effect::TonePorta, 10));
    CHECK(!f.channels[0].triggered_this_row);                 // ретриггер по-прежнему подавлен
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(64)); // но громкость сброшена на громкость сэмпла
}

void test_tone_porta_holds_target_when_row_has_no_note() {
    std::printf("test_tone_porta_holds_target_when_row_has_no_note\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48));
    const uint16_t target = soundsinth::model::amiga_note_to_period(60);
    f.dispatch_row(f.note_on(60, Effect::TonePorta, 10));
    f.tick(1); // period сдвинулся на один шаг, но ещё не дошёл до цели

    const uint16_t period_after_one_tick = f.channels[0].period;
    CHECK(period_after_one_tick != target);

    // Новая строка без ноты, TonePorta с param=0: цель не меняется, скорость
    // берётся из памяти, слайд продолжается.
    f.dispatch_row(f.empty_with_effect(Effect::TonePorta, 0));
    CHECK_EQ(f.channels[0].tone_porta_target, target);
    CHECK(f.channels[0].tone_porta_active);
    f.tick(1);
    CHECK(f.channels[0].period < period_after_one_tick); // продолжил сходиться дальше к цели
}

// GlissandoControl (MOD E3x / S3M,IT S1x): звучит ступенями по полутонам,
// а период TonePorta едет плавно, как в OpenMPT (Sndmix.cpp). Прижимался
// сам период - и слайд медленнее половины полутона за тик стоял на месте:
// 428 - 5 = 423 прижималось обратно к 428. Здесь проверяется, что период
// едет, флаг тон-портаменто на ноте выставлен, и прижатие
// amiga_snap_period в обоих режимах округления.
void test_glissando_control_snaps_tone_porta_to_nearest_note() {
    std::printf("test_glissando_control_snaps_tone_porta_to_nearest_note\n");

    Fixture g(FrequencyModel::Amiga);
    g.dispatch_row(g.note_on(48, Effect::GlissandoControl, 1)); // period=428, glissando до TonePorta
    CHECK(g.channels[0].glissando_enabled);
    CHECK(!g.channels[0].glissando_porta); // тон-портаменто на этой ноте ещё не было
    g.dispatch_row(g.note_on(51, Effect::TonePorta, 5, SlideRate::PerTick));
    CHECK(g.channels[0].glissando_porta);
    g.tick(1);
    CHECK_EQ(g.channels[0].period, static_cast<uint16_t>(423));
    g.tick(1);
    CHECK_EQ(g.channels[0].period, static_cast<uint16_t>(418)); // не стоит

    // Звучит: вверх по высоте - первая нота с периодом не больше (404,
    // C#), у FT2 - ближайшая (граница - среднее геометрическое 428 и 404,
    // около 415.8).
    CHECK_EQ(soundsinth::model::amiga_snap_period(423, false), static_cast<uint16_t>(404));
    CHECK_EQ(soundsinth::model::amiga_snap_period(423, true), static_cast<uint16_t>(428));
    CHECK_EQ(soundsinth::model::amiga_snap_period(415, true), static_cast<uint16_t>(404));
    CHECK_EQ(soundsinth::model::amiga_snap_period(428, false), static_cast<uint16_t>(428)); // на ноте - она сама
    // За пределами трёх октав таблицы - октавные сдвиги (S3M/IT).
    CHECK_EQ(soundsinth::model::amiga_snap_period(107, false), soundsinth::model::amiga_note_to_period(72));

    // Режим персистентный, не сбрасывается на строках без нового Sxx; флаг
    // тон-портаменто снимает только новая нота.
    g.dispatch_row(g.empty_with_effect(Effect::TonePorta, 0));
    CHECK(g.channels[0].glissando_enabled);
    g.dispatch_row(g.note_on(48));
    CHECK(!g.channels[0].glissando_porta);
}

// Панорама MOD по эвристикам Load_mod.cpp: метки синхронизации не двигают
// панораму, 7-битная шкала удваивается, A4 в ней - surround.
void test_mod_panning_quirks() {
    std::printf("test_mod_panning_quirks\n");

    Fixture f(FrequencyModel::Amiga);
    f.song.quirks = soundsinth::model::kQuirkModIgnorePanning;
    f.dispatch_row(f.note_on(48, Effect::SetPanning, 0x10));
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(32));
    f.dispatch_row(f.empty_with_effect(Effect::SetPanning4Bit, 15));
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(32));

    f.song.quirks = soundsinth::model::kQuirkMod7BitPanning;
    f.dispatch_row(f.empty_with_effect(Effect::SetPanning, 0x40)); // 0x80 в байтовой шкале
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(32));
    f.dispatch_row(f.empty_with_effect(Effect::SetPanning, 0x80));
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(63)); // 255 / 4
    f.dispatch_row(f.empty_with_effect(Effect::SetPanning, 0xA4));
    CHECK(f.channels[0].surround);
}

// Arpeggio в колонке эффекта и вибрато из колонки громкости на одном тике
// складываются, как у OpenMPT (ProcessArpeggio, затем ProcessVibrato).
// Раньше действовало только арпеджио. Прямоугольная волна на фазе 0 даёт
// +255 без раскачки фазы: вибрато 255 * 32 / 64 = 127 единиц.
void test_arpeggio_with_volume_column_vibrato() {
    std::printf("test_arpeggio_with_volume_column_vibrato\n");

    Fixture f(FrequencyModel::Linear);
    f.dispatch_row(f.note_on(48, Effect::SetVibratoWaveform, 2));
    PatternCell cell  = f.empty_with_effect(Effect::Arpeggio, 0x40);
    cell.volume.type  = VolumeColumnType::VibratoDepth;
    cell.volume.param = 8; // глубина 8 << 2 = 32
    f.dispatch_row(cell);
    f.tick(1); // фаза арпеджио 1: +4 полутона
    CHECK_EQ(f.channels[0].pitch_offset, 4 * 64 + 127);
    f.tick(3); // фаза 0: только вибрато
    CHECK_EQ(f.channels[0].pitch_offset, 127);
}

// SetVibratoWaveform/SetTremoloWaveform/SetPanbrelloWaveform (S3x/S4x/S5x
// IT,S3M) переключают форму волны LFO (0=синус/умолчание, 1=спад,
// 2=прямоугольник, см. lfo_waveform_value в .cpp). Проверяется: (a) синус
// на фазе 0 даёт offset=0 (table[0]==0), (b) после переключения на
// прямоугольник (2) та же фаза 0 даёт максимальное положительное
// отклонение (phase<32 -> +255), а не 0 - волна переключилась, а не
// прочитан старый синус.
void test_set_vibrato_waveform_selects_square_instead_of_sine() {
    std::printf("test_set_vibrato_waveform_selects_square_instead_of_sine\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x44)); // speed=4, depth=4(<<2=16)
    f.tick(0);
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(0));     // синус: table[0]==0
    CHECK_EQ(f.channels[0].vibrato_waveform, static_cast<uint8_t>(0)); // умолчание - синус

    Fixture g(FrequencyModel::Amiga);
    g.dispatch_row(g.note_on(48, Effect::SetVibratoWaveform, 2)); // прямоугольник, до Vibrato
    CHECK_EQ(g.channels[0].vibrato_waveform, static_cast<uint8_t>(2));
    g.dispatch_row(g.empty_with_effect(Effect::Vibrato, 0x44));
    g.tick(0); // прямоугольник: phase(0)<32 -> raw=255*16=4080, offset=4080/512=7 (целочисленно)
    CHECK_EQ(g.channels[0].pitch_offset, static_cast<int32_t>(7));
}

// Формы 4..7 - те же 0..3, но фаза на новой ноте не сбрасывается; у IT
// тремоло не сбрасывается и при 0..3, как у OpenMPT.
void test_lfo_waveform_no_retrigger_bit() {
    std::printf("test_lfo_waveform_no_retrigger_bit\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::SetTremoloWaveform, 6)); // 6 & 3 == 2, прямоугольник
    CHECK_EQ(f.channels[0].tremolo_waveform, static_cast<uint8_t>(6));
    f.dispatch_row(f.empty_with_effect(Effect::Tremolo, 0x44));
    f.tick(1); // прямоугольник на фазе 0: 255 * 4 / 64
    CHECK_EQ(f.channels[0].volume_offset, static_cast<int16_t>(15));
    f.dispatch_row(f.empty_with_effect(Effect::SetVibratoWaveform, 4));
    f.dispatch_row(f.empty_with_effect(Effect::Vibrato, 0x44));
    f.tick(1);
    f.dispatch_row(f.note_on(50));
    CHECK_EQ(f.channels[0].tremolo_phase, static_cast<uint8_t>(4)); // не сброшены
    CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(4));
    f.dispatch_row(f.empty_with_effect(Effect::SetVibratoWaveform, 0));
    f.dispatch_row(f.note_on(48));
    CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(0));

    Fixture it(FrequencyModel::Linear);
    it.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn | soundsinth::model::kQuirkItVibratoTable;
    it.dispatch_row(it.note_on(48, Effect::Tremolo, 0x44));
    it.tick(1);
    it.dispatch_row(it.note_on(50));
    CHECK_EQ(it.channels[0].tremolo_phase, static_cast<uint8_t>(16)); // IT: тремоло не сбрасывается
}

// SetPanbrelloWaveform - своя, независимая память формы волны (отдельно
// от vibrato_waveform/tremolo_waveform).
void test_set_panbrello_waveform_is_independent_of_vibrato_waveform() {
    std::printf("test_set_panbrello_waveform_is_independent_of_vibrato_waveform\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::SetVibratoWaveform, 1));
    f.dispatch_row(f.empty_with_effect(Effect::SetPanbrelloWaveform, 2));
    CHECK_EQ(f.channels[0].vibrato_waveform, static_cast<uint8_t>(1));
    CHECK_EQ(f.channels[0].panbrello_waveform, static_cast<uint8_t>(2));
    CHECK_EQ(f.channels[0].tremolo_waveform, static_cast<uint8_t>(0)); // не тронута
}

// SetFinetune (MOD E5x / S3M,IT S2x): param - знаковый ниббл -8..7,
// применяется как непрерывная поправка к period сразу на Note-Trigger той
// же строки (формула и сверка с libxmp src/effects.c EX_FINETUNE и
// period.c - в .cpp). param=8 -> ниббл=-8 -> finetune=-128 (в шкале
// "128 ед/полутон") -> period=428*2^(1/12)=453 (finetune ниже - период
// выше, период обратно пропорционален частоте).
void test_set_finetune_shifts_period_on_same_row_trigger_amiga() {
    std::printf("test_set_finetune_shifts_period_on_same_row_trigger_amiga\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::SetFinetune, 8));
    CHECK(f.channels[0].finetune_override_active);
    CHECK_EQ(f.channels[0].finetune_override, static_cast<int8_t>(-128));
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(453));
}

// Linear-модель: param=7 -> ниббл=7 -> finetune=112 -> linear_pitch +=
// 112*64/128=56 единиц "1/64 полутона" (см. .cpp).
void test_set_finetune_shifts_linear_pitch_on_same_row_trigger_linear() {
    std::printf("test_set_finetune_shifts_linear_pitch_on_same_row_trigger_linear\n");

    Fixture f(FrequencyModel::Linear);
    f.dispatch_row(f.note_on(48, Effect::SetFinetune, 7));
    CHECK_EQ(f.channels[0].finetune_override, static_cast<int8_t>(112));
    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(56));
}

// Override действует на канал и временно: следующий Note-Trigger без
// своего SetFinetune возвращает канал к finetune сэмпла (см. .h). Здесь
// finetune сэмпла==0, поэтому period снова табличный (428).
void test_set_finetune_resets_on_next_trigger_without_command() {
    std::printf("test_set_finetune_resets_on_next_trigger_without_command\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::SetFinetune, 8));
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(453));

    f.dispatch_row(f.note_on(48)); // новый триггер, без SetFinetune на этой строке
    CHECK(!f.channels[0].finetune_override_active);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428));
}

// Собственный (статический) finetune сэмпла (SampleDescriptor::finetune,
// загрузчик заполняет его из MOD-инструмента, см. formats/mod.cpp) раньше
// молча игнорировался при Note-Trigger. Это был баг: MOD с ненулевым
// finetune играл на неверной высоте на каждой ноте, а не только с E5x.
// finetune=64 (полполутона) -> period=428*2^(-64/128/12)=416 без команд
// SetFinetune.
void test_sample_own_finetune_applies_without_any_effect_command() {
    std::printf("test_sample_own_finetune_applies_without_any_effect_command\n");

    Fixture f(FrequencyModel::Amiga);
    f.sample.finetune = 64;
    f.dispatch_row(f.note_on(48));
    CHECK(!f.channels[0].finetune_override_active); // не override, а собственный finetune сэмпла
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(416));
}

// NoteDelay (MOD EDx / S3M,IT SDx / XM EDx): вся ячейка
// (нота+инструмент+volume+effect) откладывается до тика param внутри
// строки (см. ChannelState::delayed_cell в .h; libxmp src/player.c
// check_delay).
void test_note_delay_defers_trigger_to_specified_tick() {
    std::printf("test_note_delay_defers_trigger_to_specified_tick\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::NoteDelay, 3));
    CHECK(!f.channels[0].voice_active); // тик 0 строки - ячейка отложена, не применена
    CHECK(!f.channels[0].triggered_this_row);
    CHECK_EQ(f.channels[0].delayed_cell_tick, static_cast<uint16_t>(3));

    f.dispatch_delayed(1); // не тот тик - ничего не происходит
    CHECK(!f.channels[0].voice_active);

    f.dispatch_delayed(3); // нужный тик - ячейка доигрывается целиком
    CHECK(f.channels[0].voice_active);
    CHECK(f.channels[0].triggered_this_row);
    CHECK_EQ(f.channels[0].last_note, static_cast<uint8_t>(48));
    CHECK_EQ(f.channels[0].delayed_cell_tick, static_cast<uint16_t>(0)); // потреблена, повторно не сработает

    f.dispatch_delayed(3); // повторный вызов с тем же тиком - уже нечего доигрывать
    CHECK_EQ(f.channels[0].delayed_cell_tick, static_cast<uint16_t>(0));
}

// param==0 - "без задержки": ячейка не откладывается и обрабатывается
// сразу, как обычная нота (тот же принцип "0=выключено", что у
// большинства Sxx/Exx-команд в этом проекте).
void test_note_delay_with_zero_param_triggers_immediately() {
    std::printf("test_note_delay_with_zero_param_triggers_immediately\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::NoteDelay, 0));
    CHECK(f.channels[0].voice_active);
    CHECK_EQ(f.channels[0].delayed_cell_tick, static_cast<uint16_t>(0));
}

// Отложенная ячейка не переживает границу строки: новая строка (даже без
// своего NoteDelay) отменяет невыстрелившую отсрочку с предыдущей (см.
// .h/.cpp), иначе устаревшая ячейка могла бы выстрелить посреди другой
// строки при совпадении тиков.
void test_note_delay_pending_cell_cancelled_by_next_row() {
    std::printf("test_note_delay_pending_cell_cancelled_by_next_row\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::NoteDelay, 5)); // отложено на тик 5
    CHECK_EQ(f.channels[0].delayed_cell_tick, static_cast<uint16_t>(5));

    f.dispatch_row(f.empty_with_effect(Effect::None, 0)); // новая строка без NoteDelay - отменяет старую отсрочку
    CHECK_EQ(f.channels[0].delayed_cell_tick, static_cast<uint16_t>(0));

    f.dispatch_delayed(5); // если бы отсрочка выжила, здесь бы выстрелила
    CHECK(!f.channels[0].voice_active);
}

// Строка с отложенной ячейкой флаги прошлой строки не сбрасывает до
// доигрыша: вибрато 4xy идёт на тиках 1-2 строки с ED3 без эффекта и
// снимается на тике 3, когда ячейка доиграна.
void test_note_delay_keeps_previous_row_effects_until_replay() {
    std::printf("test_note_delay_keeps_previous_row_effects_until_replay\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x48));
    CHECK(f.channels[0].vibrato_active);
    f.dispatch_row(f.note_on(50, Effect::NoteDelay, 3));
    CHECK(f.channels[0].vibrato_active);
    f.dispatch_delayed(1);
    f.dispatch_delayed(2);
    CHECK(f.channels[0].vibrato_active);
    f.dispatch_delayed(3);
    CHECK(!f.channels[0].vibrato_active);
    CHECK_EQ(f.channels[0].last_note, static_cast<uint8_t>(50));
}

void test_tone_porta_first_note_on_channel_triggers_normally() {
    std::printf("test_tone_porta_first_note_on_channel_triggers_normally\n");

    Fixture f(FrequencyModel::Amiga);
    // TonePorta на канале без играющего голоса: сходиться не от чего,
    // трекеры в этом случае просто триггерят ноту.
    f.dispatch_row(f.note_on(48, Effect::TonePorta, 10));
    CHECK(f.channels[0].triggered_this_row);
    CHECK(f.channels[0].voice_active);
    CHECK_EQ(f.channels[0].period, soundsinth::model::amiga_note_to_period(48));
    CHECK_EQ(f.channels[0].tone_porta_target, f.channels[0].period); // цель совпадает с текущим - без изгиба

    f.tick(1); // не должно никуда сдвинуть - уже на цели
    CHECK_EQ(f.channels[0].period, soundsinth::model::amiga_note_to_period(48));
}

// Linear-модель: тот же сценарий, что
// test_tone_porta_does_not_retrigger_and_converges_to_target_without_overshoot
// выше, но на cs.linear_pitch/linear_tone_porta_target (единицы
// "1/64 полутона"). note=48->60 (октава выше) => target=(60-48)*64=768.
// Знак противоположен Amiga-периоду (там период уменьшается, здесь
// linear_pitch растёт); apply_tone_porta_linear сходится к цели в любую
// сторону, так что схождение работает без изменений.
void test_tone_porta_converges_to_target_under_linear_frequency_model() {
    std::printf("test_tone_porta_converges_to_target_under_linear_frequency_model\n");

    Fixture f(FrequencyModel::Linear);
    f.dispatch_row(f.note_on(48)); // обычный триггер - linear_pitch=0 (нота==опорной)
    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(0));

    constexpr int32_t kTarget = 12 * engine::kLinearAmountUnitsPerSemitone; // октава выше 48 = +768
    f.dispatch_row(f.note_on(60, Effect::TonePorta, 10));                   // ретриггер должен быть подавлен
    CHECK(!f.channels[0].triggered_this_row);
    CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(0)); // тик 0 ещё не сдвинул
    CHECK_EQ(f.channels[0].linear_tone_porta_target, kTarget);
    CHECK(f.channels[0].tone_porta_active);

    for (int i = 0; i < 30; ++i)
        f.tick(1);                                 // заведомо больше, чем нужно для схождения
    CHECK_EQ(f.channels[0].linear_pitch, kTarget); // сошёлся на цели, не перепрыгнул её
}

// Vibrato - временное отклонение (pitch_offset), сам period не трогается
// (в отличие от Porta/TonePorta). Формула и таблица сверены с libxmp
// src/lfo.c/effects.c (см. .cpp); table[4] == 97 - известное публичное
// значение таблицы ProTracker. Вибрато действует и на тике 0, но фаза там
// стоит (MOD, S3M, XM), как у OpenMPT и libxmp.
void test_vibrato_phase_holds_on_tick_zero_and_does_not_touch_period() {
    std::printf("test_vibrato_phase_holds_on_tick_zero_and_does_not_touch_period\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x44));           // speed=4, depth=4(<<2=16)
    CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(0)); // сброшен триггером
    CHECK(f.channels[0].vibrato_active);

    f.tick(0);
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int16_t>(0));  // table[0]==0
    CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(0)); // тик 0 - фаза стоит
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428));     // сам period не тронут

    f.tick(1);
    CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(4)); // фаза продвинулась на speed
    f.tick(2);                                                      // table[4]==97, offset = 97*16/512 = 3 (целочисленно)
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int16_t>(3));
    f.tick(0); // новый тик 0: действует фаза 8, сама не двигается
    CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(8));
    CHECK(f.channels[0].pitch_offset != 0);
}

void test_vibrato_offset_clears_when_not_respecified_on_later_row() {
    std::printf("test_vibrato_offset_clears_when_not_respecified_on_later_row\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x44));
    f.tick(1);
    f.tick(2);
    CHECK(f.channels[0].pitch_offset != 0);

    f.dispatch_row(f.empty_with_effect(Effect::None, 0)); // строка без Vibrato
    CHECK(!f.channels[0].vibrato_active);
    f.tick(0);
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int16_t>(0)); // сброшен, хотя фаза не сброшена
}

void test_vibrato_phase_resets_on_retrigger() {
    std::printf("test_vibrato_phase_resets_on_retrigger\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x44));
    f.tick(1);
    f.tick(2);
    f.tick(3);
    CHECK(f.channels[0].vibrato_phase != 0);

    f.dispatch_row(f.note_on(50)); // новый триггер, без Vibrato на этой строке
    CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(0));
}

void test_vibrato_speed_and_depth_memory_are_independent() {
    std::printf("test_vibrato_speed_and_depth_memory_are_independent\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x64)); // speed=6, depth=4(<<2=16)
    CHECK_EQ(f.channels[0].vibrato_speed, static_cast<uint8_t>(6));
    CHECK_EQ(f.channels[0].vibrato_depth, static_cast<uint8_t>(16));

    f.dispatch_row(f.empty_with_effect(Effect::Vibrato, 0x08));      // speed=0(повторяет 6), depth=8(<<2=32)
    CHECK_EQ(f.channels[0].vibrato_speed, static_cast<uint8_t>(6));  // не изменилась
    CHECK_EQ(f.channels[0].vibrato_depth, static_cast<uint8_t>(32)); // изменилась
}

// Linear-модель: pitch_offset в единицах linear_pitch. XM - как FT2:
// (таблица 0..255 * нибл) >> 5, высота сначала вниз, фаза на тике 0 стоит.
// .mid - делитель 64, вверх, фаза каждый тик (под эту шкалу конвертер
// считает CC1).
void test_vibrato_applies_under_linear_frequency_model() {
    std::printf("test_vibrato_applies_under_linear_frequency_model\n");
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkXmVolColumnBeforeEffect;
        f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x44)); // speed=4, depth=4(<<2=16)
        CHECK(f.channels[0].vibrato_active);
        f.tick(0);
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(0)); // table[0]==0
        CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(0));
        CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(0)); // сам linear_pitch не тронут
        f.tick(1);
        f.tick(2); // table[4]==97: -(97 * 16 / 128) = -12
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(-12));
    }
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkFadeoutExponential;
        f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x44));
        f.tick(0);
        CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(4));
        f.tick(0); // table[4]==97, offset = 97*16/64 = 24 (целочисленно)
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(24));
    }
}

// FineVibrato (Effect::FineVibrato, S3M/IT Uxy): та же механика и таблица,
// что у Vibrato, но depth без множителя <<2 (см. .cpp) - единственное
// отличие от Effect::Vibrato на уровне разбора параметра.
void test_fine_vibrato_depth_has_no_shift_unlike_vibrato() {
    std::printf("test_fine_vibrato_depth_has_no_shift_unlike_vibrato\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::FineVibrato, 0x44)); // speed=4, depth=4 (без <<2, в отличие от Vibrato)
    CHECK(f.channels[0].vibrato_active);
    CHECK_EQ(f.channels[0].vibrato_speed, static_cast<uint8_t>(4));
    CHECK_EQ(f.channels[0].vibrato_depth, static_cast<uint8_t>(4)); // не 16, как было бы у Effect::Vibrato с тем же param
}

// VibratoVolSlide (Effect::VibratoVolSlide, MOD 6xy/S3M,IT Kxy/XM 6xy):
// "продолжить текущее вибрато" (память и фаза не трогаются) + VolumeSlide
// тем же param, что у Effect::VolumeSlide (общая память), см. .cpp.
void test_vibrato_vol_slide_continues_vibrato_and_applies_volume_slide() {
    std::printf("test_vibrato_vol_slide_continues_vibrato_and_applies_volume_slide\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x64)); // speed=6, depth=4(<<2=16) - заводим вибрато
    f.tick(0);
    f.tick(0);
    const uint8_t phase_before = f.channels[0].vibrato_phase;
    const uint8_t speed_before = f.channels[0].vibrato_speed;
    const uint8_t depth_before = f.channels[0].vibrato_depth;

    f.dispatch_row(f.empty_with_effect(Effect::VibratoVolSlide, 0x05)); // volume slide down=5 (volume уже на максимуме 64, up сразу упёрся бы в потолок)
    CHECK(f.channels[0].vibrato_active);                                // вибрато продолжается
    CHECK_EQ(f.channels[0].vibrato_speed, speed_before); // память не тронута
    CHECK_EQ(f.channels[0].vibrato_depth, depth_before);
    CHECK_EQ(f.channels[0].vibrato_phase, phase_before); // фаза не сброшена (это не ретриггер)
    CHECK(f.channels[0].volume_slide_active);
    CHECK_EQ(f.channels[0].volume_slide_memory, static_cast<uint8_t>(0x05));

    const uint8_t volume_before = f.channels[0].volume;
    f.tick(1);
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(volume_before - 5)); // slide применился
}

// Kxy/Lxy - тот же слайд, что Dxy, как у OpenMPT: у IT два ненулевых нибла не
// слайдят; у настоящего ST3 тонкий вариант не играет, у прочих S3M играет.
void test_combined_volume_slides_by_format() {
    std::printf("test_combined_volume_slides_by_format\n");
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn;
        f.dispatch_row(f.note_on(48));
        f.dispatch_row(f.empty_with_effect(Effect::VibratoVolSlide, 0x23));
        CHECK(!f.channels[0].volume_slide_active);
        f.dispatch_row(f.empty_with_effect(Effect::TonePortaVolSlide, 0x20));
        CHECK(f.channels[0].volume_slide_active);
    }
    for (bool st3 : {false, true}) {
        Fixture f(FrequencyModel::Amiga);
        f.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkS3mVolSlideDownPriority;
        if (st3) f.song.quirks |= soundsinth::model::kQuirkS3mIgnoreCombinedFineSlides;
        f.dispatch_row(f.note_on(48, Effect::SetVolume, 32));
        f.dispatch_row(f.empty_with_effect(Effect::VibratoVolSlide, 0x2F)); // K2F - тонко вверх на 2
        CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(st3 ? 32 : 34));
        CHECK(!f.channels[0].volume_slide_active);
    }
}

// Тонкие слайды в параметре S3M/IT (kQuirkFineSlideInParam): xF? и ?F -
// разово на тике 0; у портаменто xE? - вчетверо мельче, EF0 и EE0 ничего не
// делают, память хранит сырой байт (E00 после EF3 - снова тонко на 3).
void test_fine_slides_in_param() {
    std::printf("test_fine_slides_in_param\n");
    {
        Fixture f(FrequencyModel::Amiga); // S3M
        f.song.quirks = soundsinth::model::kQuirkFineSlideInParam;
        f.dispatch_row(f.note_on(48, Effect::PortaDown, 0xF3)); // EF3
        CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 3));
        f.tick(1);
        CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 3)); // на тиках 1.. не меняется
        f.dispatch_row(f.empty_with_effect(Effect::PortaDown, 0x00));   // E00 - сырой EF3 из памяти
        CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 6));
        f.dispatch_row(f.empty_with_effect(Effect::PortaDown, 0xE3)); // EE3 - (3 + 2) / 4 = 1
        CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 7));
        f.dispatch_row(f.empty_with_effect(Effect::PortaDown, 0xF0)); // EF0 - ничего
        f.tick(1);
        CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 7));
        CHECK(!f.channels[0].porta_active);
        f.dispatch_row(f.empty_with_effect(Effect::PortaUp, 0xE0)); // FE0 - тоже ничего
        f.tick(1);
        CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428 + 7));
        CHECK(!f.channels[0].porta_active);
    }
    {
        // IT: EF0 и EE0 - ни шага, ни слайда 240 (15 полутонов за тик).
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn;
        f.dispatch_row(f.note_on(48));
        const int32_t pitch = f.channels[0].linear_pitch;
        for (uint8_t raw : {uint8_t(0xf0), uint8_t(0xe0)}) {
            f.dispatch_row(f.empty_with_effect(Effect::PortaDown, raw));
            f.tick(1);
            CHECK_EQ(f.channels[0].linear_pitch, pitch);
            CHECK(!f.channels[0].porta_active);
        }
    }
    {
        Fixture f(FrequencyModel::Linear); // IT
        f.song.quirks = soundsinth::model::kQuirkFineSlideInParam;
        f.dispatch_row(f.note_on(48, Effect::PortaUp, 0xF3)); // FF3 - 3 * 4
        CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(12));
        f.dispatch_row(f.empty_with_effect(Effect::PortaUp, 0xE3)); // FE3 - 3
        CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(15));
    }
    {
        // IT без Compatible Gxx: G20 пишет и память Fxx - F00 скользит на 0x20 * 4 за тик.
        Fixture f(FrequencyModel::Linear);
        f.song.quirks =
            soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn | soundsinth::model::kQuirkGxxSharesPortaMemory;
        f.dispatch_row(f.note_on(48));
        f.dispatch_row(f.empty_with_effect(Effect::TonePorta, 0x20));
        f.dispatch_row(f.empty_with_effect(Effect::PortaUp, 0x00));
        f.tick(1);
        CHECK_EQ(f.channels[0].linear_pitch, static_cast<int32_t>(128));
    }
    {
        // Громкость: DF? вверх, D?F вниз разово на тике 0; то же у Nxy и Wxy.
        Fixture f(FrequencyModel::Amiga);
        f.song.quirks = soundsinth::model::kQuirkFineSlideInParam;
        f.dispatch_row(f.note_on(48, Effect::SetVolume, 32));
        f.dispatch_row(f.empty_with_effect(Effect::VolumeSlide, 0xFF)); // FF - тонко вверх на 15
        CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(47));
        f.tick(1);
        CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(47));
        f.dispatch_row(f.empty_with_effect(Effect::VolumeSlide, 0x3F)); // 3F - тонко вверх на 3
        CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(50));
        f.dispatch_row(f.empty_with_effect(Effect::VolumeSlide, 0xF2)); // F2 - тонко вниз на 2
        CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(48));
        f.dispatch_row(f.empty_with_effect(Effect::ChannelVolumeSlide, 0xF1)); // NF1
        CHECK_EQ(f.channels[0].channel_volume, static_cast<uint8_t>(63));
        f.dispatch_row(f.empty_with_effect(Effect::GlobalVolumeSlide, 0xF1)); // WF1 - 1 из 64 у S3M
        CHECK_EQ(f.ps.global_volume, static_cast<uint8_t>(126));
        f.tick(1);
        CHECK_EQ(f.channels[0].channel_volume, static_cast<uint8_t>(63));
        CHECK_EQ(f.ps.global_volume, static_cast<uint8_t>(126));
    }
}

// LFO настоящего IT (kQuirkItVibratoTable): таблица +-64 на 256 точек, фаза
// +4 * speed за тик, вибрато делится на 64 (при Old Effects - на 32 и
// сначала вниз, фаза на тике 0 стоит), тремоло на 32; пила и асимметричный
// меандр - формулы OpenMPT.
void test_it_lfo() {
    std::printf("test_it_lfo\n");
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkItVibratoTable;
        f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x44)); // скорость 4, глубина 16
        f.tick(0);
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(0));
        CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(16));
        f.tick(1); // таблица[16] = 24: 24 * 16 / 64
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(6));
    }
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkItVibratoTable | soundsinth::model::kQuirkItOldEffects;
        f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x44));
        f.tick(0);
        CHECK_EQ(f.channels[0].vibrato_phase, static_cast<uint8_t>(0));
        f.tick(1);
        f.tick(2); // фаза 16: -(24 * 16 / 32)
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(-12));
    }
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkItVibratoTable;
        f.dispatch_row(f.note_on(48, Effect::Tremolo, 0x44)); // скорость 4, глубина 4
        f.channels[0].tremolo_phase = 16;
        f.tick(1); // 24 * 4 / 32
        CHECK_EQ(f.channels[0].volume_offset, static_cast<int16_t>(3));
    }
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkItVibratoTable;
        f.dispatch_row(f.note_on(48, Effect::SetVibratoWaveform, 1)); // S31 - пила
        f.dispatch_row(f.empty_with_effect(Effect::Vibrato, 0x44));
        f.tick(0); // фаза 0: 64 - (0 + 1) / 2 = 64, 64 * 16 / 64
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(16));
        f.dispatch_row(f.empty_with_effect(Effect::SetVibratoWaveform, 2)); // S32 - меандр
        f.dispatch_row(f.empty_with_effect(Effect::Vibrato, 0x44));
        f.channels[0].vibrato_phase = 128;
        f.tick(1); // вторая половина асимметричного меандра - 0
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(0));
        f.channels[0].vibrato_phase = 0;
        f.tick(1); // первая - 64
        CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(16));
    }
}

// Ветки диспетчера, которые проходит только .mid.
void test_midi_dispatch() {
    std::printf("test_midi_dispatch\n");
    {
        // (а) Абсолютный бенд: у звучащей ноты доезд к цели за строку шагом
        // (цель - 128) / speed, последний шаг садится на цель; Fine - цель x4;
        // ближе шага - сразу. Нота, взятая той же ячейкой, - сразу на бенде.
        Fixture f(FrequencyModel::Linear);
        f.ps.speed = 6;
        f.dispatch_row(f.note_on(48));
        f.dispatch_row(f.empty_with_effect(Effect::SetPitchOffset, 192));
        CHECK_EQ(f.channels[0].bend_target, static_cast<int32_t>(64));
        const int32_t want[7] = {10, 20, 30, 40, 50, 60, 64};
        for (int t = 0; t < 7; ++t) {
            f.tick(static_cast<uint8_t>(t % 6));
            CHECK_EQ(f.channels[0].bend_offset, want[t]);
        }
        f.dispatch_row(f.empty_with_effect(Effect::SetPitchOffset, 192, SlideRate::Fine));
        CHECK_EQ(f.channels[0].bend_target, static_cast<int32_t>(256));
        Fixture g(FrequencyModel::Linear);
        g.ps.speed = 6;
        g.dispatch_row(g.note_on(48, Effect::SetPitchOffset, 131)); // разность 3 меньше speed - сразу
        CHECK_EQ(g.channels[0].bend_offset, static_cast<int32_t>(3));
        Fixture h(FrequencyModel::Linear);
        h.ps.speed = 1;
        h.dispatch_row(h.note_on(48));
        h.dispatch_row(h.empty_with_effect(Effect::SetPitchOffset, 192));
        h.tick(0); // speed 1 - на цели за первый тик
        CHECK_EQ(h.channels[0].bend_offset, static_cast<int32_t>(64));
        Fixture k(FrequencyModel::Linear);
        k.ps.speed = 6;
        k.dispatch_row(k.note_on(48, Effect::SetPitchOffset, 58));  // чужой бенд слота: -70
        k.dispatch_row(k.note_on(50, Effect::SetPitchOffset, 128)); // новая нота со своим 0
        CHECK_EQ(k.channels[0].bend_offset, static_cast<int32_t>(0));
        CHECK_EQ(k.channels[0].bend_step, static_cast<int16_t>(0));
    }
    {
        // (б) Срез от силы удара: срез инструмента 40 + (громкость - 32) * 32 / 32.
        for (uint8_t vol : {uint8_t(64), uint8_t(0)}) {
            Fixture f(FrequencyModel::Linear);
            f.instrument.filter_cutoff      = 0x80 | 40;
            f.instrument.velocity_to_cutoff = 32;
            PatternCell cell                = f.note_on(60);
            cell.volume.type                = VolumeColumnType::SetVolume;
            cell.volume.param               = vol;
            f.dispatch_row(cell);
            CHECK_EQ(f.channels[0].filter_cutoff, static_cast<uint8_t>(vol == 64 ? 72 : 8));
        }
    }
    {
        // (в) Срез не ниже основного тона: 16 делений на октаву, нота 72 -
        // 16 * 24 / 12 + запас в октаву 16 = 48.
        Fixture f(FrequencyModel::Linear);
        f.song.filter_follows_note     = true;
        f.song.filter_units_per_octave = 16;
        f.instrument.filter_cutoff     = 0x80 | 40;
        f.dispatch_row(f.note_on(72));
        CHECK_EQ(f.channels[0].filter_cutoff, static_cast<uint8_t>(16 * 24 / 12 + SOUNDSINTH_MIDI_FILTER_NOTE_MARGIN * 16 / 24));
    }
    for (uint32_t rate : {0u, 60000u}) {
        // (г) Затухание в децибелах: нулевое не опускает громкость вовсе, 60000
        // обнуляется на пороге 256 на 63-м тике и снимает голос; огибающая
        // отпущенной ноты стоит.
        Fixture f(FrequencyModel::Linear);
        f.song.quirks                = soundsinth::model::kQuirkFadeoutExponential;
        f.envelope.enabled           = true;
        f.envelope.sustain_enabled   = true;
        f.envelope.point_count       = 2;
        f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
        f.envelope.points[1]         = soundsinth::model::EnvelopePoint{10, 0};
        f.envelope.sustain_point     = 0;
        f.envelope.sustain_end       = 0;
        f.instrument.volume_envelope = &f.envelope;
        f.instrument.fadeout_rate    = rate;
        f.dispatch_row(f.note_on(48));
        for (int i = 0; i < 3; ++i)
            f.tick(0);
        f.dispatch_row(f.empty_with_effect(Effect::KeyOff, 0));
        const uint16_t env_tick = f.channels[0].envelope_tick;
        const int ticks         = rate == 0 ? 100 : 62;
        for (int i = 0; i < ticks; ++i)
            f.tick(0);
        CHECK_EQ(f.channels[0].envelope_tick, env_tick);
        CHECK(f.channels[0].voice_active);
        if (rate == 0) {
            CHECK_EQ(f.channels[0].fadeout_level, 65536u);
        } else {
            CHECK(f.channels[0].fadeout_level >= 256u);
            f.tick(0);
            CHECK_EQ(f.channels[0].fadeout_level, 0u);
            CHECK(!f.channels[0].voice_active);
        }
    }
    {
        // (г) Огибающая дошла до нуля у удержанной ноты - голос жив; после
        // отпускания на нуле - снят.
        Fixture f(FrequencyModel::Linear);
        f.song.quirks                = soundsinth::model::kQuirkFadeoutExponential;
        f.envelope.enabled           = true;
        f.envelope.point_count       = 2;
        f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
        f.envelope.points[1]         = soundsinth::model::EnvelopePoint{10, 0};
        f.instrument.volume_envelope = &f.envelope;
        f.dispatch_row(f.note_on(48));
        for (int i = 0; i < 12; ++i)
            f.tick(0);
        CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(0));
        CHECK(f.channels[0].voice_active);
        f.dispatch_row(f.empty_with_effect(Effect::KeyOff, 0));
        f.tick(0);
        CHECK(!f.channels[0].voice_active);
    }
}

// Затухание по правилам IT (kQuirkItEnvelopeSustainLoop), как у OpenMPT:
// (а) огибающая без петли прошла последнюю точку - затухание и без Note-Off;
// (б) === у огибающей без петли - отпускание, затухание с её концом;
// (в) без огибающей и с нулевым fadeout === голос не снимает;
// (г) ^^^ - затухание, удержание огибающей остаётся.
void test_it_fade_rules() {
    std::printf("test_it_fade_rules\n");
    using soundsinth::model::EnvelopePoint;
    PatternCell off;
    off.note = soundsinth::model::kNoteOff;
    PatternCell fade;
    fade.note = soundsinth::model::kNoteFade;
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks                = soundsinth::model::kQuirkItEnvelopeSustainLoop;
        f.envelope.enabled           = true;
        f.envelope.point_count       = 2;
        f.envelope.points[0]         = EnvelopePoint{0, 64};
        f.envelope.points[1]         = EnvelopePoint{4, 64};
        f.instrument.volume_envelope = &f.envelope;
        f.instrument.fadeout_rate    = 1024;
        f.dispatch_row(f.note_on(48));
        for (int i = 0; i < 3; ++i)
            f.tick(0);
        CHECK(!f.channels[0].note_fading);
        CHECK_EQ(f.channels[0].fadeout_level, 65536u);
        for (int i = 0; i < 10; ++i)
            f.tick(0);
        CHECK(f.channels[0].note_fading);
        CHECK(!f.channels[0].key_released);
        CHECK(f.channels[0].fadeout_level < 65536u);
    }
    for (const PatternCell* cell : {&off, &fade}) {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks                = soundsinth::model::kQuirkItEnvelopeSustainLoop;
        f.envelope.enabled           = true;
        f.envelope.sustain_enabled   = true;
        f.envelope.point_count       = 2;
        f.envelope.points[0]         = EnvelopePoint{0, 64};
        f.envelope.points[1]         = EnvelopePoint{10, 64};
        f.instrument.volume_envelope = &f.envelope;
        f.instrument.fadeout_rate    = 1024;
        f.dispatch_row(f.note_on(48));
        for (int i = 0; i < 3; ++i)
            f.tick(0);
        f.dispatch_row(*cell);
        const bool is_off = cell == &off;
        CHECK_EQ(f.channels[0].key_released, is_off);
        CHECK_EQ(f.channels[0].note_fading, !is_off);
        for (int i = 0; i < 15; ++i)
            f.tick(0);
        CHECK(f.channels[0].note_fading);
        if (!is_off) CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(0)); // удержание на точке 0
    }
    {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = soundsinth::model::kQuirkItEnvelopeSustainLoop;
        f.dispatch_row(f.note_on(48));
        f.dispatch_row(off);
        for (int i = 0; i < 10; ++i)
            f.tick(0);
        CHECK(f.channels[0].voice_active);
        CHECK(!f.channels[0].stop_voice_pending);
        CHECK_EQ(f.channels[0].fadeout_level, 65536u);
    }
}

// Номер инструмента без ноты у живого голоса возвращает громкость сэмпла
// (MOD, S3M, IT - тот же инструмент), колонка громкости той же строки её
// перебивает; у XM громкость не трогается.
void test_lone_instrument_restores_volume() {
    std::printf("test_lone_instrument_restores_volume\n");
    using soundsinth::model::QuirkFlags;
    struct Case {
        QuirkFlags quirks;
        uint8_t want;
    };
    const Case cases[] = {
        {0, 64},
        {soundsinth::model::kQuirkItEffectBeforeVolColumn, 64},
        {soundsinth::model::kQuirkXmVolColumnBeforeEffect, 10},
    };
    for (const Case& c : cases) {
        Fixture f(FrequencyModel::Amiga);
        f.song.quirks = c.quirks;
        f.dispatch_row(f.note_on(48));
        f.dispatch_row(f.empty_with_effect(Effect::SetVolume, 10));
        CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(10));
        PatternCell lone;
        lone.instrument = 1;
        f.dispatch_row(lone);
        CHECK_EQ(f.channels[0].volume, c.want);
        CHECK(!f.channels[0].triggered_this_row);
        lone.volume.type  = VolumeColumnType::SetVolume;
        lone.volume.param = 20;
        f.dispatch_row(lone);
        CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(20));
    }
}

// Ретриггер IT, как у OpenMPT: нота + Q43, затем две строки Q00 при speed
// 6 - перезапуск на тиках {3}, {0, 3}, {0, 3}, громкость каждый раз -8. У MOD
// (без квирков) счёт заново с каждой строки: E93 на двух строках - {3}, {3}.
void test_retrigger_memory_and_counter() {
    std::printf("test_retrigger_memory_and_counter\n");
    for (bool it : {true, false}) {
        Fixture f(FrequencyModel::Amiga);
        if (it) f.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn;
        f.ps.speed = 6;
        std::vector<int> fired;
        const uint8_t first = it ? 0x43 : 0x03;
        const uint8_t next  = it ? 0x00 : 0x03;
        for (int row = 0; row < 3; ++row) {
            if (row == 0) {
                f.dispatch_row(f.note_on(48, Effect::Retrigger, first));
            } else {
                f.dispatch_row(f.empty_with_effect(Effect::Retrigger, next));
            }
            for (uint8_t t = 0; t < 6; ++t) {
                if (t > 0) f.tick(t);
                if (f.channels[0].retrig_pending) {
                    fired.push_back(row * 6 + t);
                    f.channels[0].retrig_pending = false;
                }
            }
        }
        const std::vector<int> want = it ? std::vector<int>{3, 6, 9, 12, 15} : std::vector<int>{3, 9, 15};
        CHECK(fired == want);
        CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(it ? 64 - 5 * 8 : 64));
    }
}

// Цель TonePorta - с finetune сэмпла: после портаменто нота стоит на той же
// высоте, что прямой триггер той же ноты (Linear и Amiga, finetune -64 и
// +112). Glissando на такой цели не уводит на соседнюю ноту: MOD-диапазон
// октав 36..71, finetune кратно 16, обе границы прижатия.
void test_tone_porta_target_with_finetune() {
    std::printf("test_tone_porta_target_with_finetune\n");
    for (FrequencyModel model : {FrequencyModel::Linear, FrequencyModel::Amiga}) {
        for (int8_t ft : {int8_t(-64), int8_t(112)}) {
            Fixture direct(model);
            direct.sample.finetune = ft;
            direct.dispatch_row(direct.note_on(50));
            Fixture porta(model);
            porta.sample.finetune = ft;
            porta.dispatch_row(porta.note_on(48));
            porta.dispatch_row(porta.note_on(50, Effect::TonePorta, 0xFF));
            if (model == FrequencyModel::Amiga) {
                CHECK_EQ(porta.channels[0].tone_porta_target, direct.channels[0].period);
            } else {
                CHECK_EQ(porta.channels[0].linear_tone_porta_target, direct.channels[0].linear_pitch);
            }
        }
    }
    uint32_t bad = 0;
    for (int ft = -128; ft <= 112; ft += 16) {
        for (uint8_t note = 36; note <= 71; ++note) {
            Fixture f(FrequencyModel::Amiga);
            f.sample.finetune = static_cast<int8_t>(ft);
            f.dispatch_row(f.note_on(note));
            for (bool nearest : {false, true}) {
                if (engine::glissando_amiga_period(f.channels[0], f.sample.finetune, nearest) != f.channels[0].period) ++bad;
            }
        }
    }
    CHECK_EQ(bad, 0u);
}

// Сетка glissando в Linear сдвинута на finetune, в том числе ниже нуля
// linear_pitch: высота ноты - на сетке; на единицу выше - ближайшая нота та
// же, следующая вверх - через полутон; на единицу ниже - та же нота.
void test_glissando_linear_grid_with_finetune() {
    std::printf("test_glissando_linear_grid_with_finetune\n");
    uint32_t negative = 0;
    for (int8_t ft : {int8_t(-64), int8_t(0), int8_t(64)}) {
        for (uint8_t note = 0; note < 96; note += 5) {
            Fixture f(FrequencyModel::Linear);
            f.sample.finetune = ft;
            f.dispatch_row(f.note_on(note));
            engine::ChannelState& cs = f.channels[0];
            const int32_t grid       = cs.linear_pitch;
            if (grid < 0) ++negative;
            CHECK_EQ(engine::glissando_linear_pitch(cs, ft, true), grid);
            CHECK_EQ(engine::glissando_linear_pitch(cs, ft, false), grid);
            cs.linear_pitch = grid + 1;
            CHECK_EQ(engine::glissando_linear_pitch(cs, ft, true), grid);
            CHECK_EQ(engine::glissando_linear_pitch(cs, ft, false), grid + engine::kLinearAmountUnitsPerSemitone);
            cs.linear_pitch = grid - 1;
            CHECK_EQ(engine::glissando_linear_pitch(cs, ft, true), grid);
            CHECK_EQ(engine::glissando_linear_pitch(cs, ft, false), grid);
        }
    }
    CHECK(negative > 0);
}

// Петля огибающей по формату, как у OpenMPT и libxmp: огибающая 0:0 2:64
// 4:0, петля по точкам 0..1 (тики 0..2). XM заворачивает на конце петли -
// по тикам 0, 32, 0, 32; IT за концом - 0, 32, 64, 0.
void test_envelope_loop_end_by_format() {
    std::printf("test_envelope_loop_end_by_format\n");
    for (bool it : {false, true}) {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks                = it ? soundsinth::model::kQuirkItEnvelopeSustainLoop : soundsinth::model::kQuirkXmVolColumnBeforeEffect;
        f.envelope.enabled           = true;
        f.envelope.loop_enabled      = true;
        f.envelope.point_count       = 3;
        f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 0};
        f.envelope.points[1]         = soundsinth::model::EnvelopePoint{2, 64};
        f.envelope.points[2]         = soundsinth::model::EnvelopePoint{4, 0};
        f.envelope.loop_start        = 0;
        f.envelope.loop_end          = 1;
        f.instrument.volume_envelope = &f.envelope;
        f.dispatch_row(f.note_on(48));
        const uint8_t want_xm[4] = {0, 32, 0, 32};
        const uint8_t want_it[4] = {0, 32, 64, 0};
        for (int t = 0; t < 4; ++t) {
            f.tick(0);
            CHECK_EQ(f.channels[0].envelope_volume, it ? want_it[t] : want_xm[t]);
        }
    }
}

// XM Rxy, как FT2 и OpenMPT: нота с инструментом + R43, затем R40 и R03 при
// speed 6 - нибблы держатся памятью, счёт переходит через строки; после
// ноты с инструментом первый перезапуск на тике 2 (счёт FT2 с 1), дальше
// каждые 3 тика: {2, 5}, {8, 11}, {14, 17}, громкость каждый раз -8.
void test_xm_retrigger_ft2() {
    std::printf("test_xm_retrigger_ft2\n");
    Fixture f(FrequencyModel::Linear);
    f.song.quirks = soundsinth::model::kQuirkXmVolColumnBeforeEffect;
    f.ps.speed    = 6;
    std::vector<int> fired;
    const uint8_t params[3] = {0x43, 0x40, 0x03};
    for (int row = 0; row < 3; ++row) {
        if (row == 0) {
            f.dispatch_row(f.note_on(48, Effect::RetriggerXm, params[0]));
        } else {
            f.dispatch_row(f.empty_with_effect(Effect::RetriggerXm, params[row]));
        }
        for (uint8_t t = 0; t < 6; ++t) {
            if (t > 0) f.tick(t);
            if (f.channels[0].retrig_pending) {
                fired.push_back(row * 6 + t);
                f.channels[0].retrig_pending = false;
            }
        }
    }
    CHECK(fired == (std::vector<int>{2, 5, 8, 11, 14, 17}));
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(64 - 6 * 8));
}

// XM E1x/E2x и X1x/X2x, как FT2 и OpenMPT: X14 - 4 единицы linear_pitch
// (вчетверо мельче E14 - 16), X10 и E10 берут свою память, 1xx/3xx её не
// видят и не портят; в модели Amiga X14 - 1 период.
void test_xm_fine_and_extra_fine_porta() {
    std::printf("test_xm_fine_and_extra_fine_porta\n");
    Fixture f(FrequencyModel::Linear);
    f.song.quirks = soundsinth::model::kQuirkXmVolColumnBeforeEffect;
    f.dispatch_row(f.note_on(48));
    const int32_t base = f.channels[0].linear_pitch;
    f.dispatch_row(f.empty_with_effect(Effect::PortaUp, 4, SlideRate::ExtraFine));
    CHECK_EQ(f.channels[0].linear_pitch, base + 4);
    f.dispatch_row(f.empty_with_effect(Effect::PortaUp, 0, SlideRate::ExtraFine));
    CHECK_EQ(f.channels[0].linear_pitch, base + 8);
    f.dispatch_row(f.empty_with_effect(Effect::PortaUp, 4, SlideRate::Fine));
    CHECK_EQ(f.channels[0].linear_pitch, base + 24);
    f.dispatch_row(f.empty_with_effect(Effect::PortaDown, 0, SlideRate::Fine)); // у "вниз" своя половина памяти - пусто
    CHECK_EQ(f.channels[0].linear_pitch, base + 24);
    CHECK_EQ(f.channels[0].porta_memory, static_cast<uint16_t>(0)); // 1xx/3xx памяти тонкие не трогают
    Fixture a(FrequencyModel::Amiga);
    a.song.quirks = soundsinth::model::kQuirkXmVolColumnBeforeEffect;
    a.dispatch_row(a.note_on(48));
    const uint16_t period = a.channels[0].period;
    a.dispatch_row(a.empty_with_effect(Effect::PortaUp, 4, SlideRate::ExtraFine));
    CHECK_EQ(a.channels[0].period, static_cast<uint16_t>(period - 1));
}

// Ноты арпеджио Amiga - с finetune сэмпла: на тиках 1 и 2 звучащий период
// равен периоду прямого триггера ноты +x и +y (finetune -64, +48 и +64;
// 0C при finetune 64 - октава, а не 0.49 полутона ниже неё).
void test_arpeggio_amiga_with_finetune() {
    std::printf("test_arpeggio_amiga_with_finetune\n");
    for (int8_t ft : {int8_t(-64), int8_t(48), int8_t(64)}) {
        for (uint8_t param : {uint8_t(0x47), uint8_t(0x0c)}) {
            Fixture f(FrequencyModel::Amiga);
            f.sample.finetune = ft;
            f.dispatch_row(f.note_on(48, Effect::Arpeggio, param));
            for (uint8_t tick = 1; tick <= 2; ++tick) {
                f.tick(tick);
                const uint8_t offset = tick == 1 ? static_cast<uint8_t>(param >> 4) : static_cast<uint8_t>(param & 0x0fu);
                Fixture direct(FrequencyModel::Amiga);
                direct.sample.finetune = ft;
                direct.dispatch_row(direct.note_on(static_cast<uint8_t>(48 + offset)));
                CHECK_EQ(int32_t(f.channels[0].period) + f.channels[0].pitch_offset, int32_t(direct.channels[0].period));
            }
        }
    }
}

// Arpeggio (Effect::Arpeggio), Amiga-модель: период циклически
// переключается по табличным note, note+x, note+y по tick_in_row%3 (см.
// apply_arpeggio_tick_amiga в .cpp).
void test_arpeggio_cycles_period_amiga() {
    std::printf("test_arpeggio_cycles_period_amiga\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Arpeggio, 0x37)); // x=3, y=7
    CHECK(f.channels[0].arpeggio_active);
    CHECK_EQ(f.channels[0].arpeggio_x, static_cast<uint8_t>(3));
    CHECK_EQ(f.channels[0].arpeggio_y, static_cast<uint8_t>(7));

    const uint16_t base_period = f.channels[0].period;                        // note=48
    const uint16_t period_x    = soundsinth::model::amiga_note_to_period(51); // 48+3
    const uint16_t period_y    = soundsinth::model::amiga_note_to_period(55); // 48+7

    f.tick(0); // phase 0 -> база
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(0));
    f.tick(1); // phase 1 -> +x
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(period_x) - static_cast<int32_t>(base_period));
    f.tick(2); // phase 2 -> +y
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(period_y) - static_cast<int32_t>(base_period));
    CHECK_EQ(f.channels[0].period, base_period); // period сам не тронут
}

// Arpeggio под Linear-моделью - простая арифметика (offset*64), см.
// apply_arpeggio_tick_linear в .cpp.
void test_arpeggio_cycles_linear_pitch_offset_linear_model() {
    std::printf("test_arpeggio_cycles_linear_pitch_offset_linear_model\n");

    Fixture f(FrequencyModel::Linear);
    f.dispatch_row(f.note_on(48, Effect::Arpeggio, 0x37)); // x=3, y=7
    f.tick(0);
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(0));
    f.tick(1);
    CHECK_EQ(f.channels[0].pitch_offset, 3 * static_cast<int32_t>(engine::kLinearAmountUnitsPerSemitone));
    f.tick(2);
    CHECK_EQ(f.channels[0].pitch_offset, 7 * static_cast<int32_t>(engine::kLinearAmountUnitsPerSemitone));
}

// SetChannelVolume/ChannelVolumeSlide (IT Mxx/Nxy): независимый множитель
// поверх cs.volume (см. .h). cs.channel_volume по умолчанию 64
// (нейтрально), Note-Trigger его не трогает (в отличие от cs.volume).
void test_set_channel_volume_and_slide() {
    std::printf("test_set_channel_volume_and_slide\n");

    Fixture f(FrequencyModel::Amiga);
    CHECK_EQ(f.channels[0].channel_volume, static_cast<uint8_t>(64)); // значение по умолчанию до первой Mxx

    f.dispatch_row(f.note_on(48, Effect::SetChannelVolume, 40));
    CHECK_EQ(f.channels[0].channel_volume, static_cast<uint8_t>(40));

    f.dispatch_row(f.empty_with_effect(Effect::ChannelVolumeSlide, 0x05, SlideRate::PerTick)); // down=5
    CHECK(f.channels[0].channel_volume_slide_active);
    f.tick(1);
    CHECK_EQ(f.channels[0].channel_volume, static_cast<uint8_t>(35));
}

// SetGlobalVolume/GlobalVolumeSlide - единственные эффекты уровня песни,
// а не канала: живут в PlayState (см. .h), а не в ChannelState. Шкала
// 0..128 (не 0..64, как у channel_volume/volume); тест задаёт param=100
// (>64), чтобы исключить случайное совпадение с путём 0..64. Шаг слайда у
// IT - как есть, у XM и S3M (шкала файла 0..64) - вдвое, и H8x..HFx не
// упираются в 15.
void test_set_global_volume_and_slide() {
    std::printf("test_set_global_volume_and_slide\n");
    {
        Fixture xm(FrequencyModel::Linear);
        xm.song.quirks = soundsinth::model::kQuirkXmVolColumnBeforeEffect;
        xm.dispatch_row(xm.note_on(48, Effect::SetGlobalVolume, 20));
        xm.dispatch_row(xm.empty_with_effect(Effect::GlobalVolumeSlide, 0xA0)); // HA0 - вверх на 10 из 64
        xm.tick(1);
        CHECK_EQ(xm.ps.global_volume, static_cast<uint8_t>(40));
        xm.dispatch_row(xm.empty_with_effect(Effect::GlobalVolumeSlide, 0x35)); // оба нибла - вверх
        xm.tick(1);
        CHECK_EQ(xm.ps.global_volume, static_cast<uint8_t>(46));
    }

    Fixture f(FrequencyModel::Amiga);
    f.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn;
    CHECK_EQ(f.ps.global_volume, static_cast<uint8_t>(128)); // значение PlayState по умолчанию

    f.dispatch_row(f.note_on(48, Effect::SetGlobalVolume, 100));
    CHECK_EQ(f.ps.global_volume, static_cast<uint8_t>(100)); // >64 - подтверждает шкалу 0..128, а не 0..64

    f.dispatch_row(f.empty_with_effect(Effect::GlobalVolumeSlide, 0x0A, SlideRate::PerTick)); // down=10
    CHECK(f.ps.global_volume_slide_active);
    f.tick(1);
    CHECK_EQ(f.ps.global_volume, static_cast<uint8_t>(90));

    // Новая строка без GlobalVolumeSlide - слайд должен погаснуть
    // (сбрасывается раз за строку, см. .cpp), а не продолжаться бесконечно.
    f.dispatch_row(f.empty_with_effect(Effect::None, 0));
    CHECK(!f.ps.global_volume_slide_active);
    f.tick(1);
    CHECK_EQ(f.ps.global_volume, static_cast<uint8_t>(90)); // не сдвинулся дальше
}

// Баг из 00009.it (канал 9, паттерн 50): SetChannelVolume,
// ChannelVolumeSlide, SetGlobalVolume и GlobalVolumeSlide обрабатывались в
// том же switch, после проверки cs.voice_active. На строке без ноты (голос
// на канале ещё не звучал, voice_active false с создания Fixture) команда
// молча отбрасывалась. Это состояние уровня канала или песни, а не
// свойство ноты, и оно должно применяться независимо от того, звучит ли
// что-то на канале (авторы часто выставляют громкость канала заранее, за
// много строк до первой ноты). Тесты test_set_channel_volume_and_slide и
// test_set_global_volume_and_slide выше этот баг не ловили: там команда
// идёт вместе с note_on, и Note-Trigger взводит voice_active=true до
// switch.
void test_channel_and_global_volume_apply_without_active_voice() {
    std::printf("test_channel_and_global_volume_apply_without_active_voice\n");

    Fixture f(FrequencyModel::Amiga);
    CHECK(!f.channels[0].voice_active); // ни одной ноты ещё не было

    f.dispatch_row(f.empty_with_effect(Effect::SetChannelVolume, 34));
    CHECK_EQ(f.channels[0].channel_volume, static_cast<uint8_t>(34));

    f.dispatch_row(f.empty_with_effect(Effect::SetGlobalVolume, 100));
    CHECK_EQ(f.ps.global_volume, static_cast<uint8_t>(100));

    CHECK(!f.channels[0].voice_active); // всё ещё нет голоса - команды не триггерят ноту сами по себе

    // Нота триггерится позже, много строк спустя: channel_volume должен
    // сохраниться (Note-Trigger его не сбрасывает, см. .h) и применяться к
    // громкости при сведении (сверено отдельно, здесь проверяется только,
    // что значение дошло и не потерялось).
    f.dispatch_row(f.note_on(48));
    CHECK(f.channels[0].voice_active);
    CHECK_EQ(f.channels[0].channel_volume, static_cast<uint8_t>(34));
}

// Volume-колонка (PatternCell::volume) раньше не читалась. SetVolume и
// SlideUp - самые частые её команды в реальных файлах (00009.it:
// SetVolume 11889 раз).
void test_volume_column_set_volume_and_slide_up() {
    std::printf("test_volume_column_set_volume_and_slide_up\n");

    Fixture f(FrequencyModel::Amiga);
    PatternCell cell  = f.note_on(48);
    cell.volume.type  = VolumeColumnType::SetVolume;
    cell.volume.param = 50;
    f.dispatch_row(cell);
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(50)); // не default_volume(64) сэмпла - переопределено volume-колонкой

    f.dispatch_row(f.empty_with_volcol(VolumeColumnType::SlideUp, 5));
    CHECK(f.channels[0].volume_slide_active);
    f.tick(1);
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(55));
}

void test_volume_column_set_panning_is_already_0_64_scale() {
    std::printf("test_volume_column_set_panning_is_already_0_64_scale\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48));
    f.dispatch_row(f.empty_with_volcol(VolumeColumnType::SetPanning, 60));
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(60)); // без /4, в отличие от эффект-колонки SetPanning
}

// S91 и панорама колонки громкости в одной ячейке: S9x идёт до колонки при
// любом порядке колонок, колонка гасит surround и ставит свою панораму - с
// голосом и без.
void test_surround_then_volume_column_panning() {
    std::printf("test_surround_then_volume_column_panning\n");
    for (bool it : {false, true}) {
        for (bool voice : {false, true}) {
            Fixture f(FrequencyModel::Linear);
            if (it) f.song.quirks |= soundsinth::model::kQuirkItEffectBeforeVolColumn;
            if (voice) f.dispatch_row(f.note_on(48));
            PatternCell cell  = f.empty_with_volcol(VolumeColumnType::SetPanning, 10);
            cell.effect.type  = Effect::SoundControl;
            cell.effect.param = 0x01;
            f.dispatch_row(cell);
            CHECK(!f.channels[0].surround);
            CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(10));
        }
    }
}

// Volume-колонка PortamentoUp: vol<<2 и дальше тот же путь PortaUp, что
// у эффект-колонки (сверено с OpenMPT soundlib/Snd_fx.cpp
// "PortamentoUp(nChn, vol<<2, ...)"); знак зависит от модели, как у
// Effect::PortaUp (см. .cpp).
void test_volume_column_portamento_up_amiga_and_linear_signs() {
    std::printf("test_volume_column_portamento_up_amiga_and_linear_signs\n");

    Fixture amiga(FrequencyModel::Amiga);
    amiga.dispatch_row(amiga.note_on(48));
    amiga.dispatch_row(amiga.empty_with_volcol(VolumeColumnType::PortamentoUp, 4)); // 4<<2=16
    CHECK_EQ(amiga.channels[0].porta_memory, static_cast<uint16_t>(16));
    CHECK(amiga.channels[0].porta_active);
    const uint16_t period_before = amiga.channels[0].period;
    amiga.tick(1);
    CHECK_EQ(amiga.channels[0].period, static_cast<uint16_t>(period_before - 16)); // Amiga: PortaUp уменьшает период

    Fixture linear(FrequencyModel::Linear);
    linear.dispatch_row(linear.note_on(48));
    linear.dispatch_row(linear.empty_with_volcol(VolumeColumnType::PortamentoUp, 4)); // 4<<2=16, *4 под Linear = 64
    CHECK_EQ(linear.channels[0].porta_memory, static_cast<uint16_t>(64));
    const int32_t linear_pitch_before = linear.channels[0].linear_pitch;
    linear.tick(1);
    CHECK_EQ(linear.channels[0].linear_pitch, linear_pitch_before + 64); // Linear: PortaUp увеличивает linear_pitch
}

// VolumeColumnType::TonePorta в XM-масштабе (без
// kQuirkItVolColumnPortaTable): param*16 напрямую (см. .cpp и не-IT ветку OpenMPT
// GetVolCmdTonePorta). Подавление ретриггера то же, что у эффект-колонки
// TonePorta (is_tone_porta_family включает VolumeColumnType::TonePorta,
// см. .cpp).
void test_volume_column_tone_porta_xm_scale_and_suppresses_retrigger() {
    std::printf("test_volume_column_tone_porta_xm_scale_and_suppresses_retrigger\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48));     // обычный триггер, period=428
    PatternCell cell  = f.note_on(60); // новая нота + volcol-toneporta - ретриггер должен быть подавлен
    cell.volume.type  = VolumeColumnType::TonePorta;
    cell.volume.param = 4; // 4*16=64
    f.dispatch_row(cell);
    CHECK_EQ(f.channels[0].period, static_cast<uint16_t>(428)); // не ретриггернуто - период на этой строке не изменился
    CHECK(f.channels[0].tone_porta_active);
    CHECK_EQ(f.channels[0].porta_memory, static_cast<uint16_t>(64));
    CHECK(f.channels[0].tone_porta_target != f.channels[0].period); // цель обновлена на новую ноту (60), выше текущей (48)
}

// IT-масштаб: та же команда, но через таблицу из 10 значений
// (kItPortaVolCmdTable), а не линейно; сверено с OpenMPT
// soundlib/Tables.cpp ImpulseTrackerPortaVolCmd.
void test_volume_column_tone_porta_it_scale_uses_lookup_table() {
    std::printf("test_volume_column_tone_porta_it_scale_uses_lookup_table\n");

    Fixture f(FrequencyModel::Amiga);
    f.song.quirks |= soundsinth::model::kQuirkItEffectBeforeVolColumn | soundsinth::model::kQuirkItVolColumnPortaTable;
    f.dispatch_row(f.note_on(48));
    PatternCell cell  = f.note_on(60);
    cell.volume.type  = VolumeColumnType::TonePorta;
    cell.volume.param = 4; // kItPortaVolCmdTable[4]==16, а не 4*16=64, как у XM
    f.dispatch_row(cell);
    CHECK_EQ(f.channels[0].porta_memory, static_cast<uint16_t>(16));
}

// VolumeColumnType::Offset (только IT, см. formats/it.cpp vol 223..232):
// param*2048 сэмплов (те же "cue-точки по умолчанию", что в OpenMPT без
// пользовательских cue; формула и сверка в .cpp).
void test_volume_column_offset_scales_by_2048() {
    std::printf("test_volume_column_offset_scales_by_2048\n");

    Fixture f(FrequencyModel::Amiga);
    f.sample.length_samples = 100000; // достаточно длинный, чтобы 3*2048 не вышло за предел
    PatternCell cell        = f.note_on(48);
    cell.volume.type        = VolumeColumnType::Offset;
    cell.volume.param       = 3;
    f.dispatch_row(cell);
    CHECK_EQ(f.channels[0].trigger_sample_offset, static_cast<uint32_t>(3 * 2048));
}

// VolumeColumnType::VibratoSpeed (только XM): param напрямую как скорость,
// глубина не трогается, и вибрато не включается - как FT2
// (kFT2VolColVibrato в OpenMPT). Качает его следующая Vx или 4xy.
void test_volume_column_vibrato_speed_sets_speed_keeps_depth_memory() {
    std::printf("test_volume_column_vibrato_speed_sets_speed_keeps_depth_memory\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Vibrato, 0x28)); // speed=2, depth=8(<<2=32) - заводим память
    f.dispatch_row(f.empty_with_volcol(VolumeColumnType::VibratoSpeed, 7));
    CHECK_EQ(f.channels[0].vibrato_speed, static_cast<uint8_t>(7));
    CHECK_EQ(f.channels[0].vibrato_depth, static_cast<uint8_t>(32)); // не тронута
    CHECK(!f.channels[0].vibrato_active);
    f.tick(1);
    CHECK_EQ(f.channels[0].pitch_offset, 0);
}

// Tremolo - та же синусная таблица и механика, что у Vibrato, но на
// громкости (сам cs.volume не трогается, см. .h), depth без множителя <<2
// и делитель 64 вместо 512 (см. .cpp). table[4]==97 - то же публичное
// значение таблицы, что в Vibrato-тесте. Фаза на тике 0 стоит, как у
// вибрато.
void test_tremolo_phase_holds_on_tick_zero_and_does_not_touch_volume() {
    std::printf("test_tremolo_phase_holds_on_tick_zero_and_does_not_touch_volume\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Tremolo, 0x44)); // speed=4, depth=4 (без <<2)
    CHECK_EQ(f.channels[0].tremolo_phase, static_cast<uint8_t>(0));
    CHECK(f.channels[0].tremolo_active);

    f.tick(0);
    CHECK_EQ(f.channels[0].volume_offset, static_cast<int16_t>(0)); // table[0]==0
    CHECK_EQ(f.channels[0].tremolo_phase, static_cast<uint8_t>(0));
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(64)); // сам volume не тронут

    f.tick(1);
    CHECK_EQ(f.channels[0].tremolo_phase, static_cast<uint8_t>(4));
    f.tick(2); // table[4]==97: offset = 97*4/64 = 6 (целочисленно)
    CHECK_EQ(f.channels[0].volume_offset, static_cast<int16_t>(6));
}

void test_tremolo_offset_clears_when_not_respecified_on_later_row() {
    std::printf("test_tremolo_offset_clears_when_not_respecified_on_later_row\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Tremolo, 0x44));
    f.tick(1);
    f.tick(2);
    CHECK(f.channels[0].volume_offset != 0);

    f.dispatch_row(f.empty_with_effect(Effect::None, 0));
    CHECK(!f.channels[0].tremolo_active);
    f.tick(0);
    CHECK_EQ(f.channels[0].volume_offset, static_cast<int16_t>(0));
}

// Tremor - простое вкл/выкл по счётчику тиков, от frequency_model не
// зависит (громкость общая для обеих моделей питча, в отличие от
// Porta/TonePorta/Vibrato). on=2, off=1 -> ожидаемая последовательность
// muted: [нет,нет,да, нет,нет,да, ...], см. apply_tremor_tick в .cpp.
void test_tremor_alternates_muted_by_on_off_tick_counts() {
    std::printf("test_tremor_alternates_muted_by_on_off_tick_counts\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Tremor, 0x21)); // on=2, off=1
    CHECK(f.channels[0].tremor_active);

    const bool expected_muted[6] = {false, false, true, false, false, true};
    for (int i = 0; i < 6; ++i) {
        f.tick(0);
        CHECK_EQ(f.channels[0].tremor_muted, expected_muted[i]);
    }
}

// Память Tremor - целый байт (param!=0 перезаписывает оба нибла сразу), а
// не по нибблам независимо, как у Vibrato/Tremolo; см. apply_tremor_tick
// и dispatch_row_effects в .cpp.
void test_tremor_memory_reuses_whole_byte_not_per_nibble() {
    std::printf("test_tremor_memory_reuses_whole_byte_not_per_nibble\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Tremor, 0x50)); // on=5, off=0->1
    CHECK_EQ(f.channels[0].tremor_on_ticks, static_cast<uint8_t>(5));
    CHECK_EQ(f.channels[0].tremor_off_ticks, static_cast<uint8_t>(1));

    // param=0x03 (on=0->1, off=3) ненулевой и перезаписывает оба нибла; при
    // памяти по нибблам (как у Vibrato) on остался бы 5.
    f.dispatch_row(f.empty_with_effect(Effect::Tremor, 0x03));
    CHECK_EQ(f.channels[0].tremor_on_ticks, static_cast<uint8_t>(1));
    CHECK_EQ(f.channels[0].tremor_off_ticks, static_cast<uint8_t>(3));

    // param=0x00 повторяет весь предыдущий байт (0x03).
    f.dispatch_row(f.empty_with_effect(Effect::Tremor, 0x00));
    CHECK_EQ(f.channels[0].tremor_on_ticks, static_cast<uint8_t>(1));
    CHECK_EQ(f.channels[0].tremor_off_ticks, static_cast<uint8_t>(3));
}

void test_tremor_works_under_linear_frequency_model_too() {
    std::printf("test_tremor_works_under_linear_frequency_model_too\n");

    // В отличие от питч-эффектов, Tremor/Tremolo не гейтятся по
    // frequency_model - громкость одна и та же в обеих моделях.
    Fixture f(FrequencyModel::Linear);
    f.dispatch_row(f.note_on(48, Effect::Tremor, 0x11)); // on=1, off=1
    CHECK(f.channels[0].tremor_active);
    f.tick(0);
    CHECK(!f.channels[0].tremor_muted);
    f.tick(0);
    CHECK(f.channels[0].tremor_muted);
}

// Панорама (SetPanning/PanningSlide): общая шкала 0..64 (32=центр, см.
// sequencer.h), от frequency_model не зависит, как Tremolo/Tremor.
void test_set_panning_scales_0_255_to_0_64() {
    std::printf("test_set_panning_scales_0_255_to_0_64\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::SetPanning, 0)); // 0/4=0 - крайний левый
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(0));

    f.dispatch_row(f.empty_with_effect(Effect::SetPanning, 255)); // 255/4=63 (целочисленно) - почти крайний правый
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(63));

    f.dispatch_row(f.empty_with_effect(Effect::SetPanning, 128)); // 128/4=32 - центр
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(32));
}

// Panbrello (S3M/IT Yxy): та же механика, что у Vibrato/Tremolo, но
// depth<<4 (а не <<2 или без сдвига) и делитель 2048. Наша шкала
// панорамы 0..64 в 4 раза уже libxmp-овской 0..255, и делитель в 4 раза
// больше 512 даёт тот же относительный размах; см. apply_panbrello_tick в
// .h.
void test_panbrello_applies_even_on_tick_in_row_zero_and_does_not_touch_pan() {
    std::printf("test_panbrello_applies_even_on_tick_in_row_zero_and_does_not_touch_pan\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Panbrello, 0x44)); // speed=4, depth=4(<<4=64)
    CHECK(f.channels[0].panbrello_active);
    CHECK_EQ(f.channels[0].panbrello_depth, static_cast<uint8_t>(64));

    f.tick(0);
    CHECK_EQ(f.channels[0].pan_offset, static_cast<int16_t>(0)); // table[0]==0
    CHECK_EQ(f.channels[0].panbrello_phase, static_cast<uint8_t>(4));
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(32)); // сам pan не тронут

    f.tick(0); // table[4]==97: offset = 97*64/2048 = 3 (целочисленно)
    CHECK_EQ(f.channels[0].pan_offset, static_cast<int16_t>(3));

    // Строка без Panbrello - offset сбрасывается, фаза не сбрасывается.
    f.dispatch_row(f.empty_with_effect(Effect::None, 0));
    CHECK(!f.channels[0].panbrello_active);
    f.tick(0);
    CHECK_EQ(f.channels[0].pan_offset, static_cast<int16_t>(0));
    CHECK(f.channels[0].panbrello_phase != 0);
}

void test_panning_slide_per_tick_and_memory() {
    std::printf("test_panning_slide_per_tick_and_memory\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::SetPanning, 128)); // старт с центра (32)
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(32));

    f.dispatch_row(f.empty_with_effect(Effect::PanningSlide, 0x05)); // вниз(влево) на 5/тик
    CHECK(f.channels[0].pan_slide_active);
    f.tick(1);
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(27));

    // "00" на новой строке повторяет запомненные 5.
    f.dispatch_row(f.empty_with_effect(Effect::PanningSlide, 0x00));
    f.tick(1);
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(22));
}

// SD0: у IT - как SD1 (нота на тике 1), у S3M ячейка не играет вовсе.
void test_note_delay_zero_it_s3m() {
    std::printf("test_note_delay_zero_it_s3m\n");
    Fixture it(FrequencyModel::Amiga);
    it.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn;
    it.dispatch_row(it.note_on(48, Effect::NoteDelay, 0));
    CHECK(!it.channels[0].voice_active);
    CHECK_EQ(it.channels[0].delayed_cell_tick, static_cast<uint16_t>(1));
    it.dispatch_delayed(1);
    CHECK(it.channels[0].voice_active);
    Fixture s3m(FrequencyModel::Amiga);
    s3m.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkS3mVolSlideDownPriority;
    s3m.dispatch_row(s3m.note_on(48, Effect::NoteDelay, 0));
    CHECK(!s3m.channels[0].voice_active);
    CHECK_EQ(s3m.channels[0].delayed_cell_tick, static_cast<uint16_t>(0));
}

// J00 у IT и S3M повторяет прошлое арпеджио: J37, затем J00 - на тике 1
// смещение 3 полутона.
void test_arpeggio_memory_it_s3m() {
    std::printf("test_arpeggio_memory_it_s3m\n");
    Fixture f(FrequencyModel::Linear);
    f.song.quirks = soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn;
    f.dispatch_row(f.note_on(48, Effect::Arpeggio, 0x37));
    f.dispatch_row(f.empty_with_effect(Effect::Arpeggio, 0x00));
    f.tick(1);
    CHECK_EQ(f.channels[0].pitch_offset, static_cast<int32_t>(3 * 64));
}

// Слайд с обоими ненулевыми ниблами: IT игнорирует (D99 - громкость 32
// остаётся), S3M скользит вниз (D35 - 27 после тика 1), MOD/XM вверх (A35 -
// 35).
void test_volume_slide_both_nibbles_by_format() {
    std::printf("test_volume_slide_both_nibbles_by_format\n");
    using soundsinth::model::QuirkFlags;
    struct Case {
        QuirkFlags quirks;
        uint8_t param;
        uint8_t want;
    };
    const Case cases[] = {
        {soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkItEffectBeforeVolColumn, 0x99, 32},
        {soundsinth::model::kQuirkFineSlideInParam | soundsinth::model::kQuirkS3mVolSlideDownPriority, 0x35, 27},
        {0, 0x35, 35},
    };
    for (const Case& c : cases) {
        Fixture f(FrequencyModel::Amiga);
        f.song.quirks = c.quirks;
        f.dispatch_row(f.note_on(48, Effect::SetVolume, 32));
        f.dispatch_row(f.empty_with_effect(Effect::VolumeSlide, c.param));
        f.tick(1);
        CHECK_EQ(f.channels[0].volume, c.want);
    }
}

// Направление Pxy по формату, как у OpenMPT: у IT и S3M P0x - вправо, Px0 -
// влево, PFx - тонко вправо на тике 0; оба ненулевых нибла - у IT слайда нет,
// у S3M вправо. У XM (без квирков) - наоборот, тест выше.
void test_panning_slide_direction_it_s3m() {
    std::printf("test_panning_slide_direction_it_s3m\n");
    using soundsinth::model::kQuirkFineSlideInParam;
    using soundsinth::model::kQuirkItEffectBeforeVolColumn;
    using soundsinth::model::kQuirkS3mVolSlideDownPriority;
    using soundsinth::model::QuirkFlags;
    const QuirkFlags it  = kQuirkFineSlideInParam | kQuirkItEffectBeforeVolColumn;
    const QuirkFlags s3m = kQuirkFineSlideInParam | kQuirkS3mVolSlideDownPriority;
    struct Case {
        QuirkFlags quirks;
        uint8_t param;
        uint8_t tick0, tick1, tick2;
    };
    const Case cases[] = {
        {it, 0x05, 32, 37, 42}, {it, 0x50, 32, 27, 22},  {it, 0xF2, 34, 34, 34},  {it, 0x2F, 30, 30, 30},
        {it, 0x55, 32, 32, 32}, {s3m, 0x50, 32, 27, 22}, {s3m, 0x55, 32, 37, 42},
    };
    for (const Case& c : cases) {
        Fixture f(FrequencyModel::Linear);
        f.song.quirks = c.quirks;
        f.dispatch_row(f.note_on(48, Effect::SetPanning, 128));
        f.dispatch_row(f.empty_with_effect(Effect::PanningSlide, c.param));
        CHECK_EQ(f.channels[0].pan, c.tick0);
        f.tick(1);
        CHECK_EQ(f.channels[0].pan, c.tick1);
        f.tick(2);
        CHECK_EQ(f.channels[0].pan, c.tick2);
    }
}

// SampleDescriptor::default_panning: -1 - сэмпл наследует то, что уже было
// на канале (не сбрасывает в центр); значение 0..64 применяется на каждый
// Note-Trigger этим сэмплом.
void test_sample_default_panning_minus1_inherits_else_applied_on_trigger() {
    std::printf("test_sample_default_panning_minus1_inherits_else_applied_on_trigger\n");

    Fixture f(FrequencyModel::Amiga);
    f.channels[0].pan = 10;                                      // как будто на канале раньше уже что-то стояло
    CHECK_EQ(f.sample.default_panning, static_cast<int8_t>(-1)); // в Fixture по умолчанию - "нет своей панорамы"

    f.dispatch_row(f.note_on(48));                         // без SetPanning на строке
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(10)); // унаследовал, не сброшен в центр

    f.sample.default_panning = 60;
    f.dispatch_row(f.note_on(50)); // новый триггер тем же (единственным) сэмплом, но теперь с default_panning
    CHECK_EQ(f.channels[0].pan, static_cast<uint8_t>(60));
}

// SampleOffset действует только вместе с Note-Trigger (это проверяет
// TrackerEngine через cs.triggered_this_row); здесь на уровне
// dispatch_row_effects проверяются cs.trigger_sample_offset и память.
void test_sample_offset_scales_by_256_and_remembers_last_nonzero() {
    std::printf("test_sample_offset_scales_by_256_and_remembers_last_nonzero\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::SampleOffset, 3)); // 3*256
    CHECK_EQ(f.channels[0].trigger_sample_offset, static_cast<uint32_t>(3 * 256));

    f.dispatch_row(f.note_on(48, Effect::SampleOffset, 0)); // "00" повторяет запомненные 3
    CHECK_EQ(f.channels[0].trigger_sample_offset, static_cast<uint32_t>(3 * 256));

    // Строка без SampleOffset - trigger_sample_offset сбрасывается в 0
    // (не переносит значение с давней строки на TrackerEngine).
    f.dispatch_row(f.note_on(48));
    CHECK_EQ(f.channels[0].trigger_sample_offset, static_cast<uint32_t>(0));
}

// Retrigger: интервал и тип из ниблов параметра (MOD E9x даёт param уже
// 0..15, верхний нибл получается 0 - это верное поведение E9x, см. .cpp),
// модификатор громкости по таблице ProTracker/S3M/IT/XM (сверено с libxmp
// src/player.c::rval[]).
void test_retrigger_fires_at_interval_and_applies_volume_table() {
    std::printf("test_retrigger_fires_at_interval_and_applies_volume_table\n");

    Fixture f(FrequencyModel::Amiga);
    // type=1 (rval[1]={-1,1,1} - вычесть 1), interval=3.
    f.dispatch_row(f.note_on(48, Effect::Retrigger, 0x13));
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(64)); // громкость сэмпла, тик 0 ретриггер не трогает

    f.tick(1);
    CHECK(!f.channels[0].retrig_pending);
    f.tick(2);
    CHECK(!f.channels[0].retrig_pending);
    f.tick(3); // 3-й тик подряд после строки - интервал достигнут
    CHECK(f.channels[0].retrig_pending);
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(63)); // 64 + rval[1].add(-1)

    // TrackerEngine потребляет и гасит retrig_pending - здесь это делается
    // вручную (TrackerEngine на этом уровне не тестируется).
    f.channels[0].retrig_pending = false;
    f.tick(1);
    f.tick(2);
    f.tick(3); // ещё один полный интервал спустя после сброса
    CHECK(f.channels[0].retrig_pending);
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(62));
}

void test_retrigger_mod_e9x_style_param_has_no_volume_type() {
    std::printf("test_retrigger_mod_e9x_style_param_has_no_volume_type\n");

    // MOD E9x отдаёт param уже 0..15 (только младший нибл, см.
    // formats/mod.cpp) - верхний нибл (тип модификатора громкости)
    // получается 0 сам собой ("без изменений", rval[0]={0,1,1}).
    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::Retrigger, 2)); // param=2 (0x02) - interval=2, type=0
    CHECK_EQ(f.channels[0].retrig_type, static_cast<uint8_t>(0));
    CHECK_EQ(f.channels[0].retrig_interval, static_cast<uint8_t>(2));

    f.tick(1);
    f.tick(2);
    CHECK(f.channels[0].retrig_pending);
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(64)); // type=0 - громкость не меняется
}

void test_keyoff_stops_voice_without_envelope() {
    std::printf("test_keyoff_stops_voice_without_envelope\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48));
    CHECK(f.channels[0].voice_active);

    f.dispatch_row(f.empty_with_effect(Effect::KeyOff, 0));
    CHECK(!f.channels[0].voice_active);
    CHECK(f.channels[0].stop_voice_pending);
}

// KeyOff с volume envelope - не мгновенная остановка (в отличие от теста
// выше, без envelope): voice_active остаётся true, огибающая продолжает
// движение за sustain-точку (release-фаза), см. effect_dispatch.cpp.
// fadeout_rate=0 (по умолчанию в Fixture) изолирует release огибающей от
// fadeout (для него отдельный тест ниже).
void test_keyoff_with_envelope_enters_release_instead_of_stopping() {
    std::printf("test_keyoff_with_envelope_enters_release_instead_of_stopping\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled           = true;
    f.envelope.sustain_enabled   = true;
    f.envelope.point_count       = 3;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.envelope.points[1]         = soundsinth::model::EnvelopePoint{4, 32}; // sustain здесь
    f.envelope.points[2]         = soundsinth::model::EnvelopePoint{8, 0};  // release-хвост, после sustain
    f.envelope.sustain_point     = 1;
    f.envelope.sustain_end       = 1;
    f.instrument.volume_envelope = &f.envelope;

    f.dispatch_row(f.note_on(48));
    for (int i = 0; i < 10; ++i)
        f.tick(0); // держится на sustain (тик 4), как в тесте без KeyOff
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(4));
    CHECK(f.channels[0].voice_active);

    f.dispatch_row(f.empty_with_effect(Effect::KeyOff, 0));
    CHECK(f.channels[0].voice_active); // не остановлен - есть envelope
    CHECK(f.channels[0].key_released);
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(4)); // тик 0 KeyOff-строки ещё не сдвинул

    for (int i = 0; i < 10; ++i)
        f.tick(0); // заведомо больше, чем нужно, чтобы дойти до точки 8
    // envelope_tick продолжает расти и после последней точки
    // (evaluate_envelope держит значение последней точки для любого tick за
    // её пределами, см. .cpp). Важно не точное значение тика, а то, что он
    // больше не держится на sustain (4), и volume дошёл до значения последней
    // точки (0).
    CHECK(f.channels[0].envelope_tick >= static_cast<uint16_t>(8));
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(0));
    CHECK(f.channels[0].voice_active); // fadeout_rate=0 - сам по себе не останавливает

    // Позиция насыщается на 0xFFFF, а не заворачивает в 0 (там снова 64).
    f.channels[0].envelope_tick = 0xFFFE;
    for (int i = 0; i < 3; ++i)
        f.tick(1);
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(0xFFFF));
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(0));
}

// Carry (IT): позиция огибающей новой нотой не сбрасывается; без carry -
// с нуля.
void test_envelope_carry_keeps_position_on_new_note() {
    std::printf("test_envelope_carry_keeps_position_on_new_note\n");
    for (const bool carry : {true, false}) {
        Fixture f(FrequencyModel::Amiga);
        f.envelope.enabled           = true;
        f.envelope.carry             = carry;
        f.envelope.point_count       = 2;
        f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
        f.envelope.points[1]         = soundsinth::model::EnvelopePoint{20, 0};
        f.instrument.volume_envelope = &f.envelope;
        f.dispatch_row(f.note_on(48));
        for (uint8_t t = 0; t < 5; ++t)
            f.tick(t);
        CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(5));
        f.dispatch_row(f.note_on(50));
        CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(carry ? 5 : 0));
    }
}

// Петля удержания IT (kQuirkItEnvelopeSustainLoop): пока нота не отпущена,
// позиция крутится sustain_point..sustain_end включительно, обычная петля
// (здесь 0..3, короче удержания) не действует. Раньше позиция вставала на
// sustain_end. После KeyOff - дальше, в релиз.
void test_it_envelope_sustain_loop() {
    std::printf("test_it_envelope_sustain_loop\n");

    Fixture f(FrequencyModel::Linear);
    f.song.quirks                |= soundsinth::model::kQuirkItEnvelopeSustainLoop;
    f.envelope.enabled            = true;
    f.envelope.sustain_enabled    = true;
    f.envelope.loop_enabled       = true;
    f.envelope.point_count        = 4;
    f.envelope.points[0]          = soundsinth::model::EnvelopePoint{0, 64};
    f.envelope.points[1]          = soundsinth::model::EnvelopePoint{2, 32}; // начало удержания
    f.envelope.points[2]          = soundsinth::model::EnvelopePoint{4, 48}; // конец удержания
    f.envelope.points[3]          = soundsinth::model::EnvelopePoint{6, 0};
    f.envelope.sustain_point      = 1;
    f.envelope.sustain_end        = 2;
    f.envelope.loop_start         = 0;
    f.envelope.loop_end           = 1;
    f.instrument.volume_envelope  = &f.envelope;

    f.dispatch_row(f.note_on(48));
    const uint16_t expected[] = {1, 2, 3, 4, 2, 3, 4, 2};
    for (uint16_t t : expected) {
        f.tick(0);
        CHECK_EQ(f.channels[0].envelope_tick, t);
    }
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(48)); // прочитана точка 4 перед заворотом

    f.dispatch_row(f.empty_with_effect(Effect::KeyOff, 0));
    f.tick(0); // отпущена: удержание снято, работает обычная петля 0..1
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(0));
}

// .mid: огибающие и затухание в тиках по стартовому темпу; при смене темпа
// за тик движка проходится default_tempo / темп тиков огибающих (Q8.8):
// 512 - два, 128 - один через тик, дробь копится.
void test_envelope_time_step() {
    std::printf("test_envelope_time_step\n");

    Fixture f(FrequencyModel::Linear);
    f.envelope.enabled           = true;
    f.envelope.point_count       = 2;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.envelope.points[1]         = soundsinth::model::EnvelopePoint{100, 0};
    f.instrument.volume_envelope = &f.envelope;
    f.instrument.fadeout_rate    = 1000;
    f.dispatch_row(f.note_on(48));

    engine::ChannelState& cs = f.channels[0];
    engine::advance_envelope_and_fadeout(cs, false, 0, 512);
    CHECK_EQ(cs.envelope_tick, static_cast<uint16_t>(2));
    for (int i = 0; i < 4; ++i)
        engine::advance_envelope_and_fadeout(cs, false, 0, 128);
    CHECK_EQ(cs.envelope_tick, static_cast<uint16_t>(4));    // 4 * 0.5
    engine::advance_envelope_and_fadeout(cs, false, 0, 384); // 1.5
    CHECK_EQ(cs.envelope_tick, static_cast<uint16_t>(5));
    engine::advance_envelope_and_fadeout(cs, false, 0, 128); // остаток 0.5 + 0.5
    CHECK_EQ(cs.envelope_tick, static_cast<uint16_t>(6));

    engine::release_note(cs, false); // линейное затухание - убыль * число тиков
    const uint32_t before = cs.fadeout_level;
    engine::advance_envelope_and_fadeout(cs, false, 0, 768); // 3 тика
    CHECK_EQ(cs.fadeout_level, before - 3 * 1000);
}

// IT (kQuirkItSilentEnvelopeEndStops): огибающая громкости без петли
// прошла последнюю точку со значением 0 - снимается только голос
// (stop_voice_pending), ChannelState::voice_active остаётся: по нему
// диспетчер исполняет команды строк без ноты. На последней точке и при
// петле или удержании без отпускания голос не снимается.
void test_silent_envelope_end_stops_voice_only() {
    std::printf("test_silent_envelope_end_stops_voice_only\n");

    auto run = [](bool loop, bool sustain, bool released, bool last_zero, int ticks) {
        Fixture f(FrequencyModel::Linear);
        f.envelope.enabled           = true;
        f.envelope.point_count       = 2;
        f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
        f.envelope.points[1]         = soundsinth::model::EnvelopePoint{4, static_cast<int16_t>(last_zero ? 0 : 16)};
        f.envelope.loop_enabled      = loop;
        f.envelope.sustain_enabled   = sustain;
        f.envelope.loop_start        = 0;
        f.envelope.loop_end          = 1;
        f.envelope.sustain_point     = 1;
        f.envelope.sustain_end       = 1;
        f.instrument.volume_envelope = &f.envelope;
        f.dispatch_row(f.note_on(48));
        engine::ChannelState& cs               = f.channels[0];
        cs.key_released                        = released;
        cs.stop_voice_pending                  = false;
        const soundsinth::model::QuirkFlags it = soundsinth::model::kQuirkItEnvelopeSustainLoop | soundsinth::model::kQuirkItSilentEnvelopeEndStops;
        for (int i = 0; i < ticks; ++i)
            engine::advance_envelope_and_fadeout(cs, false, it, 256);
        CHECK(cs.voice_active); // память эффектов канала не зависит от огибающей
        return cs.stop_voice_pending;
    };
    CHECK(!run(false, false, false, true, 4));  // на последней точке ещё нет
    CHECK(run(false, false, false, true, 6));   // за нулевой последней точкой
    CHECK(!run(false, false, false, false, 6)); // последняя точка не ноль
    CHECK(!run(true, false, false, true, 20));  // петля
    CHECK(!run(false, true, false, true, 20));  // удержание, нота держится
    CHECK(run(false, true, true, true, 20));    // удержание, нота отпущена
}

// Fadeout (Instrument::fadeout_rate): после KeyOff/Note-Off убывает каждый
// тик и, дойдя до 0, останавливает голос (через тот же
// stop_voice_pending, что у NoteCut). fadeout_rate подобран так, чтобы
// дойти до 0 за 4 тика (65536/16384=4) - проверка точная.
void test_fadeout_stops_voice_after_key_off_when_configured() {
    std::printf("test_fadeout_stops_voice_after_key_off_when_configured\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled = true; // без envelope KeyOff остановил бы голос сразу - envelope нужен, чтобы попасть в путь fadeout
    f.envelope.point_count       = 1;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.instrument.volume_envelope = &f.envelope;
    f.instrument.fadeout_rate    = 16384; // 65536/16384 = 4 тика до нуля

    f.dispatch_row(f.note_on(48));
    f.dispatch_row(f.empty_with_effect(Effect::KeyOff, 0));
    CHECK(f.channels[0].voice_active);
    CHECK_EQ(f.channels[0].fadeout_level, static_cast<uint32_t>(65536)); // тик 0 KeyOff-строки ещё не сдвинул

    f.tick(0);
    CHECK_EQ(f.channels[0].fadeout_level, static_cast<uint32_t>(49152)); // 65536-16384
    CHECK(f.channels[0].voice_active);
    f.tick(0);
    CHECK_EQ(f.channels[0].fadeout_level, static_cast<uint32_t>(32768));
    f.tick(0);
    CHECK_EQ(f.channels[0].fadeout_level, static_cast<uint32_t>(16384));
    CHECK(f.channels[0].voice_active); // ещё не 0

    f.tick(0); // 4-й тик - доходит до 0
    CHECK_EQ(f.channels[0].fadeout_level, static_cast<uint32_t>(0));
    CHECK(!f.channels[0].voice_active); // теперь остановлен
    CHECK(f.channels[0].stop_voice_pending);
}

// fadeout_rate==0 (MOD/S3M или XM/IT-инструмент без fadeout): голос
// никогда не останавливается через fadeout сам, сколько угодно тиков
// после KeyOff/Note-Off, пока его не остановит что-то другое (новый
// триггер, NoteCut). Так и задумано.
void test_fadeout_zero_never_auto_stops_voice() {
    std::printf("test_fadeout_zero_never_auto_stops_voice\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled           = true;
    f.envelope.point_count       = 1;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.instrument.volume_envelope = &f.envelope;
    CHECK_EQ(f.instrument.fadeout_rate, static_cast<uint32_t>(0)); // значение по умолчанию в Fixture/Instrument

    f.dispatch_row(f.note_on(48));
    f.dispatch_row(f.empty_with_effect(Effect::KeyOff, 0));
    for (int i = 0; i < 200; ++i)
        f.tick(0); // заведомо много тиков
    CHECK(f.channels[0].voice_active);
    CHECK_EQ(f.channels[0].fadeout_level, static_cast<uint32_t>(65536)); // не сдвинулся ни разу
}

// Note-Off через колонку note (IT/S3M "===" -> kNoteOff) раньше молча
// игнорировался: is_real_note() исключал его из Note-Trigger, и больше он
// нигде не обрабатывался. Это был баг: ноты, отпущенные через колонку
// note, не останавливались. Путь release тот же, что у Effect::KeyOff
// (см. выше).
void test_note_off_via_note_column_enters_release() {
    std::printf("test_note_off_via_note_column_enters_release\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled           = true;
    f.envelope.sustain_enabled   = true;
    f.envelope.point_count       = 2;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.envelope.points[1]         = soundsinth::model::EnvelopePoint{4, 32};
    f.envelope.sustain_point     = 1;
    f.envelope.sustain_end       = 1;
    f.instrument.volume_envelope = &f.envelope;

    f.dispatch_row(f.note_on(48));
    f.dispatch_row(f.empty_with_effect(Effect::None, 0)); // строка без ноты и эффекта, note остаётся kNoteNone
    CHECK(f.channels[0].voice_active);
    CHECK(!f.channels[0].key_released);

    PatternCell note_off_cell;
    note_off_cell.note = soundsinth::model::kNoteOff; // "===" в колонке note, без instrument/effect
    f.dispatch_row(note_off_cell);
    CHECK(f.channels[0].voice_active); // не остановлен - есть envelope, ушёл в release
    CHECK(f.channels[0].key_released);
}

// Note-Cut через колонку note (IT "^^^" -> kNoteCut): в отличие от
// Note-Off выше, всегда мгновенная остановка независимо от envelope.
void test_note_cut_via_note_column_stops_immediately() {
    std::printf("test_note_cut_via_note_column_stops_immediately\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled           = true;
    f.envelope.point_count       = 1;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.instrument.volume_envelope = &f.envelope;

    f.dispatch_row(f.note_on(48));
    CHECK(f.channels[0].voice_active);

    PatternCell note_cut_cell;
    note_cut_cell.note = soundsinth::model::kNoteCut;
    f.dispatch_row(note_cut_cell);
    CHECK(!f.channels[0].voice_active); // мгновенно, несмотря на envelope
    CHECK(f.channels[0].stop_voice_pending);
}

// HighOffset (S3M/IT SAx) - старший "нибл" смещения, объединяется со
// следующим SampleOffset: trigger_sample_offset=(high<<16)|(param<<8)
// (сверено с libxmp src/effects.c FX_HIOFFSET/FX_OFFSET). Персистентен,
// каждую строку не сбрасывается.
void test_high_offset_combines_with_sample_offset_and_persists() {
    std::printf("test_high_offset_combines_with_sample_offset_and_persists\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::HighOffset, 2));
    CHECK_EQ(f.channels[0].sample_offset_high, static_cast<uint8_t>(2));
    CHECK_EQ(f.channels[0].trigger_sample_offset, static_cast<uint32_t>(0)); // сам по себе смещение не задаёт

    f.dispatch_row(f.note_on(48, Effect::SampleOffset, 5));
    const uint32_t expected = (uint32_t(2) << 16) | (uint32_t(5) << 8);
    CHECK_EQ(f.channels[0].trigger_sample_offset, expected);

    // Строка без HighOffset: старший нибл держится (персистентная память, в
    // отличие от trigger_sample_offset).
    f.dispatch_row(f.note_on(48, Effect::SampleOffset, 7));
    const uint32_t expected2 = (uint32_t(2) << 16) | (uint32_t(7) << 8);
    CHECK_EQ(f.channels[0].trigger_sample_offset, expected2);
}

// NoteCut (ECx/SCx) использует тот же счётчик, что Retrigger
// (retrig_type=kNoteCutRetrigType), и срабатывает один раз (в отличие от
// Retrigger не повторяется), см. .cpp. Та же семантика у libxmp:
// xc->retrig.type==0x10 в тех же структурах, что Multi Retrig.
void test_note_cut_stops_voice_once_after_interval_and_does_not_repeat() {
    std::printf("test_note_cut_stops_voice_once_after_interval_and_does_not_repeat\n");

    Fixture f(FrequencyModel::Amiga);
    f.dispatch_row(f.note_on(48, Effect::NoteCut, 3));
    CHECK(f.channels[0].voice_active);
    CHECK(f.channels[0].retrig_active);

    f.tick(1);
    CHECK(f.channels[0].voice_active); // ещё не дошли до интервала
    f.tick(2);
    CHECK(f.channels[0].voice_active);
    f.tick(3); // интервал достигнут - срез
    CHECK(!f.channels[0].voice_active);
    CHECK(f.channels[0].stop_voice_pending);
    CHECK(!f.channels[0].retrig_active); // разово, не перезаряжается

    // Дальнейшие тики ничего не делают: voice_active уже false, весь
    // остальной continuous-путь для этого канала завязан на него.
    f.channels[0].stop_voice_pending = false; // эмулируем потребление TrackerEngine
    f.tick(4);
    CHECK(!f.channels[0].stop_voice_pending);
}

// Volume envelope - кусочно-линейная интерполяция между точками,
// применяется как множитель (envelope_volume), сам cs.volume не трогается
// (в отличие от Retrigger/SetVolume). Точки {0,64},{8,0} (span=8)
// выбраны так, чтобы деление было без остатка. Тик читает текущую позицию и
// потом сдвигает её: первый тик ноты - точка 0.
void test_volume_envelope_interpolates_and_does_not_touch_volume() {
    std::printf("test_volume_envelope_interpolates_and_does_not_touch_volume\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled           = true;
    f.envelope.point_count       = 2;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.envelope.points[1]         = soundsinth::model::EnvelopePoint{8, 0};
    f.instrument.volume_envelope = &f.envelope;

    f.dispatch_row(f.note_on(48));
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(0));
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(64)); // дефолт сэмпла - не тронут огибающей

    f.tick(0); // огибающая движется и на тике 0, как Vibrato/Tremolo
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(1));
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(64)); // точка 0

    for (int i = 0; i < 4; ++i)
        f.tick(0); // прочитаны позиции 1..4
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(5));
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(32)); // 64 + (0-64)*4/8

    for (int i = 0; i < 4; ++i)
        f.tick(0); // позиции 5..8, конец огибающей
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(0));
    CHECK_EQ(f.channels[0].volume, static_cast<uint8_t>(64)); // по-прежнему не тронут - множитель отдельно
}

void test_volume_envelope_sustain_holds_indefinitely_while_voice_alive() {
    std::printf("test_volume_envelope_sustain_holds_indefinitely_while_voice_alive\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled           = true;
    f.envelope.sustain_enabled   = true;
    f.envelope.point_count       = 3;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.envelope.points[1]         = soundsinth::model::EnvelopePoint{4, 32}; // sustain здесь
    f.envelope.points[2]         = soundsinth::model::EnvelopePoint{8, 0};
    f.envelope.sustain_point     = 1;
    f.envelope.sustain_end       = 1;
    f.instrument.volume_envelope = &f.envelope;

    f.dispatch_row(f.note_on(48));
    for (int i = 0; i < 10; ++i)
        f.tick(0); // заведомо больше, чем нужно для достижения sustain (тик 4)

    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(4)); // держится, не доходит до 8
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(32));
}

// Loop (Envelope::loop_*) независим от sustain, заворачивает на
// loop_start при превышении loop_end. points[0].tick=0, points[1].tick=4:
// после 5-го тика envelope_tick должен обнулиться (без loop он держался
// бы на последней точке с envelope_volume=64, а не вернулся к 0).
void test_volume_envelope_loop_wraps_to_start() {
    std::printf("test_volume_envelope_loop_wraps_to_start\n");

    Fixture f(FrequencyModel::Amiga);
    f.song.quirks                = soundsinth::model::kQuirkItEnvelopeSustainLoop; // IT: заворот за концом петли, у XM - на нём
    f.envelope.enabled           = true;
    f.envelope.loop_enabled      = true;
    f.envelope.point_count       = 2;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 0};
    f.envelope.points[1]         = soundsinth::model::EnvelopePoint{4, 64};
    f.envelope.loop_start        = 0;
    f.envelope.loop_end          = 1;
    f.instrument.volume_envelope = &f.envelope;

    f.dispatch_row(f.note_on(48));
    for (int i = 0; i < 5; ++i)
        f.tick(0); // 1,2,3,4,5 - на 5-м envelope_tick(5) > loop_end_tick(4) -> заворот на 0

    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(0));
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(64)); // прочитана точка 4 перед заворотом
    f.tick(0);
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(0)); // points[0].value - заворот случился
}

void test_instrument_without_volume_envelope_stays_neutral() {
    std::printf("test_instrument_without_volume_envelope_stays_neutral\n");

    Fixture f(FrequencyModel::Amiga); // instrument.volume_envelope остаётся nullptr по умолчанию
    f.dispatch_row(f.note_on(48));
    f.tick(0);
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(64)); // нейтрально - множитель не действует
}

// Panning envelope - та же механика и интерполяция, что у volume envelope
// (test_volume_envelope_interpolates_and_does_not_touch_volume), но со
// своей независимой позицией (pan_envelope_tick, а не envelope_tick).
// Проверяется независимость: разная скорость движения по каждой
// огибающей на одних и тех же тиках. Сама формула смещения панорамы
// (глубина к центру/краям) применяется в TrackerEngine::render_add; здесь
// только продвижение и интерполяция значения (effect_dispatch звук не
// рендерит).
void test_panning_envelope_advances_independently_of_volume_envelope() {
    std::printf("test_panning_envelope_advances_independently_of_volume_envelope\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled           = true;
    f.envelope.point_count       = 2;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.envelope.points[1]         = soundsinth::model::EnvelopePoint{8, 0};
    f.instrument.volume_envelope = &f.envelope;

    f.pan_envelope.enabled     = true;
    f.pan_envelope.point_count = 2;
    f.pan_envelope.points[0]   = soundsinth::model::EnvelopePoint{0, 32}; // 32=нейтрально (центр)
    f.pan_envelope.points[1]   = soundsinth::model::EnvelopePoint{4, 64}; // вдвое короче volume-огибающей - движется быстрее
    f.instrument.panning_envelope = &f.pan_envelope;

    f.dispatch_row(f.note_on(48));
    CHECK_EQ(f.channels[0].pan_envelope_value, static_cast<uint8_t>(32)); // нейтрально до первого тика

    for (int i = 0; i < 4; ++i)
        f.tick(0);
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(4));      // прочитаны позиции 0..3
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(40));    // 64+(0-64)*3/8
    CHECK_EQ(f.channels[0].pan_envelope_tick, static_cast<uint16_t>(4));  // pan - у конца своей огибающей (длиной 4 тика)
    CHECK_EQ(f.channels[0].pan_envelope_value, static_cast<uint8_t>(56)); // 32+(64-32)*3/4
    f.tick(0);
    CHECK_EQ(f.channels[0].pan_envelope_value, static_cast<uint8_t>(64));
}

// Pitch envelope (только IT, ChannelState::pitch_envelope_offset): та же
// шкала хранения 0..64 с 32=нейтрально, что у pan, во всю шкалу +-16
// полутонов, как у OpenMPT и libxmp. raw=40 -> +4 полутона, 256 единиц
// linear_pitch (64 на полутон); raw=64 -> предел 255 шагов по 1/16 полутона,
// 1020. Только Linear-модель: под Amiga тик продвигается, но offset
// остаётся 0.
void test_pitch_envelope_applies_under_linear_model_only() {
    std::printf("test_pitch_envelope_applies_under_linear_model_only\n");

    Fixture linear(FrequencyModel::Linear);
    linear.pitch_envelope.enabled     = true;
    linear.pitch_envelope.point_count = 2;
    linear.pitch_envelope.points[0]   = soundsinth::model::EnvelopePoint{0, 32}; // нейтрально
    linear.pitch_envelope.points[1]   = soundsinth::model::EnvelopePoint{4, 40};
    linear.instrument.pitch_envelope  = &linear.pitch_envelope;

    linear.dispatch_row(linear.note_on(48));
    CHECK_EQ(linear.channels[0].pitch_envelope_offset, static_cast<int16_t>(0)); // нейтрально до первого тика
    for (int i = 0; i < 4; ++i)
        linear.tick(0);
    CHECK_EQ(linear.channels[0].pitch_envelope_tick, static_cast<uint16_t>(4));
    CHECK_EQ(linear.channels[0].pitch_envelope_offset, static_cast<int16_t>(256));

    Fixture top(FrequencyModel::Linear);
    top.pitch_envelope.enabled     = true;
    top.pitch_envelope.point_count = 2;
    top.pitch_envelope.points[0]   = soundsinth::model::EnvelopePoint{0, 32};
    top.pitch_envelope.points[1]   = soundsinth::model::EnvelopePoint{4, 64};
    top.instrument.pitch_envelope  = &top.pitch_envelope;
    top.dispatch_row(top.note_on(48));
    for (int i = 0; i < 4; ++i)
        top.tick(0);
    CHECK_EQ(top.channels[0].pitch_envelope_offset, static_cast<int16_t>(1020));

    Fixture amiga(FrequencyModel::Amiga);
    amiga.pitch_envelope.enabled     = true;
    amiga.pitch_envelope.point_count = 2;
    amiga.pitch_envelope.points[0]   = soundsinth::model::EnvelopePoint{0, 32};
    amiga.pitch_envelope.points[1]   = soundsinth::model::EnvelopePoint{4, 40};
    amiga.instrument.pitch_envelope  = &amiga.pitch_envelope;

    amiga.dispatch_row(amiga.note_on(48));
    for (int i = 0; i < 4; ++i)
        amiga.tick(0);
    CHECK_EQ(amiga.channels[0].pitch_envelope_tick, static_cast<uint16_t>(4));  // тик продвигается и тут
    CHECK_EQ(amiga.channels[0].pitch_envelope_offset, static_cast<int16_t>(0)); // но под Amiga не применяется (см. .h)
}

// SetEnvelopePosition (XM Lxx) переставляет обе позиции (volume и
// panning) на заданный тик (сверено с libxmp src/effects.c FX_ENVPOS, см.
// .cpp; FT2-специфичный баг сознательно не воспроизводится).
void test_set_envelope_position_moves_both_volume_and_pan_tick() {
    std::printf("test_set_envelope_position_moves_both_volume_and_pan_tick\n");

    Fixture f(FrequencyModel::Amiga);
    f.envelope.enabled           = true;
    f.envelope.point_count       = 2;
    f.envelope.points[0]         = soundsinth::model::EnvelopePoint{0, 64};
    f.envelope.points[1]         = soundsinth::model::EnvelopePoint{10, 0};
    f.instrument.volume_envelope = &f.envelope;

    f.pan_envelope.enabled        = true;
    f.pan_envelope.point_count    = 2;
    f.pan_envelope.points[0]      = soundsinth::model::EnvelopePoint{0, 32};
    f.pan_envelope.points[1]      = soundsinth::model::EnvelopePoint{10, 64};
    f.instrument.panning_envelope = &f.pan_envelope;

    f.dispatch_row(f.note_on(48));
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(0));
    CHECK_EQ(f.channels[0].pan_envelope_tick, static_cast<uint16_t>(0));

    f.dispatch_row(f.empty_with_effect(Effect::SetEnvelopePosition, 5));
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(5));
    CHECK_EQ(f.channels[0].pan_envelope_tick, static_cast<uint16_t>(5));

    f.tick(0); // тик читает новую позицию и сдвигает её на 1
    CHECK_EQ(f.channels[0].envelope_tick, static_cast<uint16_t>(6));
    CHECK_EQ(f.channels[0].envelope_volume, static_cast<uint8_t>(32)); // 64 + (0-64)*5/10 - звучит ровно точка Lxx
}

} // namespace

void run_effect_dispatch_porta_tests() {
    test_fine_slides_in_param();
    test_combined_volume_slides_by_format();
    test_it_lfo();
    test_midi_dispatch();
    test_silent_envelope_end_stops_voice_only();
    test_it_fade_rules();
    test_lone_instrument_restores_volume();
    test_retrigger_memory_and_counter();
    test_tone_porta_target_with_finetune();
    test_arpeggio_amiga_with_finetune();
    test_glissando_linear_grid_with_finetune();
    test_xm_fine_and_extra_fine_porta();
    test_xm_retrigger_ft2();
    test_envelope_loop_end_by_format();
    test_porta_up_per_tick_decreases_period_each_tick();
    test_porta_down_per_tick_increases_period_each_tick();
    test_porta_fine_applies_once_immediately_not_per_tick();
    test_porta_memory_reuses_last_nonzero_param();
    test_porta_clamps_to_amiga_period_table_bounds();
    test_porta_up_per_tick_under_linear_frequency_model();
    test_porta_down_per_tick_under_linear_frequency_model();
    test_tone_porta_moves_on_live_grid_without_tick_one();
    test_tone_porta_does_not_retrigger_and_converges_to_target_without_overshoot();
    test_tone_porta_vol_slide_converges_and_applies_volume_slide();
    test_tone_porta_resets_volume_to_sample_default_without_retriggering();
    test_tone_porta_holds_target_when_row_has_no_note();
    test_glissando_control_snaps_tone_porta_to_nearest_note();
    test_arpeggio_with_volume_column_vibrato();
    test_mod_panning_quirks();
    test_note_delay_defers_trigger_to_specified_tick();
    test_note_delay_with_zero_param_triggers_immediately();
    test_note_delay_pending_cell_cancelled_by_next_row();
    test_note_delay_keeps_previous_row_effects_until_replay();
    test_set_vibrato_waveform_selects_square_instead_of_sine();
    test_lfo_waveform_no_retrigger_bit();
    test_set_panbrello_waveform_is_independent_of_vibrato_waveform();
    test_set_finetune_shifts_period_on_same_row_trigger_amiga();
    test_set_finetune_shifts_linear_pitch_on_same_row_trigger_linear();
    test_set_finetune_resets_on_next_trigger_without_command();
    test_sample_own_finetune_applies_without_any_effect_command();
    test_tone_porta_first_note_on_channel_triggers_normally();
    test_tone_porta_converges_to_target_under_linear_frequency_model();
    test_vibrato_phase_holds_on_tick_zero_and_does_not_touch_period();
    test_vibrato_offset_clears_when_not_respecified_on_later_row();
    test_vibrato_phase_resets_on_retrigger();
    test_vibrato_speed_and_depth_memory_are_independent();
    test_vibrato_applies_under_linear_frequency_model();
    test_fine_vibrato_depth_has_no_shift_unlike_vibrato();
    test_vibrato_vol_slide_continues_vibrato_and_applies_volume_slide();
    test_arpeggio_cycles_period_amiga();
    test_arpeggio_cycles_linear_pitch_offset_linear_model();
    test_set_channel_volume_and_slide();
    test_set_global_volume_and_slide();
    test_channel_and_global_volume_apply_without_active_voice();
    test_volume_column_set_volume_and_slide_up();
    test_volume_column_set_panning_is_already_0_64_scale();
    test_surround_then_volume_column_panning();
    test_volume_column_portamento_up_amiga_and_linear_signs();
    test_volume_column_tone_porta_xm_scale_and_suppresses_retrigger();
    test_volume_column_tone_porta_it_scale_uses_lookup_table();
    test_volume_column_offset_scales_by_2048();
    test_volume_column_vibrato_speed_sets_speed_keeps_depth_memory();
    test_tremolo_phase_holds_on_tick_zero_and_does_not_touch_volume();
    test_tremolo_offset_clears_when_not_respecified_on_later_row();
    test_tremor_alternates_muted_by_on_off_tick_counts();
    test_tremor_memory_reuses_whole_byte_not_per_nibble();
    test_tremor_works_under_linear_frequency_model_too();
    test_set_panning_scales_0_255_to_0_64();
    test_panbrello_applies_even_on_tick_in_row_zero_and_does_not_touch_pan();
    test_panning_slide_per_tick_and_memory();
    test_panning_slide_direction_it_s3m();
    test_volume_slide_both_nibbles_by_format();
    test_arpeggio_memory_it_s3m();
    test_note_delay_zero_it_s3m();
    test_sample_default_panning_minus1_inherits_else_applied_on_trigger();
    test_sample_offset_scales_by_256_and_remembers_last_nonzero();
    test_retrigger_fires_at_interval_and_applies_volume_table();
    test_retrigger_mod_e9x_style_param_has_no_volume_type();
    test_keyoff_stops_voice_without_envelope();
    test_keyoff_with_envelope_enters_release_instead_of_stopping();
    test_envelope_carry_keeps_position_on_new_note();
    test_it_envelope_sustain_loop();
    test_envelope_time_step();
    test_fadeout_stops_voice_after_key_off_when_configured();
    test_fadeout_zero_never_auto_stops_voice();
    test_note_off_via_note_column_enters_release();
    test_note_cut_via_note_column_stops_immediately();
    test_high_offset_combines_with_sample_offset_and_persists();
    test_note_cut_stops_voice_once_after_interval_and_does_not_repeat();
    test_volume_envelope_interpolates_and_does_not_touch_volume();
    test_volume_envelope_sustain_holds_indefinitely_while_voice_alive();
    test_volume_envelope_loop_wraps_to_start();
    test_instrument_without_volume_envelope_stays_neutral();
    test_panning_envelope_advances_independently_of_volume_envelope();
    test_pitch_envelope_applies_under_linear_model_only();
    test_set_envelope_position_moves_both_volume_and_pan_tick();
}
