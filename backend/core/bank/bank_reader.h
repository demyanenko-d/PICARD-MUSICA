// SPDX-License-Identifier: MIT
#pragma once

// Чтение банка инструментов: на плате ни разбора, ни поиска, только
// приведение указателей к структурам и индексация.
//
// Не бесплатна только подкачка сэмпла: прогон PCM сжат и распаковывается
// в цепочку страниц PSRAM.

#include <cstdint>

#include "platform/compiler.h"
#include "core/bank/bank_codec.h"
#include "core/bank/bank_format.h"
#include "core/memory/psram_store.h"

namespace soundsinth::bank {

// Указатели на таблицы блоба. Блоб читается по месту: во флеше, в PSRAM
// (таблицы банка с карты) или в загруженном файле на ПК.
struct Bank {
    const uint8_t* base               = nullptr;
    const BankHeader* header          = nullptr;
    const BankPreset* presets         = nullptr;
    const BankLayer* layers           = nullptr;
    const BankInstrument* instruments = nullptr;
    const BankEnvelope* envelopes     = nullptr;
    const BankKeymapRange* keymap     = nullptr;
    const BankSample* samples         = nullptr;
    const BankModel* model            = nullptr;
    bool packed                       = false;   // есть сжатые прогоны: подкачке нужна таблица распаковки
    const uint8_t* pcm                = nullptr; // nullptr, если PCM не в памяти
    BankPcmSource pcm_source{};                  // тогда байты берутся отсюда
    // Зовётся между страницами распаковки: прогон распаковывается до сотен
    // миллисекунд, шина и карта хоста всё это время ждут. Не ждать внутри.
    void (*serve)(void* user) = nullptr;
    void* serve_user          = nullptr;

