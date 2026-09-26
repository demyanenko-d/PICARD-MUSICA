#pragma once

// Формат банка инструментов .ssb - один заголовок на сборщик банков
// sf2bake и прошивку.
//
// Банк - .sf2, конвертированный на ПК в готовые трекерные инструменты.
// Лежит файлом на карте или во флеше платы отдельным регионом.
//
// Один заголовок на обе стороны: две копии раскладки расходятся молча. У
// каждой записи static_assert на размер.
//
// --- "Читается, а не разбирается" ---
//
// Все записи фиксированного размера и выровнены на 4 байта, ссылки -
// только индексы и смещения, строк переменной длины нет. Чтение -
// приведение указателя к структуре и индексация. Всё, что можно
// посчитать заранее, посчитано при сборке банка, включая откаты на
// отсутствующие банки GS (таблица presets без дырок) и перевод root key и
// подстройки в c5_speed.
//
// Порядок байт - little-endian у обеих сторон.

#include <cstdint>

namespace soundsinth::bank {

// 'S','S','B','1', одна у всех версий. Версия - отдельное поле: так банк
// старой версии в логе отличается от битого файла.
inline constexpr uint32_t kMagic = 0x31425353u;
// Банк другой версии не открывается.
inline constexpr uint16_t kVersion = 5;

// Банки MIDI 0..127 плюс синтетический 128 под ударные; программ 128.
// Таблица пресетов плоская: presets[bank * 128 + program]. Обращение раз
// на файл, при конвертации .mid в Song, поэтому форма простейшая, а не
// компактная.
inline constexpr uint32_t kBankCount = 129;
inline constexpr uint32_t kProgramCount = 128;
inline constexpr uint32_t kPresetSlots = kBankCount * kProgramCount;

// Максимум точек огибающей - как у движка, иначе перенос с потерей.
inline constexpr uint8_t kMaxEnvelopePoints = 25;

// "Нет такой записи" для индексных полей.
inline constexpr uint16_t kNoIndex = 0xffffu;

#pragma pack(push, 4)

// --- Заголовок ---
struct BankHeader {
    uint32_t magic;          // kMagic
    uint16_t version;        // kVersion
    uint16_t rate_cap_hz;    // потолок частоты при сборке банка в Гц (0 - родные частоты), справочно
    uint32_t total_bytes;    // весь блоб
    uint32_t table_crc32;    // от конца заголовка до pcm_offset; проверяется при старте
    uint32_t pcm_crc32;      // по PCM; пишет sf2bake, не проверяется: мегабайты через XIP на каждой загрузке
    uint32_t pcm_offset;     // от начала блоба
    uint32_t pcm_bytes;

    uint32_t presets_offset; // всегда kPresetSlots записей, без дырок
    uint32_t layers_offset;
    uint32_t instruments_offset;
    uint32_t envelopes_offset;
    uint32_t keymap_offset;
    uint32_t samples_offset;
    uint32_t model_offset;   // частоты модели сжатия (BankModel)

    uint16_t layer_count;
    uint16_t instrument_count;
    uint16_t envelope_count;
    uint16_t sample_count;
    uint32_t keymap_count;

    char name[28];           // из INFO/INAM исходного .sf2, для лога
};
static_assert(sizeof(BankHeader) == 96, "BankHeader: раскладка зафиксирована, менять только с версией формата");

// CRC table_crc32 и pcm_crc32: CRC-32 с отражённым полиномом без финальной
// инверсии - на "123456789" 0x340BC6D9, стандартный CRC-32 даёт его
// дополнение. Одна функция у sf2bake и у платы.
inline constexpr uint32_t kBankCrcPoly = 0xedb88320u;
inline uint32_t bank_crc(const uint8_t* p, uint32_t n) {
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (kBankCrcPoly & (0u - (crc & 1u)));
    }
    return crc;
}

// --- Пресет: (банк, программа) -> список слоёв ---
// При сборке банка заполнены все kPresetSlots: если такой вариации в .sf2 нет,
// подставлена программа из банка 0, если нет и её - рояль. На плате
// логики отката нет, ответ всегда валиден.
struct BankPreset {
    uint16_t first_layer;
    // Слоёв в пресете - 15 бит, старший бит - флаг отката. Байта мало: в
    // рояле тяжёлых банков больше тысячи зон.
    uint16_t layer_count_and_flags;
};
inline constexpr uint16_t kPresetFallbackBit = 0x8000u;  // слот заполнен откатом, а не своим пресетом
inline constexpr uint16_t kPresetLayerCountBits = 0x7fffu;
inline uint16_t preset_layer_count(const BankPreset& p) { return p.layer_count_and_flags & kPresetLayerCountBits; }
inline bool preset_is_fallback(const BankPreset& p) { return (p.layer_count_and_flags & kPresetFallbackBit) != 0; }
static_assert(sizeof(BankPreset) == 4, "BankPreset");

// --- Слой: одновременно звучащая часть программы ---
// Слоёв у пресета столько, сколько зон SF2 звучат на одну ноту разом (в
// GeneralUser GS слоисто 170 пресетов из 287). Конвертер поднимает по
// каналу на слой. Диапазона нот здесь нет: он выражен keymap инструмента
// (вне диапазона - kNoSample).
struct BankLayer {
    uint8_t vel_lo;
    uint8_t vel_hi;
    uint16_t instrument;
};
static_assert(sizeof(BankLayer) == 4, "BankLayer");

