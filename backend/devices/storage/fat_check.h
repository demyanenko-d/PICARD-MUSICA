// SPDX-License-Identifier: MIT
#pragma once

// Быстрая проверка тома FAT16/FAT32 прямо по секторам: пересчёт свободных
// кластеров, сверка двух копий FAT, признак незакрытого тома, запись
// FSINFO. Цепочки файлов и каталоги не проверяются.
//
// Носитель - через пару функций чтения и записи: одна и та же проверка
// идёт по карте, по флешке и по образу на ПК.

#include <cstdint>

#include "devices/hal/media.h"

namespace devices::storage {

inline constexpr uint32_t kFatNoValue = 0xffffffffu;

struct FatVolume {
    uint32_t part_lba; // начало раздела
    uint8_t fat_bits;  // 12, 16, 32
    uint8_t fat_count;
    uint32_t fat_lba;     // первая копия FAT
    uint32_t fat_size;    // секторов одной копии
    uint32_t fat_sectors; // из них заняты записями кластеров тома
    uint32_t clusters;    // кластеров данных, номера 2..clusters+1
    uint32_t cluster_bytes;
    uint32_t fsinfo_lba; // 0 - FSINFO нет
    uint32_t fsinfo_free;
    uint32_t fsinfo_next;
    char label[12];
};

// Разобрать таблицу разделов (первый раздел FAT) или том без неё, затем
// FSINFO. sector - буфер на 512 байт. false - FAT не найдена.
bool fat_volume_open(const hal::SectorIo& io, uint8_t* sector, FatVolume& v);

struct FatCheck {
    uint32_t sector; // следующий сектор FAT
    uint32_t free;
    uint32_t first_free;      // kFatNoValue - свободных нет
    uint32_t free_after_hint; // первый свободный не раньше подсказки FSINFO
    uint32_t bad;
    uint32_t invalid;       // ссылки за пределы тома
    uint32_t copies_differ; // секторов, где копии FAT различаются
    bool dirty;             // том не закрыт: бит в FAT[1]
    bool io_error;
};

void fat_check_begin(FatCheck& c);

// До run секторов FAT за вызов. buf - два сектора: копия 1 и копия 2.
// true - работа ещё есть. FAT12 не проверяется: сразу false.
bool fat_check_step(const hal::SectorIo& io, const FatVolume& v, FatCheck& c, uint8_t* buf, uint32_t run);

// Секторов FAT, которые пройдёт проверка.
inline uint32_t fat_check_total(const FatVolume& v) {
    return v.fat_sectors;
}

// Подсказка FSINFO, с которой ищут свободный кластер, пустая, за томом или
// перед длинным занятым участком: поиск шёл бы через него.
bool fat_fsinfo_next_wrong(const FatVolume& v, const FatCheck& c);

// FSINFO расходится с пересчётом: свободное место или подсказка.
bool fat_fsinfo_wrong(const FatVolume& v, const FatCheck& c);

// Записать пересчитанное в FSINFO. false - FSINFO нет, подписи не те или
// запись не прошла.
bool fat_fix_fsinfo(const hal::SectorIo& io, FatVolume& v, FatCheck& c, uint8_t* sector);

} // namespace devices::storage
