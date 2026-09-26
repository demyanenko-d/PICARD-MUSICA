#pragma once

// Перепаковка распакованного PCM исходного формата в резидентные
// страницы PSRAM. Загрузчик читает источник кусками, поэтому
// SamplePacker - потоковый писатель.
//
// Режимы:
//  - Raw8 - 8-битные исходники как есть, байт на отсчёт, без расширения
//    до 16 бит. Контрольные точки не нужны: страница - обычный байтовый
//    массив, отсчёт читается по индексу.
//  - Dpcm8 - 16-битные исходники, 8-битная дельта (dpcm8::encode_block),
//    контрольная точка 2 байта (только предиктор, схема не адаптивная)
//    через каждые kCheckpointIntervalSamples отсчётов. Точки пишутся по
//    ходу кодирования своей цепочкой страниц, finish() пришивает её за
//    последней страницей данных: цепочка сэмпла одна, точка K лежит по
//    смещению K*2 от checkpoint_first_page.
//  - Raw16 - 16-битные исходники как есть, два байта на отсчёт.
//
// Прореживание 2:1 средним пары соседних отсчётов - флаг конструктора;
// загрузчик ставит его сэмплам больше 1 МБ резидентно и выше 20 кГц (Amiga
// 8-16 кГц не трогаются). Непарный отсчёт ждёт следующего куска, последний
// уходит без усреднения.
//
// Запись прямо в страницу PSRAM, при прореживании - через 64 отсчёта на
// стеке: у Core1 4 КБ стека на весь загрузчик.
//
// Ping-pong петля разворачивается здесь же (set_loop_unroll): дойдя до
// конца петли, упаковщик читает уже записанную петлю из PSRAM с конца и
// дописывает её вперёд, хвост сэмпла идёт следом. Raw8 и Raw16 читаются
// как есть, Dpcm8 распаковывается и кодируется заново - обратный ход
// шумит сильнее прямого.

#include <cstdint>

#include "core/codec/dpcm8.h"
#include "core/model/instrument.h" // ResidentEncoding - один enum на весь путь (загрузчик -> каталог -> голос)
#include "core/memory/psram_store.h"
#include "core/memory/sample_cache_catalog.h"

namespace soundsinth::sample_pack {

using soundsinth::model::ResidentEncoding;

struct PackResult {
    uint16_t first_page = memory::kPageChainEnd;
    uint32_t checkpoint_count = 0; // всегда 0, кроме Dpcm8
    uint32_t total_samples = 0;
    bool ok = true;
    // kPageChainEnd - контрольных точек нет: Raw8/Raw16 или Dpcm8 без
    // единой точки (пустой сэмпл, синтетика в тестах). locate_block с
    // таким значением верен только для блока 0; к другим позициям
    // вызывающий (voice_trigger) идёт линейным проходом с выбросом.
    uint16_t checkpoint_first_page = memory::kPageChainEnd;
    const char* error = nullptr; // причина, если ok == false
};

class SamplePacker {
public:
    SamplePacker(memory::PsramStore& psram, ResidentEncoding mode, bool decimate = false);

    // samples/count - очередной кусок распакованных моно-отсчётов в
    // исходной разрядности файла, без расширения *256:
    //  - Raw8 - значения в [-128,127] (8-битная шкала источника), за это
    //    отвечает загрузчик; здесь только защитное ограничение.
    //  - Dpcm8, Raw16 - полная 16-битная шкала.
    // count любой. Для одного сэмпла можно звать сколько угодно раз:
    // состояние кодировщика и позиция в странице продолжаются между
    // вызовами. false - кончились свободные страницы PSRAM, работа
    // прекращается (PackResult::ok тоже false).
    bool add_samples(const int16_t* samples, uint32_t count);

    // Развернуть петлю loop_start..loop_end (отсчёты после прореживания) по
    // правилу mode. Звать до первого add_samples. Если поток не дошёл до
    // loop_end, finish() отказывает: дескриптор уже описывает развёрнутый
    // сэмпл.
    void set_loop_unroll(soundsinth::model::LoopUnroll mode, uint32_t loop_start, uint32_t loop_end);

    // Звать один раз, и при отказе тоже: цепочку надо опубликовать или
    // освободить. Выталкивает непарный отсчёт прореживания и пришивает
    // цепочку контрольных точек (Dpcm8) за данными.
    PackResult finish();

private:
    // Кодирует count отсчётов без прореживания; на конце петли вставляет
    // обратный проход.
    bool pack(const int16_t* samples, uint32_t count);
    // Кодирует count отсчётов в текущую цепочку как есть.
    bool write(const int16_t* samples, uint32_t count);
    // Обратный проход петли (set_loop_unroll).
    bool write_unroll();
    // Отсчёты first..first+count-1 из уже записанной цепочки.
    void read_back(uint32_t first, uint32_t count, int16_t* out);
    // Следующая страница данных, если текущей нет или она заполнена.
    bool ensure_page();
    // Новая страница в конец цепочки first..current.
    bool link_new_page(uint16_t& first, uint16_t& current);
    // Точки куска - в цепочку точек.
    bool append_checkpoints(const dpcm8::Dpcm8Checkpoint* points, uint32_t count);

    memory::PsramStore& psram_;
    ResidentEncoding mode_;
    bool decimate_;
    bool has_pending_ = false;
    int16_t pending_ = 0;
    uint32_t checkpoint_count_ = 0;
    uint16_t cp_first_page_ = memory::kPageChainEnd;
    uint16_t cp_page_ = memory::kPageChainEnd;
    uint32_t cp_pos_ = 0;

    soundsinth::model::LoopUnroll unroll_ = soundsinth::model::LoopUnroll::None; // None - разворота нет или он записан
    uint32_t unroll_start_ = 0;
    uint32_t unroll_end_ = 0;

    dpcm8::Dpcm8State state_; // только в Dpcm8
    uint32_t total_samples_ = 0;

    uint16_t first_page_ = memory::kPageChainEnd;
    uint16_t current_page_ = memory::kPageChainEnd;
    uint32_t page_pos_ = 0;

    bool ok_ = true;
    const char* error_ = nullptr;
};

// Закрыть упаковку одного сэмпла загрузчика: finish() ровно один раз, затем
// опубликовать цепочку в каталоге или освободить её ровно один раз.
// data_ok - заливка прошла целиком (файл не обрезан, PSRAM хватило).
// false - сэмпл не опубликован и остаётся нерезидентным; если заливка
// прошла, reason_out (может быть nullptr) получает причину, иначе причину
// уже назвал вызывающий. Путь .mid с общими цепочками банка
// (load_sample_from_bank) сюда не ходит.
bool finish_and_publish(SamplePacker& packer, bool data_ok, memory::PsramStore& psram,
                        memory::SampleCacheCatalog& catalog, uint16_t sample_index, const char** reason_out);

} // namespace soundsinth::sample_pack
