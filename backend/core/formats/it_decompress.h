// SPDX-License-Identifier: MIT
#pragma once

// Порт распаковки сэмплов IT из OpenMPT: ITDecompression
// (soundlib/ITCompression.cpp) и BitReader.h, арифметика не менялась.
//
// Copyright (c) OpenMPT Devs, BSD-3-Clause. Исходный алгоритм - Ben
// "GreaseMonkey" Russell, общественное достояние.
//
// Алгоритм блочный, битовая последовательность с адаптивной шириной (не
// ADPCM). Блок кодируется с mem1 = mem2 = 0, состояние декодера
// сбрасывается на каждый блок.
//
// API потоковый: блок бывает до 32768 отсчётов (8 бит), держать выход
// целиком в буфере перепаковки (сценарий SampleRepack, общий
// на всё) нельзя. decompress_step() декодирует до max_samples отсчётов за
// вызов, состояние (позиция в битовом потоке, mem1/mem2, ширина) - в
// DecompressState, размер куска выбирает вызывающий.

#include <cstdint>

namespace soundsinth::formats::it {

// Состояние распаковки одного блока. Новый блок - DecompressState{} и флаги
// заголовка сэмпла; width == 0 - decompress_step поставит стартовую ширину
// на первом вызове.
struct DecompressState {
    uint32_t byte_pos = 0;
    uint32_t bit_buf  = 0;
    int32_t bit_num   = 0;
    bool ok           = true;
    bool is16bit      = false;
    bool is215        = false; // вариант сжатия IT 2.15
    int32_t mem1      = 0;
    int32_t mem2      = 0;
    int32_t width     = 0;
};

// Шаг загрузчика - kStepSamples отсчётов. На отсчёт не больше 38 бит (поле 17
// бит плюс смена ширины 17 + 4) - 4864 байта на шаг, kStepWorstBytes с
// запасом. Посреди значения decompress_step не возобновляется: перед шагом
// впереди должно быть kStepWorstBytes байт потока или конец блока.
inline constexpr uint32_t kStepSamples    = 1024;
inline constexpr uint32_t kStepWorstBytes = 5120;

// bitstream/bitstream_bytes - сжатые данные блока от его начала (2-байтный
// префикс длины блока уже прочитан вызывающим). state - вход и выход: для
// продолжения - то, что вернул предыдущий вызов. out - сырой диапазон (8
// бит: -128..127, без *256): масштабирование делает вызывающий, порт
// остаётся побайтово сверяемым с эталоном.
//
// Возвращает число декодированных за этот вызов отсчётов; меньше
// max_samples, если поток блока кончился (конец блока или битый файл). 0 -
// блок исчерпан или поток оборвался, дальше звать не нужно.
uint32_t decompress_step(DecompressState& state, const uint8_t* bitstream, uint32_t bitstream_bytes, uint32_t max_samples, int16_t* out);

} // namespace soundsinth::formats::it
