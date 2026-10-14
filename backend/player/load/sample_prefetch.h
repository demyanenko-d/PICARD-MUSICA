// SPDX-License-Identifier: MIT
#pragma once

// Планировщик порядка загрузки сэмплов (прогрессивная загрузка).
//
// Одним проходом по order-листу отвечает на два вопроса:
//   1. В каком порядке тянуть сэмплы с хоста, чтобы звук стартовал как
//      можно раньше и дальше не натыкался на незагруженные ноты.
//   2. До какой позиции order каждый сэмпл ещё нужен - данные для
//      вытеснения из PSRAM.
//
// Память не выделяет и в PSRAM не пишет: читает упакованные паттерны и
// заполняет массивы вызывающего.

#include <cstdint>

#include "core/model/song.h"
#include "core/memory/psram_store.h"
#include "player/config.h"

namespace player::load {

// Сколько первых воспроизводимых позиций order попадает в префетч - то,
// что должно быть в PSRAM до старта звука.
inline constexpr uint16_t kPrefetchOrderPositions = 2;

// out_last_use_order_pos[i] для сэмпла, не встреченного в order ни разу.
// Такой сэмпл не попадает в out_indices: грузить его незачем, для
// вытеснения он первый кандидат.
inline constexpr uint16_t kSampleNeverUsed = 0xffff;

// Маршрут по файлу за сэмплами. Выбирает хост (параметр команды начала
// сессии): цена перемотки у хостов разная.
enum class LoadOrder : uint8_t {
    // По возрастанию смещения в файле: у хоста прыжок назад - перечитывание
    // файла с начала. Маршрут почти линейный, хвост идёт от конца префетча
    // вперёд; назад - дважды: после метаданных к первому сэмплу префетча и
    // когда впереди не осталось ничего.
    ByFile = 0,
    // В порядке воспроизведения, как встретились в order, без сортировки.
    // Для хоста, который встаёт на любое смещение одинаково дёшево, но читает
    // небыстро (TR-DOS): следующий загруженный сэмпл - тот, что скорее всего
    // понадобится следующим.
    ByPlayback = 1,
};

// Результат plan_playback_order: сколько индексов записано в out_indices
// (не больше capacity) и сколько из них, с начала, - префетч.
struct PlaybackPlan {
    uint16_t count          = 0;
    uint16_t prefetch_count = 0;
};

// Заполняет out_indices индексами сэмплов в порядке загрузки.
//
// Порядок:
//   [0, prefetch_count) - сэмплы первых prefetch_positions воспроизводимых
//       позиций order (состав - по появлению в них); для .mid вызывающий
//       считает их по времени (SOUNDSINTH_MIDI_PREFETCH_SECONDS);
//   [prefetch_count, count) - остальные.
//
// Внутри части ByFile сортирует по file_offset, хвост - от последнего сэмпла
// префетча вперёд, лежащие раньше - в конце; ByPlayback - как встретились.
// Состав частей от стратегии не зависит.
//
// out_last_use_order_pos - обязателен, массив ровно на song.sample_count
// (он же пометка "сэмпл уже встречался", поэтому отдельного битового
// массива и потолка на число сэмплов нет): последняя позиция order, на
// которой сэмпл ещё звучит, или kSampleNeverUsed. Считается по позициям,
// а не по номерам паттернов: паттерн может стоять в order много раз, и
// его сэмплы свободны только после последнего вхождения.
//
// out_first_use_order_pos - необязательный, той же длины и индексации
// по номеру сэмпла: позиция order, на которой сэмпл звучит впервые, или
// kSampleNeverUsed. Нужен фоновой догрузке, чтобы не уходить дальше
// заданного упреждения. Индексация по сэмплу, потому что план
// пересортировывается по file_offset.
PlaybackPlan plan_playback_order(const soundsinth::model::Song& song, soundsinth::memory::PsramStore& psram, uint16_t* out_indices, uint16_t capacity,
                                 uint16_t* out_last_use_order_pos, LoadOrder order = LoadOrder::ByFile, uint16_t prefetch_positions = kPrefetchOrderPositions,
                                 uint16_t* out_first_use_order_pos = nullptr);

// Карта позиций: на каких позициях order звучит сэмпл, бит на корзину.
// Корзина - 1 << shift соседних позиций, корзин не больше kUseMapBuckets
// (у .mid позиций бывает больше 256). По ней вытеснение выбирает сэмпл с
// самым дальним следующим использованием. Память даёт вызывающий,
// use_map_bytes байт, обнулённых.
inline constexpr uint16_t kUseMapBuckets = 256;
inline constexpr uint16_t kUseMapNever   = 0xffff; // сэмпл не звучит вовсе
struct UseMap {
    uint32_t* words          = nullptr;
    uint16_t sample_count    = 0;
    uint16_t buckets         = 0;
    uint8_t words_per_sample = 0;
    uint8_t shift            = 0;
};

// Раскладка под песню, words не задаётся. buckets 0 - позиций нет.
UseMap use_map_layout(uint16_t sample_count, uint16_t order_count);
inline uint32_t use_map_bytes(const UseMap& m) {
    return static_cast<uint32_t>(m.sample_count) * m.words_per_sample * sizeof(uint32_t);
}
void use_map_mark(UseMap& m, uint16_t sample, uint16_t order_pos);
// Через сколько корзин от корзины order_pos сэмпл прозвучит: 0 - в ней же.
// После последней отметки счёт идёт с начала песни - так играет повтор.
// kUseMapNever - не звучит вовсе или карты нет.
uint16_t use_map_distance(const UseMap& m, uint16_t sample, uint16_t order_pos);

// --- Тот же план, но проходом длительности ---
//
// План собирается тем же проходом, что длительность: второй обход -
// повторная распаковка строк, у .mid - весь трек через конвертер.
//
// Сборщик цепляется наблюдателем к проходу длительности и заполняет те же
// три массива. Отличие от линейного обхода в составе: проход идёт по
// настоящему порядку воспроизведения, поэтому позиции, через которые
// песня перепрыгивает, в план не попадают, а позиция может и убывать -
// первая и последняя считаются минимумом и максимумом.
//
// Живёт на стеке вызывающего: инструмент канала переносится между
// паттернами, и это состояние прохода, а не песни.
struct PlanCollector {
    const soundsinth::model::Song* song = nullptr;
    uint16_t* indices                   = nullptr;
    uint16_t* last_use                  = nullptr;
    uint16_t* first_use                 = nullptr;
    uint16_t capacity                   = 0;
    uint16_t count                      = 0;

    uint16_t last_instrument[soundsinth::model::kMaxPatternChannels] = {};

    UseMap* use_map = nullptr; // nullptr - не собирать
};

void plan_collect_begin(PlanCollector& pc, const soundsinth::model::Song& song, uint16_t* out_indices, uint16_t capacity, uint16_t* out_last_use_order_pos,
                        uint16_t* out_first_use_order_pos);

// Ставится RowObserver'ом в compute_song_total_frames, user - сборщик.
void plan_collect_row(void* user, const soundsinth::model::PatternCell* cells, uint8_t channel_count, uint16_t order_pos);

// Разделить собранный план на префетч и хвост по первой позиции сэмпла и
// отсортировать части по стратегии. Граница префетча известна только
// вызывающему - у .mid она считается по времени, - поэтому деление идёт
// отдельно от сбора.
PlaybackPlan plan_split_prefetch(const soundsinth::model::Song& song, uint16_t* indices, uint16_t count, const uint16_t* first_use, uint16_t prefetch_positions,
                                 LoadOrder order);

} // namespace player::load
