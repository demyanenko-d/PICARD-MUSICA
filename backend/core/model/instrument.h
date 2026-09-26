#pragma once

#include <cstdint>

#include "core/model/envelope.h"

namespace soundsinth::model {

// Шкалы величин форматов, общие для загрузчиков и движка. Громкость ноты,
// канала и сэмпла - 0..64; глобальная громкость песни и инструмента IT -
// 0..128; панорама - 0..64, 32 - центр.
inline constexpr uint8_t kVolumeMax = 64;
inline constexpr uint8_t kGlobalVolumeMax = 128;
inline constexpr uint8_t kPanMax = 64;
inline constexpr uint8_t kPanCenter = 32;

// Кодирование PCM сэмпла в исходном файле: как распаковывать данные из
// файла. Резидентный кодек (ResidentEncoding) выбирается отдельно, от
// исходного берётся только разрядность.
enum class SampleEncoding : uint8_t {
    Pcm8,            // MOD всегда, S3M часто; знаковость - SampleDescriptor::signed_pcm
    Pcm16,           // S3M/XM/IT, несжатые
    XmDelta8,        // XM, 8-битная дельта
    XmDelta16,       // XM, 16-битная дельта
    ItCompressed8,   // IT-компрессия, блоки по 0x8000 сэмплов
    ItCompressed16,  // IT-компрессия, блоки по 0x4000 сэмплов
    S3mAdpcm4,       // S3M pack != 0 (ADPCM ModPlug - 4), не распаковывается, сэмпл нерезидентен
};

// Резидентное хранение сэмпла в PSRAM (SampleEncoding - исходный файл).
enum class ResidentEncoding : uint8_t {
    Raw8,   // 8-битные исходники как есть, 1 байт на отсчёт
    Dpcm8,  // 16-битные исходники: 8-битная линейная (не адаптивная) дельта
    Raw16,  // 16-битные исходники как есть, 2 байта на отсчёт, без кодека
};

// Raw16 - 16-битный сэмпл как есть, 2 байта на отсчёт. Dpcm8 последователен:
// на высоком питче выходной отсчёт стоит нескольких распаковок
// (000h_cara_mia.xm: 6.75, 11 голосов - 97% ядра), точки через 256 отсчётов
// не помогают. Выбирается, если трек помещается целиком.

// Сколько байт занимает один отсчёт в резидентном потоке.
inline constexpr uint32_t resident_bytes_per_sample(ResidentEncoding e) {
    return e == ResidentEncoding::Raw16 ? 2u : 1u;
}

// Кодек без состояния: позиция любого отсчёта вычисляется арифметикой,
// без прохода по потоку. Это свойство, а не "равен Raw8", проверяют
// быстрые пути голоса; у Raw16 оно тоже есть.
inline constexpr bool resident_is_direct(ResidentEncoding e) {
    return e == ResidentEncoding::Raw8 || e == ResidentEncoding::Raw16;
}

// Ping-pong петля, развёрнутая в прямую при упаковке: после исходного
// конца петли le дописан обратный проход, хвост сэмпла идёт следом. Стык -
// как у OpenMPT и libxmp, где позиция отражается от конца петли: у XM на
// le, у IT на le - 1/2. Для петли ls..le длиной L = le - ls:
//   Xm - после le s[le-1], s[le-1], s[le-2] .. s[ls+1], L отсчётов, период 2L;
//   It - после le s[le-1], s[le-2] .. s[ls+1], L - 1 отсчётов, период 2L - 1.
// С линейной интерполяцией прямая петля даёт то же, что отражение.
enum class LoopUnroll : uint8_t {
    None,
    Xm,
    It,
};

// Сколько отсчётов дописывает разворот петли длиной loop_len.
inline constexpr uint32_t loop_unroll_extra(LoopUnroll u, uint32_t loop_len) {
    if (u == LoopUnroll::Xm) return loop_len;
    if (u == LoopUnroll::It) return loop_len > 0 ? loop_len - 1u : 0u;
    return 0u;
}

struct SampleDescriptor {
    SampleEncoding encoding = SampleEncoding::Pcm8;
    ResidentEncoding resident_encoding = ResidentEncoding::Raw8; // как закодирован резидентный поток этого сэмпла
    // Сэмпл прорежен 2:1 при упаковке (закодированный размер больше 1 МБ и
    // c5_speed выше 20 кГц). length_samples, loop_* и c5_speed уже после
    // прореживания. При повторной распаковке из file_offset данные надо
    // прогнать через то же прореживание.
    bool decimated = false;
    uint8_t channels = 1;              // 1 у всех загрузчиков: стерео-сэмпл S3M, XM и IT - левый канал
    uint32_t length_samples = 0;        // длина в отсчётах, не в байтах

