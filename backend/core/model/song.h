#pragma once

#include <cstdint>

#include "core/config.h"
#include "core/model/effect.h"
#include "core/model/instrument.h"
#include "core/model/quirks.h"

namespace soundsinth::model {

// Специальные значения PatternCell::note (вместо номера ноты 0..119).
inline constexpr uint8_t kNoteNone = 0xFF;
inline constexpr uint8_t kNoteOff = 0xFE;   // отпускание ноты (XM нота 97, IT "===", S3M "^^"): с огибающей громкости - release, без неё - остановка голоса
inline constexpr uint8_t kNoteFade = 0xFD;  // IT note fade ("~~~")
inline constexpr uint8_t kNoteCut = 0xFC;   // мгновенная остановка голоса без release и fadeout

// Нота запускает голос и требует сэмпла; специальные значения выше - нет.
// Одно правило для движка (effect_dispatch) и планировщика загрузки
// (sample_prefetch): новое специальное значение их не разведёт.
static_assert(kNoteCut < kNoteFade && kNoteFade < kNoteOff && kNoteOff < kNoteNone,
              "специальные значения ноты - от kNoteCut и выше");
inline constexpr bool is_real_note(uint8_t note) { return note < kNoteCut; }

// Номер инструмента первым: 16 бит без дырки выравнивания, ячейка 8 байт.
struct PatternCell {
    uint16_t instrument = 0;  // 0 - нет инструмента, иначе 1-based индекс в Song::instruments
    uint8_t note = kNoteNone;
    VolumeColumnCommand volume;
    EffectCommand effect;
};
static_assert(sizeof(PatternCell) == 8, "PatternCell: 8 байт, номер инструмента 16-битный");

// Скорость и темп по умолчанию (тиков на строку, BPM) и нижний предел
// темпа: Set Tempo меньше 32 - это Set Speed у MOD/XM и слайд темпа у S3M/IT.
inline constexpr uint8_t kDefaultSpeed = 6;
inline constexpr uint16_t kDefaultTempo = 125;
inline constexpr uint16_t kMinTempo = 32;

// Предел каналов паттерна: маска строки у PatternPacker и PatternReader -
// 64 бита. У форматов пределы свои (IT - 6-битный номер канала, S3M - 32),
// не больше этого - static_assert в каждом загрузчике.
inline constexpr uint8_t kMaxPatternChannels = 64;

// Делений шкалы среза фильтра на октаву (Song::filter_units_per_octave).
inline constexpr uint8_t kFilterUnitsIt = 24;
inline constexpr uint8_t kFilterUnitsItExtended = 20;
inline constexpr uint8_t kFilterUnitsMid = 16;

// Предел строк паттерна: XM и IT - до 256, MOD и S3M - 64, .mid - 128.
// XM и IT отвергают больше; у MOD, S3M и .mid число строк постоянное.
inline constexpr uint16_t kMaxPatternRows = 256;

// Паттерн лежит в PSRAM сжатым блоком (маска, таблица смещений, словарь
// ячеек); psram_offset - смещение блока от начала зоны паттернов. Движок
// читает по строке.
struct Pattern {
    uint16_t row_count = 0;
    uint8_t channel_count = 0;
    static constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;
    uint32_t psram_offset = kInvalidOffset;  // невалиден, пока упаковщик не записал паттерн
};

enum class FrequencyModel : uint8_t { Amiga, Linear };

// Специальные значения Song::order[i], аналог маркеров S3M/IT 0xFE/0xFF.
// У MOD/XM таких маркеров нет, представление общее для всех форматов.
inline constexpr uint16_t kOrderEnd = 0xFFFF;   // конец списка воспроизведения
inline constexpr uint16_t kOrderSkip = 0xFFFE;  // пропустить позицию (не паттерн, не воспроизводится)

// Промежуточное представление, в которое загрузчики MOD/S3M/XM/IT/.mid
// переводят файл. Движок работает с ним, не зная формата; различия
// форматов выражены данными (quirks, flow_mode), а не ветками кода.
//
// Структура целиком (массивы Instrument и SampleDescriptor, keymap,
// огибающие) живёт в резидентной арене. PCM сюда не входит: SampleDescriptor
// хранит смещение в файле и параметры, данные доставляет загрузчик
// сэмплов (load_track_sample).
// Паттерны - сжатыми блоками в PSRAM.
struct Song {
    // Времена огибающих и затухания заданы в реальном времени (.mid): загрузчик
    // перевёл миллисекунды банка в тики по default_tempo, и при смене темпа
    // движок продвигает их на default_tempo / текущий темп тика за тик, чтобы
    // атака и релиз не растягивались вместе с темпом. У трекеров - false:
    // огибающие там в тиках самого трека, как у оригинальных трекеров.
    bool envelopes_in_real_time = false;

