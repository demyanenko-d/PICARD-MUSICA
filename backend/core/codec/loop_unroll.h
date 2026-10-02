// SPDX-License-Identifier: MIT
#pragma once

// Как сэмплы трека лягут в зону сэмплов PSRAM: кодек, прореживание и какие
// ping-pong петли развернуть в прямую (LoopUnroll).
//
// Решается на весь трек после разбора всех заголовков и заморозки зоны
// паттернов, до первого PCM, одинаково при полной загрузке и при загрузке
// по сэмплу: иначе один и тот же сэмпл звучал бы по-разному в зависимости
// от пути.

#include <cstdint>

#include "core/model/instrument.h"
#include "core/model/song.h"

namespace soundsinth::model {

// Страниц PSRAM под резидентный сэмпл: данные, у Dpcm8 ещё контрольные
// точки с новой страницы.
uint32_t resident_pages(ResidentEncoding e, uint32_t length_samples);

// Кодек и прореживание каждого сэмпла (decide_resident_encoding). Raw16 у
// 16-битных - если все резидентные сэмплы с ним помещаются в free_pages по
// точному счёту страниц, иначе Dpcm8; решение одно на трек. Дескрипторы - с
// длиной, петлёй и c5_speed из файла, прореживание их переписывает; зовётся
// один раз. Возвращает, выбран ли Raw16.
bool choose_resident_encoding(Song& song, uint32_t free_pages);

// Развернуть ping-pong петли по правилу mode и переписать дескрипторы:
// loop_unroll, loop_end, length_samples. free_pages - свободные страницы
// зоны сэмплов. Разворот берёт только остаток после всех сэмплов без
// разворота: трек, который помещался, помещается и дальше. На все не
// хватает - сначала короткие петли, остальные играют прямой петлёй.
// Петли в два отсчёта и короче не разворачиваются. Возвращает число
// развёрнутых.
uint16_t unroll_pingpong_loops(Song& song, uint32_t free_pages, LoopUnroll mode);

} // namespace soundsinth::model