    // Поля для распаковки сэмпла после разбора заголовков: при прогрессивной
    // загрузке метаданные разбираются сразу, а PCM сэмпла тянется с хоста в
    // свою очередь.

    // Длина до прореживания, в отсчётах. Из length_samples не
    // восстанавливается: длина после прореживания (orig+1)/2, одному значению
    // соответствуют две исходные. Из файла читается ровно исходное число
    // отсчётов. При decimated == false совпадает с length_samples.
    uint32_t source_length_samples = 0;

    // Знаковость PCM в файле: IT - бит 0 байта cvt на сэмпл, S3M - ffi на файл
    // (2 - беззнаковый); MOD и XM знаковые.
    bool signed_pcm = true;

    // ModPlug-ADPCM (XM и IT, только 8-битное моно): не распаковывается,
    // сэмпл нерезидентен.
    bool unsupported_codec = false;
    // Громкость сэмпла (IT: GvL, 0..64), множитель перед
    // Instrument::global_volume, как у OpenMPT; у MOD/S3M/XM 64. В дырке
    // выравнивания после unsupported_codec: рядом с default_volume структура
    // растёт, и bz_ult9.it не влезает в арену.
    uint8_t global_volume = kVolumeMax; // 0..64
    // IT: сжатый сэмпл в варианте 2.15 (бит 2 байта cvt заголовка сэмпла), на
    // сэмпл, как у OpenMPT и libxmp. Занимает последний байт дырки перед
    // loop_start, размер структуры не растёт.
    bool it_is215 = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;              // равен length_samples, если петли нет
    bool loop_enabled = false;
    bool loop_bidirectional = false;    // ping-pong петля (S3M/XM/IT)
    // Петля развёрнута (LoopUnroll): loop_end и length_samples уже
    // развёрнутые, исходный конец - loop_end_before_unroll().
    LoopUnroll loop_unroll = LoopUnroll::None;
    uint32_t file_offset = 0;           // смещение начала PCM в исходном файле
    uint32_t c5_speed = 8363;           // частота воспроизведения на ноте C-5, Гц; у MOD всегда 8363, finetune - в поле finetune
    int8_t relative_note = 0;           // XM: смещение ноты сэмпла от базовой; IT без линейных слайдов: -12; 0 у остальных
    int8_t finetune = 0;                // 128 единиц на полутон, как у XM; MOD -8..7 загрузчик умножает на 16
    uint8_t default_volume = kVolumeMax; // 0..64
    int8_t default_panning = -1;        // -1 - своей панорамы нет (наследуется от инструмента или канала), иначе 0..64
};

// Самая крупная статья арены у трекеров (gk-funky.xm - 778 сэмплов): рост
// должен ловить компилятор.
static_assert(sizeof(SampleDescriptor) == 40, "SampleDescriptor: 40 байт, новое поле обязано лечь в дырку");

// Сколько байт сэмпл занимает в исходном файле. Длина - исходная, до
// прореживания и разворота петли. У сжатых кодировок она в заголовке не
// стоит - ноль: конец файла тогда оценивается снизу, по остальным сэмплам.
// У стереофайла считается один канал, это тоже оценка снизу.
inline constexpr uint32_t sample_source_bytes(const SampleDescriptor& sd) {
    const uint32_t frames = sd.source_length_samples ? sd.source_length_samples : sd.length_samples;
    switch (sd.encoding) {
        case SampleEncoding::Pcm8:
        case SampleEncoding::XmDelta8:
            return frames;
        case SampleEncoding::Pcm16:
        case SampleEncoding::XmDelta16:
            return frames * 2u;
        default: // сжатое и нераспаковываемое
            return 0;
    }
}

// Исходник 16-битный: Raw16 или Dpcm8, 8-битный - Raw8.
inline constexpr bool sample_is_16bit(SampleEncoding e) {
    return e == SampleEncoding::Pcm16 || e == SampleEncoding::XmDelta16 || e == SampleEncoding::ItCompressed16;
}

// Сэмпл, который загрузчик кладёт в PSRAM. Нерезидентны пустые, стерео и
// кодировки, которые загрузчик не распаковывает.
inline constexpr bool sample_is_resident(const SampleDescriptor& sd) {
    return sd.length_samples > 0 && sd.channels == 1 && !sd.unsupported_codec &&
           sd.encoding != SampleEncoding::S3mAdpcm4;
}

// Конец петли до разворота. Смещения Oxx приходят в отсчётах файла, и
// выход за конец петли считается по нему.
inline constexpr uint32_t loop_end_before_unroll(const SampleDescriptor& sd) {
    const uint32_t unrolled = sd.loop_end - sd.loop_start;
    if (sd.loop_unroll == LoopUnroll::Xm) return sd.loop_start + unrolled / 2u;
    if (sd.loop_unroll == LoopUnroll::It) return sd.loop_start + (unrolled + 1u) / 2u;
    return sd.loop_end;
}

inline constexpr uint8_t kNoteMapSize = 120; // нот в шкале трекеров (0..119), общее для keymap XM/IT
// Верхняя нота движка. У трекеров шкала кончается на 119 (форматы выше не
// шлют), у .mid клавиши идут до 127: в архиве ноты 120..127 есть у 0.51%
// файлов, а у одного - четверть всех нот. Банк их играет: диапазоны клавиш
// SF2 доходят до 127.
inline constexpr uint8_t kNoteMax = 127;

// "Сэмпла нет": для нот, которые инструмент не размечает (IT: ноль в
// таблице нота-сэмпл), и для инструментов без сэмплов (XM: num_samples == 0).
//
// Отдельное значение, а не 0: ноль - законный индекс первого сэмпла песни,
// иначе неразмеченная нота играла бы его. Реакция на эти два случая у
// форматов разная (kQuirkCutOnUnmappedNote, kQuirkCutOnEmptySample).
inline constexpr uint16_t kNoSample = 0xffff;

// Нота -> сэмпл для XM, IT и .mid (у MOD/S3M keymap нет); sample_index - в
// Song::samples. Диапазоны [start_note, следующий start_note), а не массив
// на 120 нот: на плотном файле это вчетверо меньше арены. В диапазоне
// постоянны сэмпл и либо note_offset (целевая нота = нота + note_offset),
// либо сама целевая нота (бит kKeymapFixedNote, нота в note_offset) - у
// ударных IT с разметкой всех клавиш на одну ноту. У XM note_offset 0.
// pack(1): 4 байта вместо 6, невыровненный uint16_t Cortex-M33 читает
// аппаратно.
#pragma pack(push, 1)
struct KeymapRange {
    uint8_t start_note = 0;   // первая нота диапазона включительно (биты 0..6); диапазоны по возрастанию покрывают весь [0,120)
    uint16_t sample_index = 0;
    int8_t note_offset = 0;   // target_note = queried_note + note_offset, ограничение - за вызывающим (apply_relative_note)
};
#pragma pack(pop)
static_assert(sizeof(KeymapRange) == 4, "KeymapRange должен паковаться без выравнивания — см. комментарий выше");

// Бит start_note: целевая нота постоянна и лежит в note_offset (0..119).
inline constexpr uint8_t kKeymapFixedNote = 0x80;

inline uint8_t keymap_range_start(const KeymapRange& r) {
    return static_cast<uint8_t>(r.start_note & ~kKeymapFixedNote);
}

// note + SampleDescriptor::relative_note с ограничением 0..119 вместо
// переполнения на патологических смещениях реальных файлов. Единая точка
// для всего кода, считающего питч из ноты (Voice, диспетчер питч-эффектов).
inline uint8_t apply_relative_note(uint8_t note, int8_t relative_note) {
    // Нулевое смещение - нота как есть: так приходит .mid, и клавиши 120..127
    // не упираются в шкалу трекеров. Ограничение 119 остаётся там, где
    // смещение есть: это XM и IT, у них выше 119 нот не бывает.
    if (relative_note == 0) return note;
    int32_t n = static_cast<int32_t>(note) + relative_note;
    if (n < 0) n = 0;
    if (n > kNoteMapSize - 1) n = kNoteMapSize - 1;
    return static_cast<uint8_t>(n);
}

// Целевая нота клавиши note в диапазоне r.
inline uint8_t keymap_range_note(const KeymapRange& r, uint8_t note) {
    return (r.start_note & kKeymapFixedNote) != 0 ? static_cast<uint8_t>(r.note_offset)
                                                   : apply_relative_note(note, r.note_offset);
}

enum class NewNoteAction : uint8_t { Cut = 0, Continue, Off, Fade };            // только IT
enum class DuplicateCheckType : uint8_t { Off = 0, Note, Sample, Instrument };  // только IT
enum class DuplicateCheckAction : uint8_t { Cut = 0, Off, Fade };               // только IT

// Громкость ноты по умолчанию - у сэмпла (SampleDescriptor::default_volume),
// затухание - fadeout_rate: сырые поля файла загрузчик в арену не кладёт.
// Указатели и 32-битные поля первыми, байты в конце - без дырок.
struct Instrument {
    // MOD/S3M (инструмент == сэмпл): note_to_sample_ranges == nullptr, для
    // любой ноты default_sample_index. XM/IT/.mid: note_to_sample_ranges -
    // note_to_sample_range_count диапазонов в арене (KeymapRange), покрывают
    // весь [0,120) без пропусков.
    const KeymapRange* note_to_sample_ranges = nullptr;