    // Нужен ли треку общий ревербератор. Ставит только загрузчик MIDI: у
    // трекерных форматов посылов нет, и при false движок не трогает ни шину,
    // ни ревербератор, выход побайтово прежний.
    bool reverb_enabled = false;

    // Нужен ли треку лимитер на выходе шины. Ставит только загрузчик MIDI: у
    // трекерного файла уровень задан файлом и сверен с эталонными плеерами, а
    // у .mid зависит от банка - GeneralUser печётся на 12 дБ тише Timbres of
    // Heaven, и тот же файл на втором банке клипует. При false шина не
    // выполняет ни одной лишней команды.
    bool limiter_enabled = false;

    // Мягкое насыщение на выходе шины вместо жёсткой обрезки
    // (MixBus::set_soft_clip). Ставит только загрузчик MIDI: у трекерных
    // форматов перегруз обрезается, как у эталонных плееров.
    bool soft_clip_enabled = false;

    // Не давать срезу фильтра опускаться ниже основного тона играемой ноты.
    // Ставит только загрузчик MIDI.
    //
    // Срез в SF2 задан абсолютной частотой на весь диапазон инструмента, а
    // keymap покрывает всю клавиатуру: на верхних октавах такой срез глушит
    // саму ноту. Это key tracking фильтра; генератора для него в SF2 нет,
    // поэтому делается на воспроизведении. Срез только поднимается.
    bool filter_follows_note = false;

    // Делений шкалы среза на октаву U: срез 110 * 2^(0.25 + cutoff/U) Гц.
    //   kFilterUnitsIt (24) - IT, 131 Гц..5.1 кГц;
    //   kFilterUnitsItExtended (20) - IT с флагом extendedFilterRange (0x1000),
    //        до ~10 кГц;
    //   kFilterUnitsMid (16) - .mid, до 20 кГц: огибающие пэдов SGM раскрывают
    //        срез до 8.3 кГц.
    uint8_t filter_units_per_octave = kFilterUnitsIt;

    // Фильтр с откликом SF2 (биквад ФНЧ RBJ, filter_compute sf2_response)
    // вместо фильтра IT. Ставит только загрузчик MIDI.
    bool filter_sf2_response = false;

    // Конструктор только ради channel_pan и channel_volume: у массива нет
    // инициализатора, заполняющего все элементы одним ненулевым значением.
    Song() {
        for (auto& p : channel_pan) p = kPanCenter;
        for (auto& v : channel_volume) v = kVolumeMax;
    }

    // --- Общее ---
    char title[32] = {};  // с запасом под самый длинный заголовок (S3M: 28 символов + '\0')
    uint8_t channel_count = 4;
    uint16_t default_speed = kDefaultSpeed; // тиков на строку
    uint16_t default_tempo = kDefaultTempo; // BPM
    // 0..128 (шкала IT). MOD - всегда 128; S3M - GlobalVolume заголовка (0..64)
    // * 2; XM/IT - свои поля, приведённые загрузчиком.
    uint8_t default_global_volume = kGlobalVolumeMax;
    // Режим сведения по тому, чем сделан файл, как у OpenMPT. Original
    // (ModPlug Tracker): сверх Compatible ослабление по числу каналов и выход
    // на 3 бита тише - в 1.5 раза на паре каналов, в 2.94 на 31; без него
    // bz_ult9.it и 034djzjack громче эталона в 1.9-2.9 раза.
    enum class MixLevels : uint8_t { Compatible = 0, Original = 1 };
    MixLevels mix_levels = MixLevels::Compatible;