// --- Инструмент: трекерный Instrument без указателей ---
struct BankInstrument {
    uint8_t default_volume;      // 0..64
    uint8_t global_volume;       // 0..128, 128 - нейтраль; уровень зоны - в записи сэмпла
    uint16_t fadeout_ms;         // 0 - нет затухания; в тики переводит конвертер

    uint16_t default_sample;     // индекс в samples[], kNoIndex если нет
    uint16_t keymap_first;       // индекс в keymap[]
    uint16_t keymap_count;
    uint16_t env_volume;         // индексы в envelopes[] или kNoIndex
    uint16_t env_panning;
    uint16_t env_pitch;
    uint16_t env_filter;

    uint8_t filter_cutoff;       // 0..127, 127 - открыт (16 делений на октаву, у IT 24)
    uint8_t filter_resonance;    // 0..127
    int8_t velocity_to_cutoff;   // сдвиг среза на (velocity-64), в делениях той же шкалы; 0 - не зависит
    uint8_t exclusive_class;     // 0 - нет. Движку не нужен: обрыв выписывает конвертер

    uint8_t nna;                 // soundsinth::model::NewNoteAction
    uint8_t dct;                 // DuplicateCheckType
    uint8_t dca;                 // DuplicateCheckAction
    int8_t instrument_panning;   // -1 - своей нет, иначе 0..64
    uint16_t reserved;           // до ровных 28 байт, запас под следующее поле
};
static_assert(sizeof(BankInstrument) == 28, "BankInstrument");

// --- Огибающая: X в миллисекундах, не в тиках ---
//
// Envelope::points[].tick у движка - тики, а частота тиков зависит от
// темпа файла (сетку под темп подбирает конвертер). Огибающая в тиках -
// это банк под каждый темп. Поэтому банк хранит миллисекунды, конвертер
// переводит: тик = мс * частота_тиков / 1000.
struct BankEnvelopePoint {
    uint16_t ms;
    int16_t value;
};
static_assert(sizeof(BankEnvelopePoint) == 4, "BankEnvelopePoint");

struct BankEnvelope {
    uint8_t flags;           // kEnvEnabledBit, kEnvSustainBit, kEnvLoopBit, kEnvCarryBit
    uint8_t point_count;
    uint8_t sustain_point;
    uint8_t sustain_end;
    uint8_t loop_start;
    uint8_t loop_end;
    uint16_t reserved;
    BankEnvelopePoint points[kMaxEnvelopePoints];
};
static_assert(sizeof(BankEnvelope) == 108, "BankEnvelope");

inline constexpr uint8_t kEnvEnabledBit = 0x01u;
inline constexpr uint8_t kEnvSustainBit = 0x02u;
inline constexpr uint8_t kEnvLoopBit = 0x04u;
inline constexpr uint8_t kEnvCarryBit = 0x08u;

// --- Диапазон keymap: нота -> сэмпл ---
// По смыслу как soundsinth::model::KeymapRange, но sample_index - индекс в
// samples[] банка; конвертер перенумеровывает его в индекс сэмпла песни.
//
// note_offset выражает и scaleTuning=0 (высота не зависит от клавиши):
// зона раскладывается в диапазоны по одной ноте с note_offset = root -
// нота.
struct BankKeymapRange {
    uint8_t start_note;      // первая нота диапазона включительно
    uint8_t note_offset_s8;  // int8 в беззнаковом поле, читается через static_cast<int8_t>
    uint16_t sample_index;   // kNoSample движка (0xFFFF), тот же смысл
};
static_assert(sizeof(BankKeymapRange) == 4, "BankKeymapRange");

inline constexpr uint16_t kNoSample = 0xffffu;

// --- Сэмпл: дескриптор и прогон PCM ---
//
// pcm_bytes - тело и контрольные точки Dpcm8 вместе: точки с новой
// страницы после тела, как у упаковщика сэмплов. Подкачка - выделить
// цепочку страниц PSRAM и распаковать прогон в неё подряд, без разбора.
//
// Полей исходного формата (encoding, signed_pcm, unsupported_codec, file_offset,
// контрольные точки распаковки файла) нет: всё уже распаковано.
struct BankSample {
    uint32_t pcm_offset;        // от начала зоны PCM
    uint32_t pcm_bytes;         // распакованный размер: тело, добивка, контрольные точки
    uint32_t pcm_packed_bytes;  // сжатый размер; у несжатого равен pcm_bytes
    uint32_t length_samples;
    uint32_t loop_start;
    uint32_t loop_end;
    uint32_t c5_speed;       // сюда сложены root key, coarse/fine tune и поправка сэмпла

    uint16_t checkpoint_count;
    uint8_t resident_encoding; // soundsinth::model::ResidentEncoding
    uint8_t flags;             // kSampleLoopBit, kSamplePackedBit

    uint8_t default_volume;    // 0..64
    // 0..255, 64 - единица: initialAttenuation в SF2 бывает усилением (у
    // GeneralUser GS - 98 зон из 21650, до +7 дБ, почти все - тарелки).
    uint8_t global_volume;
    int8_t default_panning;    // -1 - своей нет, иначе 0..64
    int8_t reserved;
};
static_assert(sizeof(BankSample) == 36, "BankSample");

inline constexpr uint8_t kSampleLoopBit = 0x01u;
// Прогон сжат моделью сжатия банка; без флага байты лежат как есть.
inline constexpr uint8_t kSamplePackedBit = 0x02u;

#pragma pack(pop)

} // namespace soundsinth::bank