    // Огибающие - указатели, nullptr, когда огибающая выключена: у MOD/S3M
    // всегда, у XM у 77.9% инструментов, у IT у 72.6%. Данные выделяются в
    // арене вместе с keymap, только когда нужны. pitch_envelope - только IT, у
    // XM отдельной огибающей питча нет.
    const Envelope* volume_envelope = nullptr;
    const Envelope* panning_envelope = nullptr;
    const Envelope* pitch_envelope = nullptr;
    // Только IT: та же ячейка данных файла, что и pitch_envelope - одна
    // огибающая означает либо высоту, либо срез, решает бит 0x80 её флага. Оба
    // поля одновременно не заполняются.
    const Envelope* filter_envelope = nullptr;

    // Затухание после note-off: убыль ChannelState::fadeout_level (Q16.16,
    // старт 65536) за тик, у .mid (kQuirkFadeoutExponential) - множитель Q16
    // на тик. Каждый загрузчик считает её сам, как libxmp: IT fadeout << 6
    // для нового формата инструмента (cmwt >= 2.00) и << 7 для старого, XM
    // v_fade << 1. 0 - затухания нет: после note-off громкость ведёт только
    // огибающая, а без огибающей громкости (у MOD/S3M всегда) note-off сразу
    // останавливает голос.
    uint32_t fadeout_rate = 0;