    // Строки не из PSRAM, а от источника: у .mid их делает конвертер из
    // файла по ходу игры, и хранить упакованные паттерны незачем - они вдвое
    // дороже самого файла. Дескрипторы паттернов при этом остаются: по ним
    // секвенсор ведёт позицию, а стоят они 10 байт на паттерн.
    //
    // nullptr - обычный трек, строки читаются из зоны паттернов.
    // Зовётся из тика секвенсора: строки идут по порядку, назад - только
    // перемоткой, и источник обязан это выдержать.
    using RowFetch = void (*)(void* user, uint16_t pattern_idx, uint16_t row, PatternCell* out, uint8_t channels);
    RowFetch row_fetch = nullptr;
    void* row_fetch_user = nullptr;

    // Закон панорамирования. Linear - сумма усилений 1 (IT, MOD, S3M).
    // Ft2Sqrt - сумма квадратов 1, вправо до конца не доводит, слева 1/16,
    // как FT2 (XM; с Linear XM в 1.43 раза тише эталона). Sqrt - постоянная
    // мощность без квирка: у .mid половины стереопары SF2 - жёстко влево и
    // вправо.
    enum class PanLaw : uint8_t { Linear = 0, Ft2Sqrt = 1, Sqrt = 2 };
    PanLaw pan_law = PanLaw::Linear;

    // Предусиление сэмплов (sample pre-amp, mixing volume), 0..128:
    // статический множитель на все каналы, эффектами не меняется. Автор
    // задаёт его, чтобы плотный многоканальный микс не клипал. IT - mv
    // заголовка, S3M - masterVolume без бита стерео. У MOD и XM поля нет,
    // значение считает загрузчик по правилам OpenMPT (MOD: 256/каналов в
    // пределах 32..128, XM: 48).
    uint8_t sample_preamp = 128;

    // Сколько отсчётов доезжает громкость голоса до новой величины. Ноль -
    // SOUNDSINTH_VOLUME_RAMP_SAMPLES, так у трекерных форматов.
    //
    // У .mid огибающая пересчитывается раз в тик (около 15 мс), а релизы в
    // банках бывают по 30 мс: за тик уровень падает на полсотни децибел, и
    // миллисекундного доезда мало, остаётся щелчок. На басу Blues2.mid худшее
    // падение 58.8 дБ за миллисекунду при доезде 1 мс, 23.7 дБ при 5 мс, у
    // эталонного синтезатора на том же банке 28.2.
    //
    // Трекерам столько нельзя: 5 мс - четверть периода быстрого тремоло, оно
    // смажется. Предел 255: счётчики доезда - байт, больше движок прижимает.
    uint16_t volume_ramp_samples = 0;
    FrequencyModel frequency_model = FrequencyModel::Amiga;

    // Начальная панорама канала, 0..64, 32 - центр (шкала ChannelState::pan).
    // IT: таблица ChnPan[64] заголовка по смещению 0x40. S3M: аппаратная
    // L/R-разводка по типу канала у стерео-файлов, поверх - таблица
    // панорамы каналов (usePanningTable). MOD: поля нет, разводка
    // Paula LRRL - квирк kQuirkModHardwarePanning. XM: поля на уровне песни
    // нет, только default_panning сэмпла. Умолчание 32.
    uint8_t channel_pan[SOUNDSINTH_MAX_VOICES];
    // Начальная громкость канала 0..64 (эффекты Mxx/Nxy меняют её дальше).
    // IT: таблица ChnVol[64] заголовка по смещению 0x80. У остальных
    // форматов поля нет, 64.
    uint8_t channel_volume[SOUNDSINTH_MAX_VOICES];
    // Выключенные каналы, бит на канал: IT - бит 0x80 в ChnPan; S3M не из ST3 -
    // бит 0x80 байта канала (у ST3 ячейки такого канала не хранятся). Эффекты
    // канала идут, голос не запускается, как у OpenMPT. 0 - все каналы звучат.
    uint64_t channel_muted = 0;
    // Каналы в surround с начала трека, бит на канал (IT: ChnPan 100), как S91.
    uint64_t channel_surround = 0;

