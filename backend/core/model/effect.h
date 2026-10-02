// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace soundsinth::model {

// Канонический код эффекта (колонка эффекта ячейки), общий для всех
// форматов. Загрузчик переводит опкод формата в этот список один раз при
// загрузке, дальше движок не знает, из какого формата пришла команда.
// Буквы в комментариях - ориентир происхождения, точное соответствие
// опкодов - в загрузчике формата.
//
// Расширенная семья MOD/XM Exx и S3M/IT Sxx - значения того же enum, а не
// отдельное поле: подкоманды используются только вместе с Effect, а
// лишний байт на ячейку раздул бы словарь каждого паттерна в PSRAM (6 байт
// на уникальную ячейку) и буферы строк. Все значения умещаются в 6 бит.
enum class Effect : uint8_t {
    None = 0,

    // --- Питч ---
    Arpeggio,          // MOD 0xy / S3M,IT Jxy / XM 0xy
    PortaUp,           // MOD 1xx / S3M,IT Fxx / XM 1xx; тонкий и сверхтонкий вариант - SlideRate
    PortaDown,         // MOD 2xx / S3M,IT Exx / XM 2xx
    TonePorta,         // MOD 3xx / S3M,IT Gxx / XM 3xx
    Vibrato,           // MOD 4xy / S3M,IT Hxy / XM 4xy
    TonePortaVolSlide, // MOD 5xy / S3M,IT Lxy / XM 5xy: TonePorta + VolumeSlide на общей памяти портаменто
    VibratoVolSlide,   // MOD 6xy / S3M,IT Kxy / XM 6xy: Vibrato + VolumeSlide
    FineVibrato,       // S3M,IT Uxy: вибрато вчетверо мельче обычного, нет в MOD/XM

    // --- Громкость и панорама ---
    VolumeSlide,        // MOD Axy / S3M,IT Dxy / XM Axy
    SetVolume,          // MOD,XM Cxx; у S3M/IT только колонка громкости (VolumeColumnType::SetVolume)
    SetPanning,         // MOD,XM 8xx / S3M,IT Xxx; у MOD эвристики панорамы (kQuirkModIgnorePanning, kQuirkMod7BitPanning)
    PanningSlide,       // S3M,IT Pxy / XM Pxy
    Panbrello,          // S3M,XM,IT Yxy, нет в MOD
    SetGlobalVolume,    // S3M Vxx / XM Gxx / IT Vxx
    GlobalVolumeSlide,  // XM Hxy / S3M,IT Wxy
    SetChannelVolume,   // S3M,IT Mxx
    ChannelVolumeSlide, // S3M,IT Nxy

    // --- Сэмпл и нота ---
    SampleOffset,        // MOD,XM 9xx / S3M,IT Oxx (старший байт - HighOffset у S3M/IT)
    Retrigger,           // MOD E9x / S3M,IT Qxy / XM E9x: ретриггер с изменением громкости
    RetriggerXm,         // XM Rxy: то же, память по нибблам и счёт FT2
    KeyOff,              // XM Kxx; у MOD/S3M/IT отпускание обычно кодируется нотой (kNoteOff)
    SetEnvelopePosition, // XM Lxx

    // --- Тремоло и тремор ---
    Tremolo, // MOD 7xy / S3M,IT Rxy / XM 7xy
    Tremor,  // S3M,IT Ixy / XM Txy

    // --- Тайминг и структура песни ---
    SetSpeed,     // MOD,XM Fxx(<0x20) / S3M,IT Axx
    SetTempo,     // MOD,XM Fxx(>=0x20) / S3M,IT Txx
    PositionJump, // MOD,S3M,XM,IT Bxx
    PatternBreak, // MOD,XM Dxx / S3M,IT Cxx

    // --- MIDI-макросы (IT, расширение OpenMPT) ---
    SetMidiMacro,    // S3M,XM,IT Zxx
    SmoothMidiMacro, // IT \xx (расширение OpenMPT поверх Zxx)