    uint16_t default_sample_index = 0;
    uint8_t note_to_sample_range_count = 0;

    uint8_t global_volume = kGlobalVolumeMax; // IT: множитель поверх громкости сэмпла и огибающей, 0..128; у остальных 128 (нейтраль)

    // Сдвиг среза от силы удара, только .mid: срез + velocity_to_cutoff*(v-64)/64,
    // в делениях шкалы среза песни (у .mid 16 на октаву). Модулятор SF2
    // initialFilterFc <- velocity (у GeneralUser GS 248 зон).
    int8_t velocity_to_cutoff = 0;

    // Резонансный фильтр, только IT: сырые поля IFC/IFR инструмента, бит 0x80
    // - значение задано, младшие 7 бит - значение 0..127. Не задано - канал
    // берёт срез 127 и резонанс 0, фильтра нет, пока его не включит
    // огибающая или Zxx.
    uint8_t filter_cutoff = 0;
    uint8_t filter_resonance = 0;

    // NNA/DCT/DCA - только IT. У остальных форматов NNA равен Cut, Duplicate
    // Check выключен: загрузчики MOD/S3M/XM оставляют умолчания, исключений по
    // формату в движке нет.
    NewNoteAction nna = NewNoteAction::Cut;
    DuplicateCheckType dct = DuplicateCheckType::Off;
    DuplicateCheckAction dca = DuplicateCheckAction::Cut;

    // Панорама и Pitch-Pan Separation - только новый формат IT-инструмента
    // (cmwt >= 2.00), у старого этих полей нет. В заголовке инструмента pps и
    // ppc - между fadeout и global_volume, панорама - за ним.
    //
    // instrument_panning - панорама самого инструмента. Панорама сэмпла её
    // перебивает, если заданы обе: сначала инструментная, потом сэмпла поверх
    // (как у OpenMPT). -1 - своей нет, иначе 0..64. В файле
    // у инструмента старший бит байта DfP означает "игнорировать", у сэмпла -
    // "включить"; загрузчик это уже учёл.
    int8_t instrument_panning = -1;
    // pitch_pan_separation (pps, знаковый): сдвиг панорамы на полутон
    // отклонения ноты от pitch_pan_center (ppc). Прибавляется к уже
    // установленной панораме на каждом реальном Note-Trigger, после базовой.
    // 0 - выключено, так почти у всех инструментов.
    int8_t pitch_pan_separation = 0;
    uint8_t pitch_pan_center = 60; // нота 0..119 (шкала PatternCell::note), нейтральная точка pps
};

// Размер зафиксирован: на плате указатель 4 байта, запись обязана
// остаться в 40 байтах, арена считана под них. Рост должен ловить
// компилятор, а не отказ загрузки большого IT на железе.
static_assert(sizeof(void*) != 4 || sizeof(Instrument) == 40,
              "Instrument: 40 байт при 32-битном указателе - новое поле обязано лечь в дырку");

} // namespace soundsinth::model