    bool valid() const { return base != nullptr; }
};

// Проверяет сигнатуру, версию и CRC таблиц; CRC по PCM не считается.
//
// tables_only - в памяти только таблицы (банк на карте): проверки те же,
// длина сверяется с концом таблиц, pcm остаётся нулевым. Флаг явный, а не
// выведенный из длины: иначе обрезанный банк во флеше выглядел бы банком
// с карты и отвечал бы тишиной вместо отказа.
bool bank_open(const uint8_t* blob, uint32_t bytes, Bank& out, const char** error_out = nullptr, bool tables_only = false);

// Пресет есть всегда: все 16512 слотов заполнены при сборке банка, с
// откатами на отсутствующие вариации GS.
inline const BankPreset& bank_preset(const Bank& b, uint8_t bank_no, uint8_t program) {
    const uint32_t idx = static_cast<uint32_t>(bank_no) * kProgramCount + program;
    return b.presets[idx < kPresetSlots ? idx : 0];
}

// Сэмпл слоя для ноты или kNoSample, если слой её не покрывает.
// note_offset_out - транспонирование диапазона (им же выражена
// фиксированная высота у перкуссии).
inline uint16_t bank_lookup_note(const Bank& b, const BankInstrument& inst, uint8_t note, int8_t* note_offset_out) {
    const BankKeymapRange* r = b.keymap + inst.keymap_first;
    uint16_t sample          = kNoSample;
    int8_t offset            = 0;
    for (uint16_t i = 0; i < inst.keymap_count; ++i) {
        if (r[i].start_note > note) break;
        sample = r[i].sample_index;
        offset = static_cast<int8_t>(r[i].note_offset_s8);
    }
    if (note_offset_out) *note_offset_out = offset;
    return sample;
}

// Потолок слоёв на ноту. Слоёв у пресета на одну ноту и силу удара (по
// архиву из 126 тысяч .mid, в долях звучащих сочетаний): один слой у 50.7%, два у 41.3%,
// три у 7.2%, четыре у 0.7%, пять у 0.04% - только в расширенных банках GS,
// в GM ни разу. Слои умножаются на голоса, а голоса - самый узкий ресурс.
inline constexpr uint8_t kMaxNoteLayers = 4;

// Для выбора слоя velocity округляется к одной из шести точек по квантилям
// velocity нот библиотеки (770 тысяч нот: диапазон 43..113, медиана 81).
// Иначе разных слоёв файл задевает тем больше, чем больше в нём оттенков (у
// SGM в рояле 1133 слоя на 340 полос), и каждый тянет сэмплы. Громкость - по
// точному значению.
inline constexpr uint8_t kVelBands[6] = {39, 61, 75, 87, 100, 115};

inline uint8_t quantize_velocity(uint8_t vel) {
    uint8_t best  = kVelBands[0];
    int32_t bestd = 1000;
    for (uint8_t b : kVelBands) {
        const int32_t d  = static_cast<int32_t>(vel) - static_cast<int32_t>(b);
        const int32_t ad = d < 0 ? -d : d;
        if (ad < bestd) {
            bestd = ad;
            best  = b;
        }
    }
    return best;
}

// Слой пресета, звучащий на ноте, и его сэмпл банка.
struct NoteLayer {
    uint16_t layer;
    uint16_t bank_sample;
};

// Какие слои пресета звучат на ноте: в полосе силы удара, с сэмплом на эту
// ноту, не больше kMaxNoteLayers. accept(слой, сэмпл) - последнее слово
// вызывающего (номер инструмента, потолок); отказ места не занимает.
//
// Округление может увести силу удара за узкую полосу слоя, и нота не
// найдёт ни одного: сначала округлённое значение, если ни один слой в
// полосу не попал - точное. Одна функция на всех, кто выбирает слои.
template <typename Accept>
SOUNDSINTH_NOINLINE uint32_t select_note_layers(const Bank& b, const BankPreset& preset, uint8_t note, uint8_t velocity, Accept&& accept,
                                                NoteLayer (&out)[kMaxNoteLayers]) {
    uint8_t sel = quantize_velocity(velocity);
    bool any    = false;
    for (uint16_t l = 0; l < preset_layer_count(preset) && !any; ++l) {
        const BankLayer& la = b.layers[preset.first_layer + l];
        any                 = sel >= la.vel_lo && sel <= la.vel_hi;
    }
    if (!any) sel = velocity;
    uint32_t n = 0;
    for (uint16_t l = 0; l < preset_layer_count(preset) && n < kMaxNoteLayers; ++l) {
        const uint16_t li      = static_cast<uint16_t>(preset.first_layer + l);
        const BankLayer& layer = b.layers[li];
        if (sel < layer.vel_lo || sel > layer.vel_hi) continue;
        const uint16_t bs = bank_lookup_note(b, b.instruments[layer.instrument], note, nullptr);
        if (bs == kNoSample) continue;
        if (!accept(li, bs)) continue;
        out[n++] = NoteLayer{li, bs};
    }
    return n;
}

// Кладёт сэмпл банка в PSRAM: выделяет цепочку страниц и распаковывает
// прогон прямо в неё по килобайту. Промежуточного буфера нет, страницы
// лежат не подряд, распаковщик потоковый.
//
// table - таблица распаковки из модели банка (bank_build_decode_table);
// без неё сжатый прогон не читается, несжатый - да.
//
// Возвращает первую страницу цепочки или kPageChainEnd при нехватке
// памяти, отказе чтения (read_failed_out, может быть nullptr) или сжатом
// прогоне без таблицы. checkpoint_first_page_out - страница начала
// контрольных точек Dpcm8 (с новой страницы после тела).
uint16_t bank_make_resident(const Bank& b, const BankDecodeTable* table, uint16_t sample_index, memory::PsramStore& psram, uint16_t* checkpoint_first_page_out,
                            bool* read_failed_out = nullptr);

// Причина отказа, если банк не читается с карты: одна строка на ядро,
// вызывающий сравнивает указатель.
inline constexpr char kBankReadError[] = "bank cannot be read from the card";

} // namespace soundsinth::bank
