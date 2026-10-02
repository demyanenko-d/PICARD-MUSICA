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
// его сэмплы свободны только после последнего вхождения. Вытеснение
// освобождает сэмпл, когда позиция воспроизведения ушла дальше его
// последней позиции.
//
// out_first_use_order_pos - необязательный, той же длины и индексации
// по номеру сэмпла: позиция order, на которой сэмпл звучит впервые, или
// kSampleNeverUsed. Нужен фоновой догрузке, чтобы не уходить дальше
// заданного упреждения. Индексация по сэмплу, потому что план
// пересортировывается по file_offset.
PlaybackPlan plan_playback_order(const soundsinth::model::Song& song, soundsinth::memory::PsramStore& psram, uint16_t* out_indices, uint16_t capacity,
                                 uint16_t* out_last_use_order_pos, LoadOrder order = LoadOrder::ByFile, uint16_t prefetch_positions = kPrefetchOrderPositions,
                                 uint16_t* out_first_use_order_pos = nullptr);

// --- Тот же план, но проходом длительности ---
//
// До первой ноты песня обходилась дважды: проход длительности и отдельный
// обход плана читали одни и те же строки. У трекерного файла второй обход -
// повторная распаковка из PSRAM, у .mid - весь трек через конвертер заново,
// потому что строк в памяти нет и их выдаёт источник.
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
    const soundsinth::model::Song* song                              = nullptr;
    uint16_t* indices                                                = nullptr;
    uint16_t* last_use                                               = nullptr;
    uint16_t* first_use                                              = nullptr;
    uint16_t capacity                                                = 0;
    uint16_t count                                                   = 0;
    uint16_t prefetch_positions                                      = 0;
    uint16_t prefetch_count                                          = 0;
    uint16_t positions_seen                                          = 0;
    uint16_t prev_order_pos                                          = 0xffffu;
    uint16_t last_instrument[soundsinth::model::kMaxPatternChannels] = {};
};

void plan_collect_begin(PlanCollector& pc, const soundsinth::model::Song& song, uint16_t* out_indices, uint16_t capacity, uint16_t* out_last_use_order_pos,
                        uint16_t* out_first_use_order_pos, uint16_t prefetch_positions);

// Ставится RowObserver'ом в compute_song_total_frames, user - сборщик.
void plan_collect_row(void* user, const soundsinth::model::PatternCell* cells, uint8_t channel_count, uint16_t order_pos);

// Досортировать по стратегии и отдать результат. Проход кончился неполным
// (упёрся в пределы или буфера не было) - план неполон, брать его нельзя.
PlaybackPlan plan_collect_finish(PlanCollector& pc, LoadOrder order);

// Разделить собранный план на префетч и хвост по первой позиции сэмпла и
// отсортировать части по стратегии. Граница префетча известна только
// вызывающему - у .mid она считается по времени, - поэтому деление идёт
// отдельно от сбора.
PlaybackPlan plan_split_prefetch(const soundsinth::model::Song& song, uint16_t* indices, uint16_t count, const uint16_t* first_use, uint16_t prefetch_positions,
                                 LoadOrder order);

} // namespace player::load
