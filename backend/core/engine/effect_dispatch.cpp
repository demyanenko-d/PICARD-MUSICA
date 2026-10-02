// SPDX-License-Identifier: MIT
#include "core/engine/effect_dispatch.h"

#include <cmath>
#include <iterator>

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "core/config.h"
#include "core/engine/engine_defs.h"
#include "core/model/amiga_period.h"

namespace soundsinth::engine {

using soundsinth::model::Effect;
using soundsinth::model::EffectCommand;
using soundsinth::model::Envelope;
using soundsinth::model::FrequencyModel;
using soundsinth::model::Instrument;
using soundsinth::model::is_real_note;
using soundsinth::model::PatternCell;
using soundsinth::model::QuirkFlags;
using soundsinth::model::resolve_sample_index;
using soundsinth::model::SampleDescriptor;
using soundsinth::model::SlideRate;
using soundsinth::model::Song;
using soundsinth::model::VolumeColumnCommand;
using soundsinth::model::VolumeColumnType;

namespace {

// Раскладка Axy/Dxy/Kxy/Pxy: верхний нибл - слайд вверх, нижний - вниз.
// Оба ненулевые - редкий, но реальный случай; побеждает "вниз" у S3M
// (kQuirkS3mVolSlideDownPriority), "вверх" у MOD/XM. Флаг общий для
// VolumeSlide и PanningSlide.
// Ограниченная дельта [0, max]: громкость, громкость канала и панорама
// живут на шкале 0..64 (центр панорамы 32), глобальная громкость
// (0..128) передаёт свой потолок явно.
void apply_bounded_slide_delta(uint8_t& value, uint8_t param, bool down_priority, uint8_t max = kVolumeMax) {
    const uint8_t up   = static_cast<uint8_t>(param >> 4);
    const uint8_t down = static_cast<uint8_t>(param & 0x0fu);

    int16_t v = value;
    if (up > 0 && down > 0) {
        v += down_priority ? -static_cast<int16_t>(down) : static_cast<int16_t>(up);
    } else if (up > 0) {
        v += up;
    } else if (down > 0) {
        v -= down;
    }
    if (v < 0) v = 0;
    if (v > max) v = max;
    value = static_cast<uint8_t>(v);
}

// Шаг слайда глобальной громкости со знаком, ниблы - как у
// apply_bounded_slide_delta. У S3M и XM шкала файла 0..64 - шаг вдвое, у IT
// 0..128 - как есть, как у OpenMPT.
int8_t global_volume_slide_step(uint8_t param, bool down_priority, bool it) {
    const int32_t up   = param >> 4;
    const int32_t down = param & 0x0fu;
    const int32_t step = (up != 0 && !(down != 0 && down_priority)) ? up : -down;
    return static_cast<int8_t>(it ? step : step * 2);
}

void apply_global_volume_step(uint8_t& volume, int8_t step) {
    const int32_t v = static_cast<int32_t>(volume) + step;
    volume          = static_cast<uint8_t>(v < 0 ? 0 : (v > kGlobalVolumeMax ? kGlobalVolumeMax : v));
}

// "00 повторяет последний ненулевой": ноль берёт память, иначе параметр
// запоминается.
uint8_t recall(uint8_t param, uint8_t& memory) {
    if (param != 0) memory = param;
    return memory;
}

// Ячейка без ноты и эффектов - вместо пропущенной (S3M SD0).
const PatternCell kEmptyCell;

// IT игнорирует слайд громкости, громкости канала и глобальной громкости с
// обоими ненулевыми ниблами (тонкие xF/Fx разобраны раньше), как OpenMPT.
bool it_ignores_slide(bool it, uint8_t param) {
    return it && (param >> 4) != 0 && (param & 0x0fu) != 0;
}

// Выделить тонкий вариант слайда из параметра - так его кодируют S3M и IT
// (kQuirkFineSlideInParam; у MOD/XM это отдельные команды). true - слайд
// тонкий; в param остаётся один нибл, годный для apply_bounded_slide_delta.
//
// Порядок проверок как в OpenMPT: сначала xF, потом Fy, поэтому FF - "тонко
// вверх на 15", а не "тонко вниз".
bool split_fine_slide(uint8_t& param) {
    const uint8_t up   = static_cast<uint8_t>(param >> 4);
    const uint8_t down = static_cast<uint8_t>(param & 0x0fu);
    if (down == 0x0f && up != 0) {
        param = static_cast<uint8_t>(up << 4);
        return true;
    }
    if (up == 0x0f && down != 0) {
        param = down;
        return true;
    }
    return false;
}

// Слайд шкалы 0..64 (громкость, громкость канала) с памятью "00" и тонким
// вариантом в параметре (S3M, IT): тонкий - разово на тике 0 (без
// fine_on_tick0 пропадает), обычный взводит active на тики 1..speed-1.
// it_rules - правило IT: слайд с двумя ненулевыми ниблами не идёт.
void row_slide(uint8_t& value, uint8_t& memory, bool& active, const EffectCommand& effect, bool fine_in_param, bool down_priority, bool it_rules,
               bool fine_on_tick0) {
    uint8_t param   = recall(effect.param, memory);
    const bool fine = fine_in_param && split_fine_slide(param);
    if (!fine && effect.rate == SlideRate::PerTick) {
        active = !it_ignores_slide(it_rules, param);
    } else if (fine_on_tick0) {
        apply_bounded_slide_delta(value, param, down_priority);
    }
}

// Pxy в раскладке apply_bounded_slide_delta (старший нибл - вправо, как у
// XM). У S3M и IT ниблы наоборот: P0x - вправо, Px0 - влево; оба ненулевые -
// у S3M вправо, у IT слайда нет, как у OpenMPT.
inline uint8_t pan_slide_param(uint8_t param, QuirkFlags quirks) {
    if ((quirks & soundsinth::model::kQuirkFineSlideInParam) == 0) return param;
    const uint8_t left  = static_cast<uint8_t>(param >> 4);
    const uint8_t right = static_cast<uint8_t>(param & 0x0fu);
    if (left != 0 && right != 0) {
        return (quirks & soundsinth::model::kQuirkItEffectBeforeVolColumn) != 0 ? 0 : static_cast<uint8_t>(right << 4);
    }
    return static_cast<uint8_t>((right << 4) | left);
}

// PortaUp/PortaDown в модели Amiga: знаковая дельта периода с ограничением
// (clamp_amiga_period): 113..856 при kQuirkAmigaLimits, иначе только в
// пределах 16 бит.
void apply_porta_delta(ChannelState& cs, int16_t delta, bool amiga_limits) {
    cs.period = soundsinth::model::clamp_amiga_period(static_cast<int32_t>(cs.period) + delta, amiga_limits);
}

// Шаг value к target на speed, не перескакивая цель.
template <typename T>
void step_toward(T& value, T target, T speed) {
    if (value == target) return;
    if (value > target) {
        const T remaining = static_cast<T>(value - target);
        value             = static_cast<T>(value - ((remaining < speed) ? remaining : speed));
    } else {
        const T remaining = static_cast<T>(target - value);
        value             = static_cast<T>(value + ((remaining < speed) ? remaining : speed));
    }
}

// TonePorta: высота сходится к цели на cs.porta_memory за тик и не
// перескакивает её; Amiga - период, Linear - linear_pitch. GlissandoControl
// высоту не трогает: ступени по полутонам - только в звучащей высоте
// (TrackerEngine), иначе слайд медленнее половины полутона за тик прижимался
// бы обратно и стоял.
void apply_tone_porta(ChannelState& cs) {
    step_toward<uint16_t>(cs.period, cs.tone_porta_target, cs.porta_memory);
}

void apply_tone_porta_linear(ChannelState& cs) {
    step_toward<int32_t>(cs.linear_pitch, cs.linear_tone_porta_target, cs.porta_memory);
}

// Слайд в модели Linear без ограничения: linear_pitch не привязан к таблице.
void apply_linear_porta_delta(ChannelState& cs, int16_t delta) {
    cs.linear_pitch += delta;
}

// Сдвиг высоты канала на дельту в его модели: Amiga - период с ограничением,
// Linear - linear_pitch.
void porta_step(ChannelState& cs, int16_t delta, bool amiga_pitch, bool amiga_limits) {
    if (amiga_pitch) {
        apply_porta_delta(cs, delta, amiga_limits);
    } else {
        apply_linear_porta_delta(cs, delta);
    }
}

// Скорость портаменто в единицах модели: Amiga - период как есть, Linear -
// 1/64 полутона, вчетверо больше.
uint16_t porta_units(uint32_t speed, bool amiga_pitch) {
    return static_cast<uint16_t>(amiga_pitch ? speed : speed * 4);
}

// Знак дельты по модели: у Amiga период обратно пропорционален высоте
// (вверх - период меньше), у Linear linear_pitch прямо пропорционален.
int16_t porta_signed(int32_t amount, bool is_up, bool amiga_pitch) {
    return static_cast<int16_t>((amiga_pitch == is_up) ? -amount : amount);
}

// Шаг тонкого портаменто: тонкий - та же величина, что обычный слайд за
// тик; сверхтонкий - вчетверо мельче, в модели Amiga округляется до целого
// периода (X11 и EE1 не сдвигают).
int32_t fine_porta_step(uint32_t amount, bool extra, bool amiga_pitch) {
    if (!extra) return porta_units(amount, amiga_pitch);
    return amiga_pitch ? static_cast<int32_t>((amount + 2) / 4) : static_cast<int32_t>(amount);
}

// Сырой байт портаменто S3M/IT: тонкий (xF?) и сверхтонкий (xE?) слайд - в
// параметре, память хранит сырой байт, тонкость решается после неё. true -
// байт тонкого вида: шаг применён разово на тике 0, а EF0 и EE0 не делают
// ничего; false - обычный слайд, скорость в raw.
constexpr uint8_t kFinePortaPrefix      = 0xf0;
constexpr uint8_t kExtraFinePortaPrefix = 0xe0;

bool apply_raw_fine_porta(ChannelState& cs, uint8_t& raw, bool is_up, bool amiga_pitch, bool amiga_limits) {
    raw                  = recall(raw, cs.porta_raw_memory);
    const uint8_t prefix = raw & 0xf0u;
    if (prefix != kFinePortaPrefix && prefix != kExtraFinePortaPrefix) return false;
    const uint8_t amount = raw & 0x0fu;
    if (amount != 0) {
        const int32_t step = fine_porta_step(amount, prefix == kExtraFinePortaPrefix, amiga_pitch);
        porta_step(cs, porta_signed(step, is_up, amiga_pitch), amiga_pitch, amiga_limits);
    }
    return true;
}

// Синус вибрато ProTracker/libxmp, 64 точки, амплитуда 0..255, сверено с
// libxmp побайтово. Общая для Vibrato, Tremolo и Panbrello; у IT - только
// Panbrello.
constexpr int16_t kVibratoSineTable[64] = {
    0,    24,   49,   74,   97,   120,  141,  161,  180,  197,  212,  224,  235,  244,  250,  253,  // 0-15
    255,  253,  250,  244,  235,  224,  212,  197,  180,  161,  141,  120,  97,   74,   49,   24,   // 16-31
    0,    -24,  -49,  -74,  -97,  -120, -141, -161, -180, -197, -212, -224, -235, -244, -250, -253, // 32-47
    -255, -253, -250, -244, -235, -224, -212, -197, -180, -161, -141, -120, -97,  -74,  -49,  -24,  // 48-63
};

// xorshift32, общий на процесс, только для случайной волны LFO (waveform=3).
// Побитового совпадения с каким-либо трекером нет и не нужно: у каждого
// свой генератор, стандарта нет.
uint32_t lfo_random_state = 0x9e3779b9u; // произвольное ненулевое зерно

int32_t lfo_next_random() {
    uint32_t& s  = lfo_random_state;
    s           ^= s << 13;
    s           ^= s >> 17;
    s           ^= s << 5;
    return static_cast<int32_t>(s % 512) - 256;
}

// Тот же генератор под диапазон IT-таблицы (+-64). Отдельная функция:
// остаток от уже отмасштабированного lfo_next_random() дал бы смещённое
// распределение.
int32_t it_lfo_next_random() {
    uint32_t& s  = lfo_random_state;
    s           ^= s << 13;
    s           ^= s >> 17;
    s           ^= s << 5;
    return static_cast<int32_t>(s % 128) - 64;
}

// Значение LFO для одной из 4 форм волны при фазе 0..63, диапазон около
// [-255, 255] (случайная - [-256, 255]); один порядок величины для всех
// форм, чтобы depth и делители Vibrato/Tremolo/Panbrello работали
// одинаково. waveform: 0 синус (умолчание во всех форматах), 1 спад-пила,
// 2 прямоугольник, 3 случайная; бит 0x04 (без сброса фазы) не учитывается.
// Одна формула на все форматы: прямоугольник ST3 (255/0) и сдвиг фазы пилы
// FT2 не воспроизводятся.
int32_t lfo_waveform_value(uint8_t waveform, uint8_t phase) {
    switch (waveform & 0x03u) {
        case 1:
            return 255 - phase * 8; // спад: +255 на фазе 0 до -249 на фазе 63
        case 2:
            return phase < 32 ? 255 : -255;
        case 3:
            return lfo_next_random();
        default:
            return kVibratoSineTable[phase];
    }
}

// Таблица IT (ITSinusTable, сверена побайтово с ITTECH.TXT): 256 точек,
// амплитуда +-64 - не 64 точки и +-255, как kVibratoSineTable. Индекс -
// полная фаза 0..255; фаза продвигается на 4*vibrato_speed за тик.
constexpr int8_t kItVibratoSineTable[256] = {
    0,   2,   3,   5,   6,   8,   9,   11,  12,  14,  16,  17,  19,  20,  22,  23,  // 0-15
    24,  26,  27,  29,  30,  32,  33,  34,  36,  37,  38,  39,  41,  42,  43,  44,  // 16-31
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  56,  57,  58,  59,  // 32-47
    59,  60,  60,  61,  61,  62,  62,  62,  63,  63,  63,  64,  64,  64,  64,  64,  // 48-63
    64,  64,  64,  64,  64,  64,  63,  63,  63,  62,  62,  62,  61,  61,  60,  60,  // 64-79
    59,  59,  58,  57,  56,  56,  55,  54,  53,  52,  51,  50,  49,  48,  47,  46,  // 80-95
    45,  44,  43,  42,  41,  39,  38,  37,  36,  34,  33,  32,  30,  29,  27,  26,  // 96-111
    24,  23,  22,  20,  19,  17,  16,  14,  12,  11,  9,   8,   6,   5,   3,   2,   // 112-127
    0,   -2,  -3,  -5,  -6,  -8,  -9,  -11, -12, -14, -16, -17, -19, -20, -22, -23, // 128-143
    -24, -26, -27, -29, -30, -32, -33, -34, -36, -37, -38, -39, -41, -42, -43, -44, // 144-159
    -45, -46, -47, -48, -49, -50, -51, -52, -53, -54, -55, -56, -56, -57, -58, -59, // 160-175
    -59, -60, -60, -61, -61, -62, -62, -62, -63, -63, -63, -64, -64, -64, -64, -64, // 176-191
    -64, -64, -64, -64, -64, -64, -63, -63, -63, -62, -62, -62, -61, -61, -60, -60, // 192-207
    -59, -59, -58, -57, -56, -56, -55, -54, -53, -52, -51, -50, -49, -48, -47, -46, // 208-223
    -45, -44, -43, -42, -41, -39, -38, -37, -36, -34, -33, -32, -30, -29, -27, -26, // 224-239
    -24, -23, -22, -20, -19, -17, -16, -14, -12, -11, -9,  -8,  -6,  -5,  -3,  -2,  // 240-255
};

// LFO для IT, как у OpenMPT: диапазон +-64, прямоугольник 0..64
// (асимметричен), случайная [-64, 63], фаза 0..255.
int32_t it_lfo_waveform_value(uint8_t waveform, uint8_t phase) {
    switch (waveform & 0x03u) {
        case 1:
            return 64 - (phase + 1) / 2; // спад
        case 2:
            return phase < 128 ? 64 : 0; // прямоугольник - асимметричный
        case 3:
            return it_lfo_next_random(); // равномерно [-64, 63]
        default:
            return kItVibratoSineTable[phase];
    }
}

// Делитель вибрато со знаком: минус - высота сначала вниз. Как у OpenMPT:
// - Amiga - 512: таблица * нибл / 128 периода, как ProTracker и FT2; рост
//   периода - высота вниз. Для таблицы IT в Amiga не сверялся: IT почти
//   всегда играет в модели Linear;
// - Linear, единица 1/64 полутона: IT - 64 (таблица IT +-64), сначала
//   вверх, с Old Effects -32; XM - -128, как FT2: (таблица 0..255 * нибл) >>
//   5; .mid - 64, под эту шкалу конвертер считает глубину CC1.
int32_t vibrato_divisor(QuirkFlags quirks, bool amiga_pitch) {
    if (amiga_pitch) return 512;
    if ((quirks & soundsinth::model::kQuirkXmVolColumnBeforeEffect) != 0) return -128;
    return (quirks & soundsinth::model::kQuirkItOldEffects) != 0 ? -32 : 64;
}

// LFO вибрато, тремоло и панбрелло: значение формы в текущей фазе, затем шаг
// фазы. Скорость фазы у таблицы IT - 4*speed по 256 точкам (частота та же,
// что у speed по 64). advance == false - фаза стоит (тик 0 строки у MOD, S3M,
// XM и IT с Old Effects). Отдельной функцией в SRAM: встроенная, она
// раскрывалась в каждой копии цикла по каналам.
SOUNDSINTH_NOINLINE int32_t SOUNDSINTH_HOT_PATH(lfo_step)(uint8_t waveform, uint8_t& phase, uint8_t speed, bool it_table, bool advance) {
    const int32_t value = it_table ? it_lfo_waveform_value(waveform, phase) : lfo_waveform_value(waveform, phase);
    const uint8_t step  = advance ? speed : 0;
    phase               = it_table ? static_cast<uint8_t>(phase + 4 * step) : static_cast<uint8_t>((phase + step) & 0x3fu);
    return value;
}

// Finetune канала: подмена S2x/E5x или свой у сэмпла.
int8_t effective_finetune(const ChannelState& cs, int8_t sample_finetune) {
    return cs.finetune_override_active ? cs.finetune_override : sample_finetune;
}

// Период ноты с finetune - та же формула, что на Note-Trigger: period *=
// 2^(-finetune/1536) с округлением.
uint16_t amiga_period_with_finetune(uint8_t note, int8_t finetune) {
    const uint16_t period = soundsinth::model::amiga_note_to_period(note);
    if (finetune == 0) return period;
    const double fine   = finetune;
    const double factor = std::pow(2.0, -fine / 128.0 / 12.0);
    return static_cast<uint16_t>(period * factor + 0.5);
}

// Arpeggio: эффективная нота по tick%3 - 0 база, 1 +arpeggio_x, 2
// +arpeggio_y полутонов (схема MOD/S3M/XM/IT, как в libxmp). Amiga берёт
// периоды нот, посчитанные на строке с finetune канала: период нелинеен по
// ноте, дельтой не выразить. Linear - offset*64 (полутон = 64 единицы).
// Возвращает смещение высоты этого тика.
int32_t arpeggio_offset_amiga(const ChannelState& cs, uint16_t tick_in_row) {
    const uint8_t phase  = static_cast<uint8_t>(tick_in_row % 3);
    const uint8_t offset = phase == 1 ? cs.arpeggio_x : (phase == 2 ? cs.arpeggio_y : 0);
    if (offset == 0) return 0;
    const int32_t target = phase == 1 ? cs.arpeggio_period_x : cs.arpeggio_period_y;
    return target - cs.period;
}

int32_t arpeggio_offset_linear(const ChannelState& cs, uint16_t tick_in_row) {
    const uint8_t phase  = static_cast<uint8_t>(tick_in_row % 3);
    const uint8_t offset = phase == 1 ? cs.arpeggio_x : (phase == 2 ? cs.arpeggio_y : 0);
    return static_cast<int32_t>(offset) * kLinearAmountUnitsPerSemitone;
}

// Tremolo: та же синус-таблица, что Vibrato, но на громкости и без <<2 у
// depth. Делитель общей ветки 64, а не
// 512, как у Vibrato: разные делители у периода и громкости, не опечатка.
// cs.volume не ограничивает - пишет cs.volume_offset, TrackerEngine
// прибавляет и ограничивает при рендере.
//
// kQuirkItVibratoTable: у настоящего IT таблица +-64 и затухание 5
// (делитель 32, не 64). Максимум на depth=15
// у libxmp-формулы около 60 единиц volume_offset, у IT - 30.
void apply_tremolo_tick(ChannelState& cs, bool it_table, bool advance) {
    const int32_t raw = lfo_step(cs.tremolo_waveform, cs.tremolo_phase, cs.tremolo_speed, it_table, advance) * cs.tremolo_depth;
    cs.volume_offset  = static_cast<int16_t>(raw / (it_table ? 32 : 64));
}

// Panbrello: та же таблица и тот же принцип временного отклонения, но на
// панораме. depth = нибл<<4, как в libxmp. Делитель 2048 = 512*4: libxmp
// делит на 512 в своей шкале 0..255, у нас 0..64 - вчетверо уже.
void apply_panbrello_tick(ChannelState& cs) {
    const int32_t raw = lfo_step(cs.panbrello_waveform, cs.panbrello_phase, cs.panbrello_speed, false, true) * cs.panbrello_depth;
    cs.pan_offset     = static_cast<int16_t>(raw / 2048);
}

// Tremor: чередование "звучит/молчит" по счётчику тиков, конечный автомат.
// Счётчик 0 переключает фазу
// и перезаряжается длиной другой фазы, уменьшается каждый тик. Пишет
// cs.tremor_muted - TrackerEngine глушит канал на этом тике, cs.volume не
// трогается.
void apply_tremor_tick(ChannelState& cs) {
    if (cs.tremor_counter == 0) {
        cs.tremor_on_phase = !cs.tremor_on_phase;
        cs.tremor_counter  = cs.tremor_on_phase ? cs.tremor_on_ticks : cs.tremor_off_ticks;
    }
    --cs.tremor_counter;
    cs.tremor_muted = !cs.tremor_on_phase;
}

// Модификатор громкости мульти-ретриггера (верхний нибл параметра, 0..15),
// таблица ProTracker/S3M/IT/XM, общая для форматов, сверена с libxmp.
// Применяется как (volume + add) * mul / div - именно в этом порядке.
struct RetrigVolumeOp {
    int8_t add;
    uint8_t mul;
    uint8_t div;
};
constexpr RetrigVolumeOp kRetrigVolumeTable[16] = {
    {0, 1, 1}, {-1, 1, 1}, {-2, 1, 1}, {-4, 1, 1}, {-8, 1, 1}, {-16, 1, 1}, {0, 2, 3}, {0, 1, 2},
    {0, 1, 1}, {1, 1, 1},  {2, 1, 1},  {4, 1, 1},  {8, 1, 1},  {16, 1, 1},  {0, 3, 2}, {0, 2, 1},
};

// Метка retrig_type: не ретриггер, а NoteCut (ECx/SCx). Использует тот же
// счётчик retrig_counter/retrig_interval, что и Retrigger. Нибл Retrigger не
// выходит за 0..15, поэтому 0x10 однозначен.
constexpr uint8_t kNoteCutRetrigType = 0x10;
static_assert(kNoteCutRetrigType >= std::size(kRetrigVolumeTable), "the NoteCut marker collided with a retrigger table index");

void apply_retrigger_volume(ChannelState& cs) {
    const RetrigVolumeOp& op = kRetrigVolumeTable[cs.retrig_type];
    int32_t v                = (static_cast<int32_t>(cs.volume) + op.add) * op.mul / op.div;
    if (v < 0) v = 0;
    if (v > kVolumeMax) v = kVolumeMax;
    cs.volume = static_cast<uint8_t>(v);
}

// Значение огибающей в точке tick: кусочно-линейная интерполяция между
// точками (значения на шкале 0..64). До первой точки - значение первой,
// после последней - значение последней. Отдельной функцией в SRAM: встроенная,
// она раскрывалась в четыре огибающие.
SOUNDSINTH_NOINLINE uint8_t SOUNDSINTH_HOT_PATH(evaluate_envelope)(const Envelope& env, uint16_t tick) {
    // Огибающая включена, но без точек - у настоящих файлов не бывает.
    if (env.point_count == 0) return kEnvelopeNeutral;
    if (tick <= env.points[0].tick) return static_cast<uint8_t>(env.points[0].value);
    for (uint8_t i = 1; i < env.point_count; ++i) {
        if (tick <= env.points[i].tick) {
            const auto& a = env.points[i - 1];
            const auto& b = env.points[i];
            if (b.tick == a.tick) return static_cast<uint8_t>(b.value);
            const int32_t span  = b.tick - a.tick;
            const int32_t pos   = tick - a.tick;
            const int32_t from  = a.value;
            const int32_t value = from + (b.value - from) * pos / span;
            return static_cast<uint8_t>(value);
        }
    }
    return static_cast<uint8_t>(env.points[env.point_count - 1].value);
}

// Позиция огибающей на steps тиков вперёд (у .mid при смене темпа и 0).
// Удержанная нота стоит на точке удержания, у IT (kEnvItSustainLoop)
// крутится по петле удержания, и обычная петля тогда не действует; иначе
// обычная петля от отпускания не зависит. В SRAM: зовётся каждый тик на
// каждый голос и сама не встраивается.
// Правила позиции огибающей, битами. kEnvItSustainLoop - петля удержания IT.
// kEnvFt2Loop - обычная петля по правилам FT2 (XM): заворот на её конце
// (точка конца не звучит), петля с концом на точке удержания после
// отпускания не крутится; без него (IT, .mid) - заворот за концом петли.
constexpr uint32_t kEnvItSustainLoop = 1u;
constexpr uint32_t kEnvFt2Loop       = 2u;

void SOUNDSINTH_HOT_PATH(advance_envelope_tick)(uint16_t& tick, bool key_released, const Envelope& env, uint32_t rules, uint32_t steps) {
    const bool it_sustain_loop = (rules & kEnvItSustainLoop) != 0;
    const bool ft2_loop        = (rules & kEnvFt2Loop) != 0;
    const bool sustained       = !key_released && env.sustain_enabled && env.sustain_end < env.point_count;
    uint32_t wrap_at           = 0xffffffffu; // петли нет
    if (env.loop_enabled && env.loop_end < env.point_count && !(ft2_loop && key_released && env.sustain_enabled && env.loop_end == env.sustain_end)) {
        wrap_at = env.points[env.loop_end].tick + (ft2_loop ? 0u : 1u);
    }
    const uint16_t loop_start_tick = env.loop_start < env.point_count ? env.points[env.loop_start].tick : 0;
    uint32_t pos                   = tick;
    for (uint32_t s = 0; s < steps; ++s) {
        if (sustained && it_sustain_loop) {
            const uint16_t start = env.sustain_point < env.point_count ? env.points[env.sustain_point].tick : 0;
            pos                  = (pos >= env.points[env.sustain_end].tick) ? start : pos + 1;
            continue;
        }
        if (!(sustained && pos >= env.points[env.sustain_end].tick)) {
            ++pos;
        }
        if (pos >= wrap_at) pos = loop_start_tick;
    }
    // Насыщение: иначе через 65536 тиков позиция завернёт в 0 и доигравшая
    // огибающая зазвучит снова.
    tick = static_cast<uint16_t>(pos > 0xffffu ? 0xffffu : pos);
}

// Канальные и песенные эффекты и постоянные режимы канала - состояние, а не
// свойство ноты: действуют и без голоса. Mxx на пустой строке задаёт
// громкость канала для ноты, которая придёт позже; Xxx или S8x до первой
// ноты канала - у 640 из 9661 файлов IT. SetGlobalVolume и GlobalVolumeSlide -
// песенного уровня (ctx->ps). Прочие команды здесь ничего не делают.
SOUNDSINTH_NOINLINE void apply_channel_effect(DispatchContext* ctx, ChannelState& cs, const EffectCommand& effect) {
    const QuirkFlags quirks         = ctx->song->quirks;
    const bool down_priority        = (quirks & soundsinth::model::kQuirkS3mVolSlideDownPriority) != 0;
    const bool fine_in_param        = (quirks & soundsinth::model::kQuirkFineSlideInParam) != 0;
    const bool effect_before_volcol = (quirks & soundsinth::model::kQuirkItEffectBeforeVolColumn) != 0;
    switch (effect.type) {
        case Effect::SetChannelVolume:
            cs.channel_volume = effect.param > kVolumeMax ? kVolumeMax : effect.param;
            break;
        case Effect::ChannelVolumeSlide:
            row_slide(cs.channel_volume, cs.channel_volume_slide_memory, cs.channel_volume_slide_active, effect, fine_in_param, down_priority,
                      effect_before_volcol, true);
            break;
        case Effect::SetGlobalVolume:
            ctx->ps->global_volume = effect.param > kGlobalVolumeMax ? kGlobalVolumeMax : effect.param;
            break;
        case Effect::GlobalVolumeSlide: {
            uint8_t param     = recall(effect.param, ctx->ps->global_volume_slide_memory);
            const bool fine   = fine_in_param && split_fine_slide(param);
            const int8_t step = global_volume_slide_step(param, down_priority, effect_before_volcol);
            if (!fine && effect.rate == SlideRate::PerTick) {
                // на тиках 1..speed-1
                ctx->ps->global_volume_slide_active = !it_ignores_slide(effect_before_volcol, param);
                ctx->ps->global_volume_slide_step   = step;
            } else {
                apply_global_volume_step(ctx->ps->global_volume, step); // Fine - разово, тик 0
            }
            break;
        }
        case Effect::SetPanning: {
            // Разово, различия PerTick/Fine у этой команды нет ни в одном формате.
            // param - байт 0..255, в шкалу 0..64 делится на 4.
            if ((quirks & soundsinth::model::kQuirkModIgnorePanning) != 0) break;
            uint32_t param = effect.param;
            if ((quirks & soundsinth::model::kQuirkMod7BitPanning) != 0) {
                if (param == 0xa4) { // 7-битный surround, как S91
                    cs.surround = true;
                    cs.pan      = kPanCenter;
                    break;
                }
                param = param * 2 > 255 ? 255 : param * 2;
            }
            const uint32_t scaled = param / 4;
            cs.pan                = static_cast<uint8_t>(scaled > kPanMax ? kPanMax : scaled);
            cs.surround           = false;
            break;
        }
        case Effect::SetPanning4Bit: {
            if ((quirks & soundsinth::model::kQuirkModIgnorePanning) != 0) break;
            // S3M/IT S8x: param 4-битный (0..15), а не 0..255, как у SetPanning, -
            // другая формула. Как у OpenMPT: pan_0_256 =
            // (param*256 + 8) / 15; здесь сразу в шкале 0..64, округление +2/15.
            const uint32_t scaled = (static_cast<uint32_t>(effect.param) * kPanMax + 2) / 15;
            cs.pan                = static_cast<uint8_t>(scaled > kPanMax ? kPanMax : scaled);
            cs.surround           = false;
            break;
        }
        case Effect::HighOffset:
            cs.sample_offset_high = effect.param; // держится до нового SAx, как в libxmp
            break;
        case Effect::GlissandoControl:
            // param 0 - выключено, иначе включено. Построчно не сбрасывается, режим
            // держится до явного переключения.
            cs.glissando_enabled = effect.param != 0;
            break;
        // SetVibratoWaveform/SetTremoloWaveform/SetPanbrelloWaveform (S3x/S4x/S5x
        // IT/S3M, E4x/E7x MOD): param & 0x03 - 0 синус, 1 спад, 2 прямоугольник, 3
        // случайная. У вибрато и тремоло бит 0x04 - та же форма без сброса фазы
        // на новой ноте.
        case Effect::SetVibratoWaveform:
            cs.vibrato_waveform = effect.param & 0x07u;
            break;
        case Effect::SetTremoloWaveform:
            cs.tremolo_waveform = effect.param & 0x07u;
            break;
        case Effect::SetPanbrelloWaveform:
            cs.panbrello_waveform = effect.param & 0x03u;
            break;
        default:
            break;
    }
}

// Панорама колонки громкости, и без звучащего голоса - тот же случай, что
// apply_channel_effect у колонки эффекта. Шкала уже 0..64 (param = vol -
// 128), в отличие от SetPanning колонки эффекта (0..255): не делить повторно.
void apply_volume_column_panning(ChannelState& cs, const VolumeColumnCommand& vc) {
    if (vc.type == VolumeColumnType::SetPanning) {
        cs.pan      = vc.param > kPanMax ? kPanMax : vc.param;
        cs.surround = false;
    }
}

// Колонка громкости: все 14 типов VolumeColumnType, кроме None. Порядок относительно
// колонки эффекта - по формату: у IT эффект раньше
// (kQuirkItEffectBeforeVolColumn), у остальных после.
// TonePorta из колонки громкости у IT (kQuirkItVolColumnPortaTable): param
// 0..9 через эту таблицу, а не линейно, как у XM (сверена с OpenMPT
// побайтово). Индексы 10..15 в файлах не встречаются, таблица полная.
constexpr uint8_t kItPortaVolCmdTable[16] = {0, 1, 4, 8, 16, 32, 64, 96, 128, 255, 255, 255, 255, 255, 255, 255};

void process_volume_column(ChannelState& cs, const VolumeColumnCommand& vc, const Song& song) {
    const bool amiga_pitch    = song.frequency_model == FrequencyModel::Amiga;
    const bool down_priority  = (song.quirks & soundsinth::model::kQuirkS3mVolSlideDownPriority) != 0;
    const bool it_porta_table = (song.quirks & soundsinth::model::kQuirkItVolColumnPortaTable) != 0;
    switch (vc.type) {
        case VolumeColumnType::SetVolume:
            cs.volume = vc.param > kVolumeMax ? kVolumeMax : vc.param;
            break;
        case VolumeColumnType::SlideUp: {
            const uint8_t packed   = static_cast<uint8_t>(vc.param << 4); // вверх - верхний нибл
            cs.volume_slide_memory = packed;
            cs.volume_slide_active = true;
            break;
        }
        case VolumeColumnType::SlideDown:
            cs.volume_slide_memory = vc.param; // "down" в нижнем нибле
            cs.volume_slide_active = true;
            break;
        case VolumeColumnType::FineSlideUp:
            apply_bounded_slide_delta(cs.volume, static_cast<uint8_t>(vc.param << 4), down_priority);
            break;
        case VolumeColumnType::FineSlideDown:
            apply_bounded_slide_delta(cs.volume, vc.param, down_priority);
            break;
        case VolumeColumnType::SetPanning:
            apply_volume_column_panning(cs, vc);
            break;
        case VolumeColumnType::PanSlideLeft: // "влево" = уменьшение - тот же нижний нибл, что и SlideDown у громкости
            cs.pan_slide_memory = vc.param;
            cs.pan_slide_step   = vc.param;
            cs.pan_slide_active = true;
            break;
        case VolumeColumnType::PanSlideRight:
            cs.pan_slide_memory = static_cast<uint8_t>(vc.param << 4);
            cs.pan_slide_step   = cs.pan_slide_memory;
            cs.pan_slide_active = true;
            break;
        case VolumeColumnType::VibratoDepth:
            // Тот же <<2, что у нижнего нибла
            // Effect::Vibrato, и вибрато включается всегда, даже при param==0 (в
            // отличие от Effect::Vibrato, где 0 не трогает память).
            if (vc.param != 0) cs.vibrato_depth = static_cast<uint8_t>(vc.param << 2);
            cs.vibrato_active = true;
            break;
        case VolumeColumnType::PortamentoUp:
        case VolumeColumnType::PortamentoDown: {
            // vol<<2 перед тем же путём PortaUp/PortaDown, что у колонки эффекта.
            uint16_t param = static_cast<uint16_t>(vc.param << 2);
            if (param == 0) {
                param = cs.porta_memory;
            } else {
                param           = porta_units(param, amiga_pitch);
                cs.porta_memory = param;
            }
            cs.porta_delta  = porta_signed(param, vc.type == VolumeColumnType::PortamentoUp, amiga_pitch);
            cs.porta_active = true;
            break;
        }
        case VolumeColumnType::TonePorta: {
            // Скорость - по своей шкале колонки громкости: IT - таблица
            // kItPortaVolCmdTable, XM - param*16. Дальше тот же путь в porta_memory/tone_porta_active, что у
            // Effect::TonePorta; ретриггер уже подавлен в dispatch_row_effects.
            const uint16_t raw = it_porta_table ? kItPortaVolCmdTable[vc.param & 0x0fu] : static_cast<uint16_t>(vc.param * 16);
            if (raw != 0) cs.porta_memory = porta_units(raw, amiga_pitch);
            cs.tone_porta_active = true;
            cs.glissando_porta   = true;
            break;
        }
        case VolumeColumnType::Offset:
            // Только IT (vol 223..232): param 1..9 -> смещение param*2048 отсчётов -
            // те же точки по умолчанию, что OpenMPT подставляет файлам без своих.
            // Свои точки OpenMPT не поддержаны. param==0 - повторить прошлое смещение:
            // cs.trigger_sample_offset не трогаем.
            if (vc.param != 0) cs.trigger_sample_offset = static_cast<uint32_t>(vc.param) * 2048;
            break;
        case VolumeColumnType::VibratoSpeed:
            // Только XM: ставит скорость, вибрато не включает (FT2) - его включают Vx
            // или 4xy.
            cs.vibrato_speed = vc.param & 0x0fu;
            break;
        default:
            break;
    }
}

} // namespace

SOUNDSINTH_NOINLINE void SOUNDSINTH_HOT_PATH(advance_envelope_and_fadeout)(ChannelState& cs, bool amiga_pitch, QuirkFlags quirks,
                                                                           uint32_t envelope_time_step_q8) {
    const bool exponential_fadeout = (quirks & soundsinth::model::kQuirkFadeoutExponential) != 0;
    const bool it_envelopes        = (quirks & soundsinth::model::kQuirkItEnvelopeSustainLoop) != 0;
    const bool stop_at_silent_end  = (quirks & soundsinth::model::kQuirkItSilentEnvelopeEndStops) != 0;
    // Сколько тиков огибающих и затухания приходится на этот тик движка: у
    // трекеров ровно один, у .mid - по отношению темпов, с дробным остатком.
    uint32_t steps = 1;
    if (envelope_time_step_q8 != kQ8One) {
        const uint32_t acc    = static_cast<uint32_t>(cs.envelope_time_frac) + envelope_time_step_q8;
        steps                 = acc >> kQ8Bits;
        cs.envelope_time_frac = static_cast<uint8_t>(acc & (kQ8One - 1u));
    }
    // Огибающие громкости и панорамы у трекеров читаются по текущей позиции и
    // потом сдвигаются: первый тик ноты звучит точкой 0, как у OpenMPT и
    // libxmp. У .mid - сдвиг, потом чтение (атаки банка сверены так). Питч и
    // фильтр - сдвиг, потом чтение: так сходится экспорт OpenMPT по высоте.
    const bool read_first = !exponential_fadeout;
    const uint32_t rules =
        (static_cast<uint32_t>(it_envelopes) * kEnvItSustainLoop) | (static_cast<uint32_t>(!(it_envelopes || exponential_fadeout)) * kEnvFt2Loop);
    // Огибающая громкости - свойство играющего инструмента, а не команда
    // эффекта: построчно не переуказывается, продвигается, пока жив голос.
    if (cs.voice_active && cs.volume_envelope != nullptr) {
        // .mid: огибающая громкости отпущенной ноты стоит, релиз ведёт затухание -
        // в SF2 релиз заменяет остаток спада.
        if (!(exponential_fadeout && cs.key_released)) {
            const uint16_t before = cs.envelope_tick;
            advance_envelope_tick(cs.envelope_tick, cs.key_released, *cs.volume_envelope, rules, steps);
            cs.envelope_volume = evaluate_envelope(*cs.volume_envelope, read_first ? before : cs.envelope_tick);
        }
        // .mid: нота снята, когда огибающая уже в нуле, - голос кончился. У
        // трекеров голос живёт до нуля fadeout_level: ноль внутри огибающей
        // законен, на нём держатся хвосты NNA.
        if (exponential_fadeout && cs.key_released && cs.envelope_volume == 0) {
            stop_voice(cs);
        }
        // IT: огибающая без петли прошла последнюю точку (удержания нет или
        // нота отпущена) - включается затухание, и у неотпущенной ноты. При
        // нулевой последней точке голос больше не зазвучит: снимается только
        // Voice (stop_voice_pending), ChannelState::voice_active остаётся - по
        // нему диспетчер решает, исполнять ли команды строки без ноты.
        if (it_envelopes || stop_at_silent_end) {
            const Envelope& env = *cs.volume_envelope;
            if (env.enabled && env.point_count > 0 && !env.loop_enabled && (!env.sustain_enabled || cs.key_released) &&
                cs.envelope_tick > env.points[env.point_count - 1].tick) {
                if (it_envelopes) cs.note_fading = true;
                if (stop_at_silent_end && env.points[env.point_count - 1].value == 0) cs.stop_voice_pending = true;
            }
        }
    } else if (cs.voice_active) {
        cs.envelope_volume = kEnvelopeNeutral; // нет огибающей - множитель нейтрален
    }
    // Огибающая панорамы: та же механика (advance_envelope_tick,
    // evaluate_envelope, своя позиция). Значение сырое, 0..64, 32 - нейтрально;
    // глубина влияния считается в TrackerEngine::render_add:
    // finalpan = pan + (env - 32) * depth / 32.
    if (cs.voice_active && cs.panning_envelope != nullptr) {
        const uint16_t before = cs.pan_envelope_tick;
        advance_envelope_tick(cs.pan_envelope_tick, cs.key_released, *cs.panning_envelope, rules, steps);
        cs.pan_envelope_value = evaluate_envelope(*cs.panning_envelope, read_first ? before : cs.pan_envelope_tick);
    } else if (cs.voice_active) {
        cs.pan_envelope_value = kEnvelopeCenter; // нет огибающей - нейтрально: в формуле выше (32 - 32) * depth = 0
    }
    // Огибающая питча (только IT): тик продвигается всегда, а смещение
    // применяется только в модели Linear. Точка 0..64 (32 - нейтрально) - это
    // +-16 полутонов: *8 даёт шаги 1/16 полутона с пределом +-255, как у
    // OpenMPT, *4 - единицы linear_pitch (1/64 полутона).
    if (cs.voice_active && cs.pitch_envelope != nullptr) {
        advance_envelope_tick(cs.pitch_envelope_tick, cs.key_released, *cs.pitch_envelope, rules, steps);
        if (!amiga_pitch) {
            const int32_t raw = evaluate_envelope(*cs.pitch_envelope, cs.pitch_envelope_tick);
            int32_t steps16   = (raw - kEnvelopeCenter) * 8;
            if (steps16 > 255) steps16 = 255;
            if (steps16 < -255) steps16 = -255;
            cs.pitch_envelope_offset = static_cast<int16_t>(steps16 * 4);
        }
    } else if (cs.voice_active) {
        cs.pitch_envelope_offset = 0;
    }
    // Огибающая фильтра: результат - модификатор среза -256..+256, перевод из
    // 0..64 как value*8 - 256. Нейтраль +256, а не 0: огибающая на максимуме
    // обязана давать то же, что её отсутствие, - открытый срез.
    if (cs.voice_active && cs.filter_envelope != nullptr) {
        advance_envelope_tick(cs.filter_envelope_tick, cs.key_released, *cs.filter_envelope, rules, steps);
        const int32_t raw      = evaluate_envelope(*cs.filter_envelope, cs.filter_envelope_tick);
        cs.filter_env_modifier = static_cast<int16_t>(raw * 8 - kFilterEnvNeutral);
    } else if (cs.voice_active) {
        cs.filter_env_modifier = kFilterEnvNeutral;
    }
    // Затухание убывает каждый тик, включая тик 0, пока оно включено
    // (note_fading). instrument_fadeout_rate==0 (MOD/S3M или инструмент без
    // затухания) - "до нуля этим путём не доходит", нота держится, пока не
    // остановлена иначе. Дойдя до 0 - остановка голоса через
    // stop_voice_pending, как у NoteCut.
    if (cs.voice_active && cs.note_fading) {
        if (exponential_fadeout) {
            // Затухание в децибелах: множитель на тик вместо убыли
            // (kQuirkFadeoutExponential). Ниже порога -48 дБ голос неразличим,
            // держать его значит занимать канал ради тишины.
            // Ноль означает то же, что у линейного затухания, - "не доходит до нуля":
            // без этой проверки умножение на ноль обрывало бы голос сразу при
            // отпускании.
            if (cs.instrument_fadeout_rate != 0) {
                constexpr uint32_t kSilentLevel = kQ16One >> 8; // -48 дБ
                for (uint32_t s = 0; s < steps; ++s) {
                    cs.fadeout_level = (cs.fadeout_level * cs.instrument_fadeout_rate) >> kQ16Bits;
                }
                if (cs.fadeout_level < kSilentLevel) cs.fadeout_level = 0;
            }
        } else {
            const uint32_t drop = cs.instrument_fadeout_rate * steps;
            cs.fadeout_level    = (cs.fadeout_level > drop) ? cs.fadeout_level - drop : 0;
        }
        if (cs.fadeout_level == 0) {
            stop_voice(cs);
        }
    }
}

void channels_init(const Song& song, ChannelState* channels) {
    const bool paula = (song.quirks & soundsinth::model::kQuirkModHardwarePanning) != 0;
    for (uint8_t ch = 0; ch < song.channel_count; ++ch) {
        channels[ch] = ChannelState{};
        // Разводка Paula у MOD (каналы 0 и 3 по модулю 4 влево, 1 и 2 вправо) -
        // свойство устройства; загрузчик MOD channel_pan не заполняет.
        channels[ch].pan            = paula ? (((ch % 4) == 0 || (ch % 4) == 3) ? 0 : kPanMax) : song.channel_pan[ch];
        channels[ch].channel_volume = song.channel_volume[ch];
        channels[ch].surround       = ((song.channel_surround >> ch) & 1u) != 0;
    }
}

namespace {

// Флаги потиковых эффектов живут одну строку; tone_porta_target, фазы LFO
// и счётчик tremor остаются. Проход строки зовёт сброс после перехвата
// NoteDelay: у отложенной ячейки эффекты прошлой строки идут до доигрыша,
// как у libxmp для MOD, S3M и XM. Доигрыш NoteDelay зовёт его перед ячейкой.
void begin_row(ChannelState& cs) {
    cs.volume_slide_active         = false;
    cs.porta_active                = false;
    cs.tone_porta_active           = false;
    cs.vibrato_active              = false;
    cs.tremolo_active              = false;
    cs.tremor_active               = false;
    cs.pan_slide_active            = false;
    cs.panbrello_active            = false;
    cs.retrig_active               = false;
    cs.arpeggio_active             = false;
    cs.channel_volume_slide_active = false;
    cs.trigger_sample_offset       = 0;     // SampleOffset ниже поставит заново
    cs.stop_voice_pending          = false; // защитно: обычно уже потреблён TrackerEngine
    cs.triggered_this_row          = false; // взводится ниже только на настоящий Note-Trigger
}

// Срез на Note-Trigger, только .mid: у трекерных форматов velocity_to_cutoff
// 0 и filter_follows_note false. Живой вход возьмёт её же для CC74/CC71.
void apply_midi_trigger_filter(ChannelState& cs, const Song& song, const Instrument& ins, const PatternCell& cell) {
    // Яркость от силы удара: банк задаёт её модулятором initialFilterFc <-
    // velocity. Сила удара - из колонки громкости (у .mid velocity сведена с
    // CC7 и CC11): канал, приглушённый контроллером, выходит темнее.
    if (ins.velocity_to_cutoff != 0 && cs.filter_cutoff < kFilterCutoffOpen) {
        const int32_t vol = (cell.volume.type == VolumeColumnType::SetVolume) ? static_cast<int32_t>(cell.volume.param) : kVolumeMax;
        int32_t c         = static_cast<int32_t>(cs.filter_cutoff) + (vol - 32) * static_cast<int32_t>(ins.velocity_to_cutoff) / 32;
        if (c < 0) c = 0;
        if (c > kFilterCutoffOpen) c = kFilterCutoffOpen;
        cs.filter_cutoff = static_cast<uint8_t>(c);
    }
    // Срез не ниже основного тона (Song::filter_follows_note).
    // Перевод точный: срез в герцах 110 * 2^(0.25 + c/U), U - делений на октаву
    // (Song::filter_units_per_octave, у .mid 16), нота MIDI n звучит на
    // 440 * 2^((n-69)/12), отсюда c = U*(n-48)/12 (при U = 24 это 2n - 96). У .mid в
    // ячейке номер ноты MIDI как есть. Запас SOUNDSINTH_MIDI_FILTER_NOTE_MARGIN
    // (24 деления - октава).
    if (song.filter_follows_note && cs.filter_cutoff < kFilterCutoffOpen && cell.note <= soundsinth::model::kNoteMax) {
        const int32_t upo = song.filter_units_per_octave;
        const int32_t floor_cut =
            upo * (static_cast<int32_t>(cell.note) - 48) / 12 + SOUNDSINTH_MIDI_FILTER_NOTE_MARGIN * upo / soundsinth::model::kFilterUnitsIt;
        if (floor_cut > static_cast<int32_t>(cs.filter_cutoff)) {
            cs.filter_cutoff = static_cast<uint8_t>(floor_cut > kFilterCutoffOpen ? kFilterCutoffOpen : floor_cut);
        }
    }
}

// Note-Trigger канала ch: сэмпл sample_idx инструмента ячейки найден и не
// пуст, resolved_note - нота после keymap. Поля Song, которые он читает, -
// у DispatchContext::song.
void trigger_note(DispatchContext* ctx, uint8_t ch, const PatternCell& cell, uint16_t sample_idx, uint8_t resolved_note) {
    const Song& song                = *ctx->song;
    ChannelState& cs                = ctx->channels[ch];
    const Instrument& ins           = song.instruments[cell.instrument - 1];
    const SampleDescriptor& sample  = song.samples[sample_idx];
    const bool fine_in_param        = (song.quirks & soundsinth::model::kQuirkFineSlideInParam) != 0;
    const bool effect_before_volcol = (song.quirks & soundsinth::model::kQuirkItEffectBeforeVolColumn) != 0;
    const bool amiga_pitch          = song.frequency_model == FrequencyModel::Amiga;
    // NNA (только IT): прямо перед перезаписью cs, пока в ней ещё старый голос.
    // Может увести старый голос в фон (Continue/Off/Fade) - фон получает свою
    // копию заранее, а cs и Voice[channel] код ниже перезапишет как обычно.
    if (ctx->on_note_trigger_nna != nullptr) {
        ctx->on_note_trigger_nna(ctx->nna_user, ch, cell.instrument, sample_idx, resolved_note);
    }
    cs.voice_active    = true;
    cs.volume          = sample.default_volume;
    cs.last_note       = resolved_note; // не cell.note: keymap мог её переопределить
    cs.last_instrument = cell.instrument;
    cs.sample_index    = sample_idx;
    // Глобальная громкость сэмпла (IT GvL) - статический множитель, отдельный
    // от инструментального; применяется и без инструмента.
    cs.sample_global_volume = sample.global_volume;
    cs.triggered_this_row   = true;
    // Настоящий ретриггер сбрасывает фазы, кроме форм с битом 0x04; тремоло
    // у IT не сбрасывается никогда, как у OpenMPT. TonePorta сюда не попадает.
    if ((cs.vibrato_waveform & 0x04u) == 0) cs.vibrato_phase = 0;
    if ((cs.tremolo_waveform & 0x04u) == 0 && !effect_before_volcol) cs.tremolo_phase = 0;
    cs.glissando_porta = false;
    // Огибающие - указатели из Instrument; cell.instrument уже проверен. С
    // carry (IT) позиция огибающей новой нотой не сбрасывается, как у
    // OpenMPT и libxmp.
    cs.volume_envelope = ins.volume_envelope;
    if (cs.volume_envelope == nullptr || !cs.volume_envelope->carry) cs.envelope_tick = 0;
    cs.envelope_volume  = kEnvelopeNeutral; // нейтрально до первого apply_continuous_effects этой ноты
    cs.panning_envelope = ins.panning_envelope;
    if (cs.panning_envelope == nullptr || !cs.panning_envelope->carry) cs.pan_envelope_tick = 0;
    cs.pan_envelope_value = kEnvelopeCenter; // нейтрально
    cs.pitch_envelope     = ins.pitch_envelope;
    if (cs.pitch_envelope == nullptr || !cs.pitch_envelope->carry) cs.pitch_envelope_tick = 0;
    cs.pitch_envelope_offset = 0; // нейтрально
    // Фильтр (IT): срез и резонанс из инструмента, только если он их задаёт
    // (бит 0x80). Не задаёт - открытый срез и нулевой резонанс, фильтра нет,
    // пока его не включит огибающая или Zxx.
    cs.filter_envelope = ins.filter_envelope;
    if (cs.filter_envelope == nullptr || !cs.filter_envelope->carry) cs.filter_envelope_tick = 0;
    cs.filter_env_modifier = kFilterEnvNeutral; // нейтрально до первого продвижения огибающей
    cs.filter_cutoff       = (ins.filter_cutoff & 0x80u) ? static_cast<uint8_t>(ins.filter_cutoff & 0x7fu) : kFilterCutoffOpen;
    cs.filter_resonance    = (ins.filter_resonance & 0x80u) ? static_cast<uint8_t>(ins.filter_resonance & 0x7fu) : 0;
    apply_midi_trigger_filter(cs, song, ins, cell);
    // Состояние Note-Off - заново на каждый настоящий триггер.
    cs.key_released = false;
    cs.note_fading  = false;
    // Счёт ретриггера у S3M - от ноты; у IT нота его не сбрасывает, только
    // Qxy на её строке; у XM Rxy не сбрасывает вовсе (у MOD он и так заново
    // на каждой строке).
    if (fine_in_param && !effect_before_volcol) cs.retrig_counter = 0;
    cs.fadeout_level           = kQ16One;
    cs.instrument_fadeout_rate = ins.fadeout_rate;
    // Instrument::global_volume (только IT) - статический множитель инструмента.
    cs.instrument_global_volume = ins.global_volume;
    // Настоящий триггер возвращает канал к finetune сэмпла; SetFinetune на этой
    // же строке ниже включит подмену заново (порядок как в libxmp).
    cs.finetune_override_active = false;
    // Панорама на Note-Trigger - три источника в этом порядке, как у OpenMPT:
    // (1) панорама инструмента (-1 - своей нет, только IT); (2) панорама
    // сэмпла, если задана, перебивает (1); если нет ни той, ни другой -
    // панорама не трогается (обычно у S3M/MOD).
    if (ins.instrument_panning >= 0) {
        cs.pan      = static_cast<uint8_t>(ins.instrument_panning);
        cs.surround = false;
    }
    if (sample.default_panning >= 0) {
        cs.pan      = static_cast<uint8_t>(sample.default_panning);
        cs.surround = false;
    }
    // (3) Pitch-Pan Separation (только IT): сдвиг (нота - pitch_pan_center) *
    // separation / 8 поверх (1)/(2); в шкале 0..256 это / 2.
    if (ins.pitch_pan_separation != 0) {
        const int32_t from_center = resolved_note - ins.pitch_pan_center;
        const int32_t delta       = from_center * ins.pitch_pan_separation / 8;
        int32_t pan               = static_cast<int32_t>(cs.pan) + delta;
        if (pan < 0) pan = 0;
        if (pan > kPanMax) pan = kPanMax;
        cs.pan      = static_cast<uint8_t>(pan);
        cs.surround = false;
    }
    // cs.effective_note - для обеих моделей: Arpeggio в Amiga читает её каждый
    // тик, Linear здесь же считает из неё начальную linear_pitch.
    cs.effective_note = soundsinth::model::apply_relative_note(resolved_note, sample.relative_note);
    if (amiga_pitch) {
        cs.period = soundsinth::model::amiga_note_to_period(cs.effective_note);
    } else {
        const int32_t reference = linear_reference_note(song.quirks);
        cs.linear_pitch         = (cs.effective_note - reference) * kLinearAmountUnitsPerSemitone;
    }
}

// Ячейка канала ch поверх его состояния: Note-Trigger, колонки ноты,
// громкости и эффекта. NoteDelay здесь не перехватывается. Не встраивается:
// разбор ячейки один на проход строки и доигрыш NoteDelay.
SOUNDSINTH_NOINLINE void dispatch_cell(DispatchContext* ctx, uint8_t ch, const PatternCell& cell) {
    const Song& song         = *ctx->song;
    ChannelState& cs         = ctx->channels[ch];
    const bool down_priority = (song.quirks & soundsinth::model::kQuirkS3mVolSlideDownPriority) != 0;
    // S3M/IT прячут тонкий вариант слайда в параметре - split_fine_slide.
    const bool fine_in_param = (song.quirks & soundsinth::model::kQuirkFineSlideInParam) != 0;
    // Amiga и Linear - разные шкалы: период против единиц 1/64 полутона.
    // Porta/TonePorta/Vibrato ниже ветвятся по этому флагу.
    const bool amiga_pitch  = song.frequency_model == FrequencyModel::Amiga;
    const bool amiga_limits = (song.quirks & soundsinth::model::kQuirkAmigaLimits) != 0;
    const bool it_envelopes = (song.quirks & soundsinth::model::kQuirkItEnvelopeSustainLoop) != 0;
    // Порядок колонок по формату: у IT эффект раньше колонки громкости
    // (kQuirkItEffectBeforeVolColumn), у XM и остальных - наоборот.
    const bool effect_before_volcol    = (song.quirks & soundsinth::model::kQuirkItEffectBeforeVolColumn) != 0;
    const bool gxx_shares_porta_memory = (song.quirks & soundsinth::model::kQuirkGxxSharesPortaMemory) != 0;
    const bool xm_fine_memory          = (song.quirks & soundsinth::model::kQuirkXmVolColumnBeforeEffect) != 0;
    const bool fine_combined_slides    = (song.quirks & soundsinth::model::kQuirkS3mIgnoreCombinedFineSlides) == 0;
    // Номер инструмента без ноты возвращает громкость сэмпла: MOD, S3M, IT. У
    // XM к этому ещё сброс огибающих и затухания - не сделан; у .mid таких
    // ячеек нет.
    const bool lone_instrument_volume = (song.quirks & (soundsinth::model::kQuirkXmVolColumnBeforeEffect | soundsinth::model::kQuirkFadeoutExponential)) == 0;

    // TonePorta/TonePortaVolSlide: если голос уже играет, нота на строке не
    // ретриггерит сэмпл, а задаёт новую цель, к которой сходится период.
    // Если голоса нет, сходиться не от чего - обычный триггер. У
    // TonePortaVolSlide та же логика плюс VolumeSlide.
    // TonePorta из колонки громкости (IT/XM) - та же семантика: подавляет
    // ретриггер и задаёт цель.
    const bool is_tone_porta_family =
        cell.effect.type == Effect::TonePorta || cell.effect.type == Effect::TonePortaVolSlide || cell.volume.type == VolumeColumnType::TonePorta;
    const bool suppress_retrigger_for_tone_porta = is_tone_porta_family && cs.voice_active;

    // Номер инструмента без ноты у живого голоса (приём MOD/S3M: погасить
    // слайд, затем ударить тем же сэмплом без перезапуска): громкость -
    // умолчание сэмпла, как у OpenMPT и libxmp; колонка громкости строки
    // ниже её перебьёт. У MOD и S3M - сэмпл инструмента, если в нём есть
    // данные; у IT - только тот же инструмент, играющий сэмпл (другой
    // номер у OpenMPT перезапускает ноту).
    if (lone_instrument_volume && cell.instrument != 0 && cell.note == soundsinth::model::kNoteNone && cs.voice_active) {
        if (effect_before_volcol) {
            if (cell.instrument == cs.last_instrument) cs.volume = song.samples[cs.sample_index].default_volume;
        } else {
            uint16_t sample_idx   = 0;
            uint8_t resolved_note = cs.last_note;
            if (resolve_sample_index(song, cell.instrument, cs.last_note, &sample_idx, &resolved_note) && song.samples[sample_idx].length_samples > 0) {
                cs.volume = song.samples[sample_idx].default_volume;
            }
        }
    }

    // Note-Trigger: громкость - на умолчание сэмпла, voice_active взводится,
    // только если у инструмента находится играющий сэмпл (length_samples > 0).
    // Ссылку на пустой слот (обычное дело в файлах) ProTracker игнорирует:
    // канал продолжает то, что играло, - сверено с libxmp. Явные команды строки
    // ниже применяются в любом случае.
    if (is_real_note(cell.note) && cell.instrument != 0) {
        uint16_t sample_idx   = 0;
        uint8_t resolved_note = cell.note; // keymap может переопределить ноту (IT) - resolve_sample_index
        bool unmapped         = true;
        const bool playable =
            resolve_sample_index(song, cell.instrument, cell.note, &sample_idx, &resolved_note, &unmapped) && song.samples[sample_idx].length_samples > 0;
        if (suppress_retrigger_for_tone_porta) {
            // Нота с инструментом на строке с TonePorta не ретриггерит сэмпл, но
            // громкость всё равно сбрасывается на умолчание сэмпла, если колонки
            // громкости на строке нет: у OpenMPT сброс не защищён проверкой
            // портаменто. Явная колонка громкости этой строки перезапишет cs.volume
            // ниже.
            if (playable) cs.volume = song.samples[sample_idx].default_volume;
        } else if (playable) {
            trigger_note(ctx, ch, cell, sample_idx, resolved_note);
        } else if (cs.voice_active) {
            // Сэмпла для ноты нет, звучать ей нечем. Обрывать ли уже идущий голос,
            // зависит от формата и от того, что не нашлось - нота вне keymap или пустой
            // сэмпл.
            const QuirkFlags cut_quirk = unmapped ? soundsinth::model::kQuirkCutOnUnmappedNote : soundsinth::model::kQuirkCutOnEmptySample;
            if ((song.quirks & cut_quirk) != 0) {
                stop_voice(cs);
            }
        }
    }

    // Note-Off через колонку ноты (IT/S3M "===" -> kNoteOff, IT "^^^" ->
    // kNoteFade); XM Kxx - тот же путь. У IT === -
    // отпускание, ^^^ - затухание (release_note, fade_note), голос живёт,
    // пока не дотухнет. У остальных с огибающей громкости - релиз и
    // затухание, без неё - мгновенная остановка.
    if (cs.voice_active && (cell.note == soundsinth::model::kNoteOff || cell.note == soundsinth::model::kNoteFade)) {
        if (it_envelopes) {
            if (cell.note == soundsinth::model::kNoteOff) {
                release_note(cs, true);
            } else {
                fade_note(cs, true);
            }
        } else {
            key_off(cs);
        }
    }
    // Note-Cut через колонку ноты (kNoteCut): всегда мгновенная остановка,
    // огибающая и затухание ни при чём - как Effect::NoteCut.
    if (cs.voice_active && cell.note == soundsinth::model::kNoteCut) {
        stop_voice(cs);
    }

    // TonePorta/TonePortaVolSlide: цель обновляется, если на строке есть нота;
    // иначе держится прежняя - слайд продолжается. Высота цели - после switch:
    // E5x той же строки меняет finetune.
    int16_t porta_target_note = -1;
    if (cs.voice_active && is_tone_porta_family && is_real_note(cell.note)) {
        // Тот же поиск по keymap, что у Note-Trigger: сэмпл не меняется (TonePorta не
        // переключает инструмент, берётся cs.last_instrument), но целевую ноту
        // keymap инструмента (IT) переопределить может.
        uint16_t unused_sample_idx = 0;
        uint8_t resolved_note      = cell.note;
        resolve_sample_index(song, cs.last_instrument, cell.note, &unused_sample_idx, &resolved_note);
        porta_target_note = soundsinth::model::apply_relative_note(resolved_note, song.samples[cs.sample_index].relative_note);
    }

    const auto& effect = cell.effect;

    // S9x - настройка канала, работает и без голоса (S91 обычно на пустой
    // строке). Есть S90 и S91 (surround); прочие подкоманды не поддержаны, игра
    // назад (S9F) - декодер идёт только вперёд. До колонки громкости у всех
    // форматов: её SetPanning гасит surround и побеждает.
    if (effect.type == Effect::SoundControl) {
        if (effect.param == 0x00) {
            cs.surround = false;
        } else if (effect.param == 0x01) {
            cs.surround = true;
            cs.pan      = kPanCenter; // центр: у surround панорама не имеет смысла
        }
    }

    // Колонка громкости до колонки эффекта - везде, кроме IT. Без голоса от неё
    // действует только панорама.
    // Голос фиксируется до колонок: гасит его в switch только XM KeyOff, а у XM
    // колонка раньше эффекта.
    const bool voice   = cs.voice_active;
    auto volume_column = [&] {
        if (voice) {
            process_volume_column(cs, cell.volume, song);
        } else {
            apply_volume_column_panning(cs, cell.volume);
        }
    };
    if (!effect_before_volcol) volume_column();
    // Канальные и песенные эффекты и настройки канала - и без голоса.
    apply_channel_effect(ctx, cs, effect);
    // Остальные команды строки - поверх Note-Trigger и только при живом голосе:
    // они двигают звучащий голос или готовят состояние, которое сразу к нему
    // применяется.
    if (voice) {
        switch (effect.type) {
            case Effect::SetVolume:
                cs.volume = effect.param > kVolumeMax ? kVolumeMax : effect.param;
                break;
            case Effect::VolumeSlide:
                row_slide(cs.volume, cs.volume_slide_memory, cs.volume_slide_active, effect, fine_in_param, down_priority, effect_before_volcol, true);
                break;
            case Effect::PortaUp:
            case Effect::PortaDown: {
                const bool is_up = effect.type == Effect::PortaUp;
                // Amiga - param как есть (период); Linear - param*4 (1/16 полутона).
                // porta_memory хранит уже отмасштабированную скорость - её же берут
                // ветка Fine и TonePorta.
                uint16_t param = effect.param;
                if (fine_in_param && effect.rate == SlideRate::PerTick) {
                    uint8_t raw = effect.param;
                    if (apply_raw_fine_porta(cs, raw, is_up, amiga_pitch, amiga_limits)) break;
                    param = raw; // обычный слайд: сырой байт из памяти, дальше общий путь
                }
                if (xm_fine_memory && effect.rate != SlideRate::PerTick) {
                    // XM E1x/E2x и X1x/X2x, как FT2 и OpenMPT: своя память у каждой
                    // пары, по нибблу на направление, с 1xx/2xx/3xx не общая;
                    // сверхтонкий вчетверо мельче тонкого. В модели Amiga
                    // сверхтонкий шаг округляется до целого периода, X11 не сдвигает.
                    const bool extra = effect.rate == SlideRate::ExtraFine;
                    uint8_t& memory  = extra ? cs.xm_extra_fine_porta_memory : cs.xm_fine_porta_memory;
                    uint8_t amount   = static_cast<uint8_t>(effect.param & 0x0fu);
                    if (amount != 0) {
                        memory = is_up ? static_cast<uint8_t>((memory & 0x0fu) | (amount << 4)) : static_cast<uint8_t>((memory & 0xf0u) | amount);
                    } else {
                        amount = is_up ? static_cast<uint8_t>(memory >> 4) : static_cast<uint8_t>(memory & 0x0fu);
                    }
                    if (amount != 0) {
                        const int32_t step = fine_porta_step(amount, extra, amiga_pitch);
                        porta_step(cs, porta_signed(step, is_up, amiga_pitch), amiga_pitch, amiga_limits);
                    }
                    break;
                }
                if (param == 0) {
                    // "00 повторяет последний ненулевой"; память общая, раздельная
                    // память FT2 (kQuirkXmFt2SeparatePortaMemory) не учтена.
                    param = cs.porta_memory;
                } else {
                    param           = porta_units(param, amiga_pitch);
                    cs.porta_memory = param;
                }
                const int16_t signed_delta = porta_signed(param, is_up, amiga_pitch);
                if (effect.rate == SlideRate::PerTick) {
                    cs.porta_delta  = signed_delta;
                    cs.porta_active = true; // на тиках 1..speed-1
                } else {
                    // Fine (MOD E1x/E2x) - разово на тике 0.
                    porta_step(cs, signed_delta, amiga_pitch, amiga_limits);
                }
                break;
            }
            case Effect::TonePorta:
                // "00 повторяет последнюю ненулевую скорость" - та же память, что у
                // PortaUp/PortaDown, уже в шкале модели (param*4 в Linear: TonePortamento
                // тоже умножает на 4).
                if (effect.param != 0) {
                    cs.porta_memory = porta_units(effect.param, amiga_pitch);
                    if (gxx_shares_porta_memory) {
                        cs.porta_raw_memory = effect.param;
                    }
                }
                cs.tone_porta_active = true; // на тиках 1..speed-1
                cs.glissando_porta   = true;
                break;
            case Effect::TonePortaVolSlide:
                // Продолжить текущий TonePorta (память и цель не трогаются, param отдан
                // под VolumeSlide) плюс VolumeSlide тем же param - как VibratoVolSlide.
                cs.tone_porta_active = true;
                cs.glissando_porta   = true;
                // Слайд - тот же, что Dxy, как у OpenMPT; настоящий ST3 тонкий вариант
                // здесь не играет.
                row_slide(cs.volume, cs.volume_slide_memory, cs.volume_slide_active, effect, fine_in_param, down_priority, effect_before_volcol,
                          fine_combined_slides);
                break;
            case Effect::Vibrato:
            case Effect::FineVibrato: {
                // Память раздельная по ниблам: скорость и глубина хранят своё последнее
                // ненулевое значение независимо. Vibrato:
                // depth<<2; FineVibrato: depth без сдвига.
                const uint8_t speed_nibble = static_cast<uint8_t>(effect.param >> 4);
                const uint8_t depth_nibble = static_cast<uint8_t>(effect.param & 0x0fu);
                if (speed_nibble != 0) cs.vibrato_speed = speed_nibble;
                if (depth_nibble != 0) {
                    cs.vibrato_depth = effect.type == Effect::Vibrato ? static_cast<uint8_t>(depth_nibble << 2) : depth_nibble;
                }
                cs.vibrato_active = true; // pitch_offset считается в apply_continuous_effects каждый тик, включая тик 0
                break;
            }
            case Effect::VibratoVolSlide:
                // Продолжить текущее вибрато (память и фаза не трогаются, param отдан под
                // VolumeSlide) плюс VolumeSlide тем же param с общей памятью
                // volume_slide_memory.
                cs.vibrato_active = true;
                // Слайд - тот же, что Dxy, как у OpenMPT; настоящий ST3 тонкий вариант
                // здесь не играет.
                row_slide(cs.volume, cs.volume_slide_memory, cs.volume_slide_active, effect, fine_in_param, down_priority, effect_before_volcol,
                          fine_combined_slides);
                break;
            case Effect::Arpeggio:
                // S3M и IT: J00 повторяет прошлое арпеджио, как у OpenMPT (у MOD и XM
                // 000 до диспетчера не доходит).
                if (!(fine_in_param && effect.param == 0)) {
                    cs.arpeggio_x = static_cast<uint8_t>(effect.param >> 4);
                    cs.arpeggio_y = static_cast<uint8_t>(effect.param & 0x0fu);
                }
                // pitch_offset считается в apply_continuous_effects каждый тик, включая тик 0.
                cs.arpeggio_active = true;
                if (amiga_pitch) {
                    // Периоды нот арпеджио - с finetune канала, как у Note-Trigger и
                    // OpenMPT: иначе интервалы шире или уже на finetune сэмпла.
                    const int8_t finetune = effective_finetune(cs, song.samples[cs.sample_index].finetune);
                    const uint8_t base    = cs.effective_note;
                    cs.arpeggio_period_x  = amiga_period_with_finetune(static_cast<uint8_t>(base + cs.arpeggio_x > 119 ? 119 : base + cs.arpeggio_x), finetune);
                    cs.arpeggio_period_y  = amiga_period_with_finetune(static_cast<uint8_t>(base + cs.arpeggio_y > 119 ? 119 : base + cs.arpeggio_y), finetune);
                }
                break;
            case Effect::Tremolo: {
                // Память раздельная по ниблам, как у Vibrato; от модели частоты не
                // зависит.
                const uint8_t speed_nibble = static_cast<uint8_t>(effect.param >> 4);
                const uint8_t depth_nibble = static_cast<uint8_t>(effect.param & 0x0fu);
                if (speed_nibble != 0) cs.tremolo_speed = speed_nibble;
                if (depth_nibble != 0) cs.tremolo_depth = depth_nibble; // без <<2, в отличие от вибрато
                // volume_offset считается в apply_continuous_effects каждый тик, включая тик 0.
                cs.tremolo_active = true;
                break;
            }
            case Effect::Tremor: {
                // Память - целый байт: param==0 повторяет весь прошлый байт, ненулевой
                // перезаписывает оба нибла разом.
                uint8_t param       = recall(effect.param, cs.tremor_memory);
                const uint8_t on    = static_cast<uint8_t>(param >> 4);
                const uint8_t off   = static_cast<uint8_t>(param & 0x0fu);
                cs.tremor_on_ticks  = on == 0 ? 1 : on; // 0 трактуется как 1, как в libxmp
                cs.tremor_off_ticks = off == 0 ? 1 : off;
                cs.tremor_active    = true; // tremor_muted считается в apply_continuous_effects каждый тик, включая тик 0
                break;
            }
            case Effect::SetReverbSend:
                // Разово и на весь канал, как панорама: значение держится,
                // пока не переуказано.
                cs.reverb_send = effect.param > 127 ? 127 : effect.param;
                break;

            case Effect::SetPitchOffset: {
                // Абсолютный бенд MIDI. Разово на строке: значение держится до
                // следующего, поэтому сброса на строке без эффекта нет - в отличие от
                // pitch_offset вибрато.
                const int32_t shift = 2 * (static_cast<int32_t>(effect.rate) & 3); // x1 x4 x16 x64 по SlideRate
                cs.bend_target      = static_cast<int16_t>((static_cast<int32_t>(effect.param) - 128) * (1 << shift));
                // Нота, взятая этой строкой, начинается на бенде своей ячейки: слот
                // мог достаться от другого канала MIDI с чужим бендом.
                if (cs.triggered_this_row) {
                    cs.bend_offset = cs.bend_target;
                    cs.bend_step   = 0;
                    break;
                }
                // Подход примерно за строку: шаг - (цель - текущее) / speed с отбрасыванием
                // остатка, последний шаг прижимается к цели. При speed == 1 это прыжок.
                // Первый тик уже двигает, поэтому деление на speed, а не на speed-1.
                const int32_t steps = ctx->ps->speed > 0 ? ctx->ps->speed : 1;
                cs.bend_step        = static_cast<int16_t>((cs.bend_target - cs.bend_offset) / steps);
                if (cs.bend_step == 0) cs.bend_offset = cs.bend_target; // ближе шага - сразу на цель
                break;
            }
            case Effect::PanningSlide: {
                uint8_t param = recall(effect.param, cs.pan_slide_memory);
                if (fine_in_param && split_fine_slide(param)) {
                    // PFx - тонко вправо, PxF - влево, разово на тике 0.
                    apply_bounded_slide_delta(cs.pan, pan_slide_param(param, song.quirks), down_priority);
                    break;
                }
                cs.pan_slide_step   = pan_slide_param(param, song.quirks);
                cs.pan_slide_active = true; // применяется на тиках 1..speed-1
                break;
            }
            case Effect::Panbrello: {
                // Память раздельная по ниблам, как у Vibrato/Tremolo; depth<<4.
                const uint8_t speed_nibble = static_cast<uint8_t>(effect.param >> 4);
                const uint8_t depth_nibble = static_cast<uint8_t>(effect.param & 0x0fu);
                if (speed_nibble != 0) cs.panbrello_speed = speed_nibble;
                if (depth_nibble != 0) cs.panbrello_depth = static_cast<uint8_t>(depth_nibble << 4);
                cs.panbrello_active = true; // pan_offset считается в apply_continuous_effects каждый тик, включая тик 0
                break;
            }
            case Effect::SampleOffset: {
                // Действует только вместе с настоящим Note-Trigger этой строки: без ноты
                // cs.triggered_this_row остаётся false, и TrackerEngine команду не читает.
                // Память обновляется в любом случае; обновление только по факту
                // срабатывания (FT2, kQuirkXmFt2OffsetMemoryOnActivate) не реализовано.
                uint8_t param = recall(effect.param, cs.sample_offset_memory);
                // sample_offset_high (Effect::HighOffset) - постоянная память, построчно
                // не сбрасывается.
                cs.trigger_sample_offset = (static_cast<uint32_t>(cs.sample_offset_high) << 16) | (static_cast<uint32_t>(param) << 8);
                break;
            }
            case Effect::Retrigger: {
                // MOD E9x даёт param 0..15 (только младший нибл) - верхний нибл, тип
                // модификатора громкости, выходит нулём, что для E9x и верно: модификатора
                // у него нет.
                uint8_t param = effect.param;
                // IT и S3M, как у OpenMPT: Q00 повторяет прошлый параметр.
                if (fine_in_param) param = recall(param, cs.retrig_memory);
                cs.retrig_type         = static_cast<uint8_t>(param >> 4);
                const uint8_t interval = static_cast<uint8_t>(param & 0x0fu);
                // 0 трактуется как 1, бесконечного интервала не бывает.
                cs.retrig_interval = interval == 0 ? 1 : interval;
                cs.retrig_active   = true; // тики 1..speed-1 считает apply_continuous_effects
                // MOD и XM: отсчёт заново с каждой строки, тик 0 не считается. IT и
                // S3M: счётчик переходит через строки и сбрасывается нотой, тик 0
                // строки без ноты тоже считается.
                if (!fine_in_param || cs.triggered_this_row) {
                    cs.retrig_counter = 0;
                } else if (cs.voice_active && ++cs.retrig_counter >= cs.retrig_interval) {
                    cs.retrig_counter = 0;
                    cs.retrig_pending = true;
                    apply_retrigger_volume(cs);
                }
                break;
            }
            case Effect::RetriggerXm: {
                // XM Rxy, как FT2 и OpenMPT: нулевой нибл берётся из памяти, счётчик
                // переходит через строки. На тике 0 номер инструмента (с нотой или
                // без) начинает счёт с 1, громкость в колонке пропускает тик, нота на
                // строке не ретриггерит. retrig_counter - счёт FT2 минус один: так его
                // ведёт общий путь тиков 1..speed-1. Громкость ретриггера на строке с
                // громкостью в колонке у FT2 не меняется - здесь меняется.
                uint8_t param = effect.param;
                if ((param & 0xf0u) == 0) param = static_cast<uint8_t>(param | (cs.retrig_memory & 0xf0u));
                if ((param & 0x0fu) == 0) param = static_cast<uint8_t>(param | (cs.retrig_memory & 0x0fu));
                cs.retrig_memory    = param;
                cs.retrig_type      = static_cast<uint8_t>(param >> 4);
                const uint8_t speed = static_cast<uint8_t>(param & 0x0fu);
                cs.retrig_interval  = speed == 0 ? 1 : speed;
                cs.retrig_active    = true;
                uint32_t count      = static_cast<uint32_t>(cs.retrig_counter) + 1;
                if (cell.instrument != 0 && (cell.note == soundsinth::model::kNoteNone || is_real_note(cell.note))) count = 1;
                if (!(cell.volume.type == VolumeColumnType::SetVolume && cell.volume.param != 0)) {
                    if (count >= cs.retrig_interval && !is_real_note(cell.note) && cs.voice_active) {
                        count             = 0;
                        cs.retrig_pending = true;
                        apply_retrigger_volume(cs);
                    }
                    ++count;
                }
                cs.retrig_counter = static_cast<uint8_t>(count - 1);
                break;
            }
            case Effect::NoteCut:
                // Та же инфраструктура счётчика, что у Retrigger, с меткой
                // kNoteCutRetrigType - как в libxmp. Параметр - число тиков до среза, 0
                // трактуется как 1.
                cs.retrig_type     = kNoteCutRetrigType;
                cs.retrig_interval = effect.param == 0 ? 1 : effect.param;
                cs.retrig_counter  = 0;
                cs.retrig_active   = true;
                break;
            case Effect::KeyOff:
                // Тот же путь, что Note-Off через колонку ноты вне правил IT.
                key_off(cs);
                break;
            case Effect::SetEnvelopePosition:
                // XM Lxx: позиция в тиках сразу у огибающих громкости и панорамы.
                // Ошибка FT2 "огибающая панорамы обновляется, только если у громкости
                // стоит sustain" не воспроизводится. Новую позицию подхватывает следующий
                // apply_continuous_effects.
                cs.envelope_tick     = effect.param;
                cs.pan_envelope_tick = effect.param;
                break;
            case Effect::SetFinetune: {
                // param - знаковый нибл -8..7 (как байт finetune у MOD-инструмента),
                // приходит замаскированным в 0..15. *16 переводит в шкалу 128 единиц на
                // полутон (SampleDescriptor::finetune), как у libxmp.
                const int8_t signed_nibble  = effect.param > 7 ? static_cast<int8_t>(effect.param) - 16 : static_cast<int8_t>(effect.param);
                cs.finetune_override        = static_cast<int8_t>(signed_nibble * 16);
                cs.finetune_override_active = true;
                break;
            }
            default:
                break; // канальные - в apply_channel_effect, прочие не реализованы
        }
    }
    // IT: после колонки эффекта.
    if (effect_before_volcol) volume_column();
    if (!voice) return;

    // SetFinetune применяется после switch, чтобы S2x/E5x на строке с
    // Note-Trigger подействовал на этот же триггер. Период и linear_pitch выше
    // посчитаны без finetune, здесь досчитывается непрерывная поправка, если
    // эффективный finetune (подмена или свой у сэмпла) ненулевой; при нуле
    // значения остаются табличными побитово.
    if (cs.triggered_this_row) {
        const int8_t finetune = effective_finetune(cs, song.samples[cs.sample_index].finetune);
        if (finetune != 0) {
            if (amiga_pitch) {
                // period *= 2^(-finetune/128/12) - та же непрерывная формула, что у
                // libxmp в модели Amiga: период обратно пропорционален
                // частоте, поэтому степень отрицательная.
                const double fine   = finetune;
                const double factor = std::pow(2.0, -fine / 128.0 / 12.0);
                cs.period           = static_cast<uint16_t>(cs.period * factor + 0.5);
            } else {
                // linear_pitch - в 1/64 полутона, finetune - в 1/128, отсюда *64/128 = /2.
                cs.linear_pitch += (static_cast<int32_t>(finetune) * kLinearAmountUnitsPerSemitone) / 128;
            }
        }
    }
    // Цель TonePorta - с finetune канала, как Note-Trigger: иначе нота после
    // портаменто стоит расстроенной до следующей ноты.
    if (porta_target_note >= 0) {
        const int8_t finetune = effective_finetune(cs, song.samples[cs.sample_index].finetune);
        const uint8_t note    = static_cast<uint8_t>(porta_target_note);
        if (amiga_pitch) {
            cs.tone_porta_target = amiga_period_with_finetune(note, finetune);
        } else {
            const int32_t reference     = linear_reference_note(song.quirks);
            cs.linear_tone_porta_target = (note - reference) * kLinearAmountUnitsPerSemitone + (finetune * kLinearAmountUnitsPerSemitone) / 128;
        }
    }
}

// Доигрыш отложенной ячейки: сброс строки канала и ячейка. Во флеше:
// dispatch_delayed_notes в SRAM зовёт её одним вызовом.
SOUNDSINTH_NOINLINE void dispatch_delayed_cell(DispatchContext* ctx, uint8_t ch) {
    ChannelState& cs = ctx->channels[ch];
    begin_row(cs);
    dispatch_cell(ctx, ch, cs.delayed_cell);
}

} // namespace

void dispatch_row_effects(DispatchContext* ctx, const PatternCell* cells, uint8_t channel_count) {
    const QuirkFlags quirks = ctx->song->quirks;
    // S3M/IT прячут тонкий вариант слайда в параметре, у IT эффект раньше
    // колонки громкости - по ним разбирается NoteDelay с нулём.
    const bool fine_in_param            = (quirks & soundsinth::model::kQuirkFineSlideInParam) != 0;
    const bool effect_before_volcol     = (quirks & soundsinth::model::kQuirkItEffectBeforeVolColumn) != 0;
    ctx->ps->global_volume_slide_active = false;
    for (uint8_t ch = 0; ch < channel_count; ++ch) {
        ChannelState& cs = ctx->channels[ch];
        // NoteDelay (MOD EDx, S3M/IT SDx, XM EDx) откладывает всю ячейку до
        // указанного тика строки. param==0 у MOD и XM - без задержки,
        // обрабатывается сразу; у IT SD0 - как SD1, ST3 ячейку с SD0 не играет
        // вовсе, как у OpenMPT.
        cs.delayed_cell_tick = 0; // отменяет отложенную ячейку прошлой строки - она не переживает границу строки
        const PatternCell* cell = &cells[ch];
        if (cell->effect.type == Effect::NoteDelay) {
            uint8_t delay = cell->effect.param;
            if (delay == 0 && fine_in_param) {
                if (effect_before_volcol) {
                    delay = 1;
                } else {
                    cell = &kEmptyCell;
                }
            }
            if (delay != 0) {
                cs.delayed_cell      = *cell;
                cs.delayed_cell_tick = delay;
                continue; // строка этого канала - позже, в dispatch_delayed_notes
            }
        }
        begin_row(cs);
        dispatch_cell(ctx, ch, *cell);
    }
}

uint16_t glissando_amiga_period(const ChannelState& cs, int8_t sample_finetune, bool nearest) {
    const int8_t finetune = effective_finetune(cs, sample_finetune);
    if (finetune == 0) return soundsinth::model::amiga_snap_period(cs.period, nearest);
    // Сетка нот сдвинута на finetune: период ноты n - round(P_n * factor), как
    // на Note-Trigger и у цели TonePorta. Поиск прямо по ней: обратное деление
    // на множитель на границе давало период на 1 меньше и ноту на полутон
    // выше. Граница как у amiga_snap_period: nearest - среднее геометрическое
    // соседних периодов, иначе первая нота не ниже звучащей высоты.
    const float fine   = finetune;
    const float factor = std::exp2(-fine / 1536.0f);
    auto note_period   = [factor](uint32_t n) -> uint64_t {
        const uint16_t period = soundsinth::model::amiga_note_to_period(static_cast<uint8_t>(n > 119 ? 119 : n));
        return static_cast<uint16_t>(period * factor + 0.5f);
    };
    const uint64_t p2 = static_cast<uint64_t>(cs.period) * cs.period;
    uint32_t lo = 0, count = 120;
    while (count > 0) {
        const uint32_t step = count / 2, mid = lo + step;
        const uint64_t pm = note_period(mid);
        const bool below  = nearest ? pm * note_period(mid + 1) <= p2 : pm <= cs.period;
        if (!below) {
            lo     = mid + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return static_cast<uint16_t>(note_period(lo));
}

int32_t glissando_linear_pitch(const ChannelState& cs, int8_t sample_finetune, bool nearest) {
    const int8_t finetune   = effective_finetune(cs, sample_finetune);
    constexpr int32_t kUnit = kLinearAmountUnitsPerSemitone;
    const int32_t offset    = (finetune * kUnit) / 128; // та же формула, что на Note-Trigger
    // У FT2 граница - полтона ниже ноты. Деление вниз и для отрицательных.
    const int32_t x    = cs.linear_pitch - offset + (nearest ? kUnit / 2 - 1 : kUnit - 1);
    const int32_t note = x >= 0 ? x / kUnit : -((-x + kUnit - 1) / kUnit);
    return note * kUnit + offset;
}

void SOUNDSINTH_HOT_PATH(dispatch_delayed_notes)(DispatchContext* ctx, uint16_t tick_in_row, uint8_t channel_count) {
    for (uint8_t ch = 0; ch < channel_count; ++ch) {
        ChannelState& cs = ctx->channels[ch];
        if (cs.delayed_cell_tick != 0 && cs.delayed_cell_tick == tick_in_row) {
            // Потреблена: на повторах строки PatternDelay tick_in_row снова идёт с 0
            // без новой строки, и ячейка сыграла бы ещё раз.
            cs.delayed_cell_tick = 0;
            dispatch_delayed_cell(ctx, ch);
        }
    }
}

// Вызывается каждый тик: Vibrato, Tremolo, Tremor, Panbrello, слайды,
// огибающие - поэтому в SRAM. dispatch_row_effects (раз в speed
// тиков и крупнее) не помечен: SRAM на всё не хватает, помечается только
// горячее.
void SOUNDSINTH_HOT_PATH(apply_continuous_effects)(PlayState& ps, ChannelState* channels, uint8_t channel_count, QuirkFlags quirks,
                                                   FrequencyModel frequency_model, uint32_t envelope_time_step_q8, bool tick_slides) {
    const bool amiga_pitch = frequency_model == FrequencyModel::Amiga;
    // Vibrato/Arpeggio/Tremolo/Tremor/Panbrello, бенд и огибающие - исключение
    // из правила "слайды с тика 1": работают и на тике 0 (особый случай
    // ProTracker на первом тике ноты не воспроизводится). Отдельный проход до
    // общего return ниже. Фаза вибрато и тремоло на тике 0 стоит у MOD, S3M, XM и IT
    // с Old Effects, как у OpenMPT и libxmp; IT и .mid двигают её каждый тик.
    const bool it_lfo = (quirks & soundsinth::model::kQuirkItVibratoTable) != 0;
    const bool lfo_every_tick =
        (it_lfo && (quirks & soundsinth::model::kQuirkItOldEffects) == 0) || (quirks & soundsinth::model::kQuirkFadeoutExponential) != 0;
    const bool lfo_advance    = ps.tick_in_row != 0 || lfo_every_tick;
    const int32_t vibrato_div = vibrato_divisor(quirks, amiga_pitch);
    for (uint8_t ch = 0; ch < channel_count; ++ch) {
        ChannelState& cs = channels[ch];
        // Бенд плавно доходит до цели.
        if (cs.bend_offset != cs.bend_target) {
            cs.bend_offset += cs.bend_step;
            const bool up   = cs.bend_step > 0;
            if ((up && cs.bend_offset >= cs.bend_target) || (!up && cs.bend_offset <= cs.bend_target)) {
                cs.bend_offset = cs.bend_target;
            }
        }
        // Arpeggio (колонка эффекта) и вибрато (эффект или колонка громкости:
        // XM Vx, IT hx) на одном тике складываются, как у OpenMPT. Оба -
        // смещения от period/linear_pitch; без обоих pitch_offset 0.
        if (cs.voice_active) {
            int32_t offset = 0;
            if (cs.arpeggio_active) {
                offset = amiga_pitch ? arpeggio_offset_amiga(cs, ps.tick_in_row) : arpeggio_offset_linear(cs, ps.tick_in_row);
            }
            // Вибрато - временное отклонение: period и linear_pitch не
            // трогаются; depth уже несёт <<2 из разбора эффекта.
            if (cs.vibrato_active) {
                const int32_t raw  = lfo_step(cs.vibrato_waveform, cs.vibrato_phase, cs.vibrato_speed, it_lfo, lfo_advance) * cs.vibrato_depth;
                offset            += raw / vibrato_div;
            }
            cs.pitch_offset = offset;
        }
        if (cs.voice_active && cs.tremolo_active) {
            apply_tremolo_tick(cs, it_lfo, lfo_advance);
        } else if (cs.voice_active) {
            cs.volume_offset = 0; // тремоло нет - громкость равна чистому volume
        }
        if (cs.voice_active && cs.tremor_active) {
            apply_tremor_tick(cs);
        } else if (cs.voice_active) {
            cs.tremor_muted = false; // tremor нет - канал не глушится
        }
        if (cs.voice_active && cs.panbrello_active) {
            apply_panbrello_tick(cs);
        } else if (cs.voice_active) {
            cs.pan_offset = 0; // панбрелло нет - панорама равна чистому pan с огибающей
        }
        // Огибающие и затухание - общая с фоновыми хвостами NNA функция.
        advance_envelope_and_fadeout(cs, amiga_pitch, quirks, envelope_time_step_q8);
    }

    if (!tick_slides) return; // остальные слайды стартуют с тика 1 - тик 0 уже отработан dispatch_row_effects
    const bool down_priority = (quirks & soundsinth::model::kQuirkS3mVolSlideDownPriority) != 0;

    // GlobalVolumeSlide - песенного уровня, вне цикла по каналам, раз за тик.
    if (ps.global_volume_slide_active) {
        apply_global_volume_step(ps.global_volume, ps.global_volume_slide_step);
    }

    for (uint8_t ch = 0; ch < channel_count; ++ch) {
        ChannelState& cs = channels[ch];
        if (cs.voice_active && cs.volume_slide_active) {
            apply_bounded_slide_delta(cs.volume, cs.volume_slide_memory, down_priority);
        }
        if (cs.voice_active && cs.porta_active) {
            porta_step(cs, cs.porta_delta, amiga_pitch, (quirks & soundsinth::model::kQuirkAmigaLimits) != 0);
        }
        if (cs.voice_active && cs.tone_porta_active) {
            if (amiga_pitch) {
                apply_tone_porta(cs);
            } else {
                apply_tone_porta_linear(cs);
            }
        }
        if (cs.voice_active && cs.pan_slide_active) {
            apply_bounded_slide_delta(cs.pan, cs.pan_slide_step, down_priority);
        }
        if (cs.voice_active && cs.channel_volume_slide_active) {
            apply_bounded_slide_delta(cs.channel_volume, cs.channel_volume_slide_memory, down_priority);
        }
        if (cs.voice_active && cs.retrig_active) {
            ++cs.retrig_counter;
            if (cs.retrig_counter >= cs.retrig_interval) {
                cs.retrig_counter = 0;
                if (cs.retrig_type == kNoteCutRetrigType) {
                    // NoteCut - разово: retrig_active гасится сразу, а не на следующей строке,
                    // как у Retrigger. Та же остановка, что у kNoteCut в колонке ноты
                    // (stop_voice_pending), но по счётчику тиков.
                    stop_voice(cs);
                    cs.retrig_active = false;
                } else {
                    // TrackerEngine ретриггерит Voice (offset 0, тот же сэмпл и нота) и гасит флаг.
                    cs.retrig_pending = true;
                    apply_retrigger_volume(cs);
                }
            }
        }
    }
}

} // namespace soundsinth::engine
