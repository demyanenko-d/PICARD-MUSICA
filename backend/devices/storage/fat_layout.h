// SPDX-License-Identifier: MIT
#pragma once

// Раскладка FAT на диске: смещения полей загрузочного сектора (BPB),
// FSINFO, таблицы разделов MBR и заголовка GPT. Одни имена на читателей
// (fat_check, partition, storage) и писателя (vdisk).

#include <cstdint>

namespace devices::storage {

// BPB: общие поля FAT16/FAT32, затем хвосты каждого. Метка - 11 знаков.
inline constexpr uint32_t kBpbBytesPerSec = 11;
inline constexpr uint32_t kBpbSecPerClus  = 13;
inline constexpr uint32_t kBpbReserved    = 14;
inline constexpr uint32_t kBpbNumFats     = 16;
inline constexpr uint32_t kBpbRootEntries = 17;
inline constexpr uint32_t kBpbTotal16     = 19;
inline constexpr uint32_t kBpbMedia       = 21;
inline constexpr uint32_t kBpbFatSize16   = 22;
inline constexpr uint32_t kBpbSecPerTrack = 24;
inline constexpr uint32_t kBpbHeads       = 26;
inline constexpr uint32_t kBpbHidden      = 28;
inline constexpr uint32_t kBpbTotal32     = 32;
inline constexpr uint32_t kBpbLabel16     = 43;
inline constexpr uint32_t kBpbFatSize32   = 36;
inline constexpr uint32_t kBpbRootClus32  = 44;
inline constexpr uint32_t kBpbFsInfo32    = 48;
inline constexpr uint32_t kBpbBackup32    = 50;
inline constexpr uint32_t kBpbDrive32     = 64;
inline constexpr uint32_t kBpbBootSig32   = 66; // 0x29: дальше том и метка
inline constexpr uint32_t kBpbVolId32     = 67;
inline constexpr uint32_t kBpbLabel32     = 71;
inline constexpr uint32_t kBpbFsType32    = 82; // "FAT32   "
inline constexpr uint32_t kBpbLabelBytes  = 11;

// FSINFO: подписи в начале, перед счётчиком и в конце.
inline constexpr uint32_t kFsiLeadSig   = 0;
inline constexpr uint32_t kFsiStructSig = 484;
inline constexpr uint32_t kFsiFree      = 488;
inline constexpr uint32_t kFsiNext      = 492;
inline constexpr uint32_t kFsiTrailSig  = 508;
inline constexpr uint32_t kFsiLead      = 0x41615252u;
inline constexpr uint32_t kFsiStruct    = 0x61417272u;
inline constexpr uint32_t kFsiTrail     = 0xaa550000u;

// MBR: подпись диска, таблица из четырёх записей, подпись 0x55 0xAA.
inline constexpr uint32_t kMbrDiskId           = 440;
inline constexpr uint32_t kMbrPartitionTable   = 446;
inline constexpr uint32_t kMbrPartitionCount   = 4;
inline constexpr uint32_t kMbrEntryBytes       = 16;
inline constexpr uint32_t kMbrEntryChsFirst    = 1;
inline constexpr uint32_t kMbrEntryType        = 4;
inline constexpr uint32_t kMbrEntryChsLast     = 5;
inline constexpr uint32_t kMbrEntryFirstSector = 8;
inline constexpr uint32_t kMbrEntrySectorCount = 12;
inline constexpr uint32_t kMbrSignature        = 510; // 0x55, 0xaa

// Заголовок GPT (сектор 1) и его записи; 64-битные поля - младшая
// половина, старшая по +4.
inline constexpr uint32_t kGptTableLba   = 72;
inline constexpr uint32_t kGptCount      = 80;
inline constexpr uint32_t kGptEntryBytes = 84;
inline constexpr uint32_t kGptEntryFirst = 32;

} // namespace devices::storage