    // --- Расширенная семья: MOD/XM Exx, S3M/IT Sxx. Набор подкоманд один по
    // смыслу, опкоды у форматов разные ---
    SetFilter,          // MOD,XM E0x: LED-фильтр Amiga; движок не обрабатывает
    GlissandoControl,   // MOD E3x / S3M,IT S1x
    SetVibratoWaveform, // MOD E4x / S3M,IT S3x / XM E4x
    SetFinetune,        // MOD E5x / S3M,IT S2x
    PatternLoop,        // MOD E6x / S3M,IT SBx / XM E6x; поведение зависит от версии трекера (FlowModeFlags)
    SetTremoloWaveform, // MOD E7x / S3M,IT S4x / XM E7x
    RetriggerFunkRepeat, // ProTracker EFx: funk repeat (инверсия петли); загрузчики не выставляют, движок не реализует
    SetPanbrelloWaveform, // S3M,IT S5x
    FinePatternDelay,     // S3M,IT S6x
    // S3M,IT S8x, MOD,XM E8x: панорама 4 битами параметра (0..15), шкала и формула не
    // такие, как у SetPanning (8xx, байт 0..255, в обработчике /4). Как у
    // OpenMPT: панорама 0..256 = (param*256+8)/15.
    SetPanning4Bit,
    SoundControl,       // S3M,IT S9x: surround и стерео
    HighOffset,         // S3M,IT SAx: старший байт для SampleOffset этой или следующих строк
    NoteCut,            // MOD ECx / S3M,IT SCx / XM ECx
    NoteDelay,          // MOD EDx / S3M,IT SDx / XM EDx
    PatternDelay,       // MOD EEx / S3M,IT SEx / XM EEx
    SetActiveMidiMacro, // S3M,IT SFx: выбор макроса Zxx (расширение OpenMPT)

    // Абсолютный питч в 1/64 полутона, только из конвертера MIDI: цель, до
    // которой движок доводит питч за строку, шагом каждый тик, без
    // накопления ошибки портаменто. (param - 128) единиц: +-2 полутона
    // (умолчание GM); до +-24 (RPN0) - множитель шага в свободном поле
    // SlideRate (x1, x4, x16, x64).
    SetPitchOffset,

    // Посыл канала в общий ревербератор, 0..127 в param. Только из
    // конвертера MIDI (CC91). В SF2 reverbEffectsSend - посыл в общий
    // ревербератор, а не эффект на канал.
    SetReverbSend,

    Count // только для static_assert на ширину битового поля, в EffectCommand не хранится
};

// Скорость слайда. В S3M/IT - верхний нибл параметра, в MOD/XM -
// отдельные подкоманды (E1x/E2x/EAx/EBx - тонкие, XM X1x/X2x -
// сверхтонкие). Загрузчик приводит к одному виду, движок исходный байт не
// разбирает.
// У SetPitchOffset те же два бита значат другое - множитель шага бенда
// (x1, x4, x16, x64), и там законно значение 3. Своё имя ему нужно: без
// него приведение к enum уходит за объявленный диапазон, и это видит
// статический анализатор.
enum class SlideRate : uint8_t {
    PerTick = 0, // обычный слайд, каждый тик
    Fine,        // один раз в начале строки
    ExtraFine,   // один раз, шаг мельче (S3M/IT, XM X1x/X2x)
    Widest,      // только SetPitchOffset: самый грубый шаг бенда (x64)
};

// type и rate - один байт (6 + 2 бита), причина - у enum Effect.
inline constexpr uint32_t kEffectTypeBits = 6;

struct EffectCommand {
    // Инициализаторы по умолчанию у битовых полей только с C++20, проект на
    // C++17 - поэтому через конструктор.
    Effect type : kEffectTypeBits;
    SlideRate rate : 2; // значим для слайдов и портаменто, у остальных эффектов свободен
    uint8_t param;      // параметр без верхнего нибла тонкого/сверхтонкого слайда (см. rate)

    EffectCommand() : type(Effect::None), rate(SlideRate::PerTick), param(0) {}
};
static_assert(static_cast<uint32_t>(Effect::Count) <= (1u << kEffectTypeBits), "Effect does not fit the EffectCommand::type field");

// Подкоманды колонки громкости. XM/IT - упакованный байт с несколькими
// диапазонами; S3M - просто громкость; у MOD колонки нет, только
// Effect::SetVolume. Диапазоны байта разбирает загрузчик, здесь только
// смысл.
//
// Битовыми полями не упакована: SetVolume нужны 0..64 (7 бит), с типом (4
// бита на 15 значений) это 11 бит, в байт без потери точности не влезает.
enum class VolumeColumnType : uint8_t {
    None = 0,
    SetVolume, // S3M: единственный смысл поля громкости; XM/IT: нижний диапазон байта
    SlideDown,
    SlideUp,
    FineSlideDown,
    FineSlideUp,
    VibratoSpeed, // только XM, 0xA0-0xAF, отдельная команда от VibratoDepth
    VibratoDepth, // XM 0xB0-0xBF, IT 203-212
    SetPanning,
    PanSlideLeft,
    PanSlideRight,
    TonePorta,
    PortamentoDown, // только IT, отдельно от TonePorta
    PortamentoUp,
    Offset, // только IT, 223-232: быстрый Oxx через колонку громкости
};

struct VolumeColumnCommand {
    VolumeColumnType type = VolumeColumnType::None;
    uint8_t param         = 0;
};

} // namespace soundsinth::model