    // --- Order list ---
    // В арене, ровно order_count элементов; значения - индекс в Song::patterns
    // или kOrderEnd/kOrderSkip.
    const uint16_t* order = nullptr;
    uint16_t order_count = 0;
    uint16_t restart_position = 0;  // индекс в order, куда переходить после kOrderEnd

    // --- Паттерны ---
    Pattern* patterns = nullptr;  // в арене, ровно pattern_count элементов
    uint16_t pattern_count = 0;

    // --- Сэмплы: общий плоский список на Song, не вложенный в Instrument
    // (KeymapRange::sample_index указывает сюда). У IT сэмплы - отдельный
    // список в файле; у XM сэмплы инструмента - непрерывный диапазон здесь. У
    // MOD/S3M (инструмент == сэмпл) instrument_count == sample_count и
    // default_sample_index у Instrument[i] равен i. ---
    SampleDescriptor* samples = nullptr;  // в арене, ровно sample_count элементов
    uint16_t sample_count = 0;

    // --- Инструменты ---
    Instrument* instruments = nullptr;  // в арене, ровно instrument_count элементов
    uint16_t instrument_count = 0;

    // --- Квирки воспроизведения (quirks.h): выставляет загрузчик один раз по
    // сигнатуре и версии трекера, движок версию не определяет ---
    QuirkFlags quirks = 0;
    FlowModeFlags flow_mode = 0;
};

// Канал выключен в заголовке файла (Song::channel_muted).
inline bool channel_is_muted(const Song& song, uint8_t ch) {
    return ((song.channel_muted >> ch) & 1u) != 0;
}

// (инструмент 1-based, нота) -> индекс в Song::samples и нота для питча.
// out_note обычно равна сыгранной, у IT-инструментов с перепривязкой
// клавиш отличается (KeymapRange::note_offset, kKeymapFixedNote). Диапазоны
// отсортированы и покрывают [0,120): линейный поиск последнего с началом <= note;
// диапазонов до 80, вызов раз на Note-Trigger.
//
// Живёт здесь, а не в движке: то же правило нужно планировщику
// предзагрузки (sample_prefetch). Копия одна на проект: разойдись они,
// предзагрузка грузила бы не те сэмплы у инструментов с keymap.
//
// out_unmapped (необязательный) - почему не нашлось: true - инструменту
// нечего дать на эту ноту (нет инструмента, нота вне keymap, kNoSample);
// false - сэмпл назван, но индекс за пределами песни. Форматы реагируют
// по-разному (kQuirkCutOnUnmappedNote, kQuirkCutOnEmptySample).
inline bool resolve_sample_index(const Song& song, uint16_t instrument_1based, uint8_t note, uint16_t* out_index,
                                  uint8_t* out_note, bool* out_unmapped = nullptr) {
    if (out_unmapped != nullptr) *out_unmapped = true;
    if (instrument_1based == 0 || instrument_1based > song.instrument_count) return false;
    const Instrument& ins = song.instruments[instrument_1based - 1];
    uint16_t idx = ins.default_sample_index;
    uint8_t resolved_note = note;
    if (ins.note_to_sample_ranges != nullptr && note <= kNoteMax) {
        const KeymapRange* range = nullptr;
        for (uint8_t r = 0; r < ins.note_to_sample_range_count; ++r) {
            if (keymap_range_start(ins.note_to_sample_ranges[r]) > note) break; // дальше только диапазоны правее note
            range = &ins.note_to_sample_ranges[r];
        }
        if (range != nullptr) {
            idx = range->sample_index;
            resolved_note = keymap_range_note(*range, note);
        }
    }
    if (idx == kNoSample) return false;                 // размечено как "сэмпла нет"
    if (out_unmapped != nullptr) *out_unmapped = false;  // сэмпл назван, дальше вопрос в его пригодности
    if (idx >= song.sample_count) return false;
    *out_index = idx;
    *out_note = resolved_note;
    return true;
}

} // namespace soundsinth::model
