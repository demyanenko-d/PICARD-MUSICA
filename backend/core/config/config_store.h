// SPDX-License-Identifier: MIT
#pragma once

// Хранение настроек в двух блоках флеша.
//
// Блоки пишутся по очереди: читаются оба, берётся годный с большим
// счётчиком, новый уходит в соседний со счётчиком на единицу больше. Так
// износ размазывается на два блока, а обрыв записи не уносит прежние
// настройки - они целы в соседнем.
//
// Блок считается годным, когда сходятся подпись, номер формата и сумма.

#include <cstdint>

#include "core/config/config.h"

namespace soundsinth::config {

// Размер блока - гранула стирания флеша, 4 КБ у всей мелочи на этих
// платах. Заполнено из него полсотни байт, остальное запас под формат.
inline constexpr uint32_t kPageBytes = 4096;
inline constexpr uint32_t kPageCount = 2;

// 'SSCF' младшим байтом вперёд.
inline constexpr uint32_t kMagic = 0x46435353u;

struct PageHeader {
    uint32_t magic;
    uint16_t format;
    uint16_t bytes;   // сколько байт настроек лежит следом за заголовком
    uint32_t counter; // растёт на единицу с каждой записью
    uint32_t crc;     // по формату, длине, счётчику и настройкам
};
static_assert(sizeof(PageHeader) == 16, "the block header is fixed by the format number");

inline constexpr uint32_t kPayloadOffset = sizeof(PageHeader);

// Что удалось понять про блок.
struct PageInfo {
    bool valid       = false;
    uint32_t counter = 0;
};

PageInfo page_inspect(const uint8_t* page);

// Настройки из годного блока. false - блок не годен, out не трогается.
bool page_read(const uint8_t* page, Settings& out);

// Собрать блок целиком: заголовок, настройки, запас единицами - как у
// стёртой флеш-памяти, чтобы запись не меняла лишних ячеек.
void page_build(uint8_t* page, const Settings& s, uint32_t counter);

// Какой из блоков брать: 0, 1 или -1, если годных нет. При равных
// счётчиках берётся первый - такого быть не должно, но выбор определён.
int store_pick(const uint8_t* page0, const uint8_t* page1);

// В какой блок писать следующий и с каким счётчиком. Годных нет - пишем в
// нулевой с единицы.
struct NextWrite {
    int slot         = 0;
    uint32_t counter = 1;
};
NextWrite store_next(const uint8_t* page0, const uint8_t* page1);

} // namespace soundsinth::config
