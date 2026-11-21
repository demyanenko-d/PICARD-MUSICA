// SPDX-License-Identifier: MIT
#include "devices/storage/partition.h"

#include <cstring>

#include "core/util/le_bytes.h"

#include "devices/storage/fat_layout.h"

namespace devices::storage {

namespace {

using soundsinth::util::le16;
using soundsinth::util::le32;

bool signed_55aa(const uint8_t* s) {
    return s[kMbrSignature] == 0x55 && s[kMbrSignature + 1] == 0xaa;
}

bool extended_type(uint8_t t) {
    return t == 0x05 || t == 0x0f || t == 0x85;
}

// Глубина обхода: цепочка логических и таблица GPT конечны, испорченная
// таблица не должна зациклить поиск.
constexpr uint32_t kMaxLogical    = 64;
constexpr uint32_t kMaxGptEntries = 128;

// "Основные данные" Microsoft: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, на
// диске первые три поля - младшим байтом вперёд.
constexpr uint8_t kGptBasicData[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};

bool fat_at(hal::SectorReadFn read, void* user, uint8_t* buf, uint32_t lba) {
    return read(user, lba, buf) && is_fat_boot_sector(buf);
}

uint32_t from_gpt(hal::SectorReadFn read, void* user, uint8_t* buf) {
    if (!read(user, 1, buf) || std::memcmp(buf, "EFI PART", 8) != 0) return kNoFatVolume;
    const uint32_t table = le32(buf + kGptTableLba);
    const uint32_t count = le32(buf + kGptCount);
    const uint32_t size  = le32(buf + kGptEntryBytes);
    if (le32(buf + kGptTableLba + 4) != 0 || size < 128 || size > hal::kSectorBytes || (hal::kSectorBytes % size) != 0) return kNoFatVolume;
    const uint32_t per_sector = hal::kSectorBytes / size;
    for (uint32_t i = 0; i < count && i < kMaxGptEntries; i++) {
        if (!read(user, table + i / per_sector, buf)) return kNoFatVolume;
        const uint8_t* e = buf + (i % per_sector) * size;
        if (std::memcmp(e, kGptBasicData, sizeof(kGptBasicData)) != 0 || le32(e + kGptEntryFirst + 4) != 0) continue;
        const uint32_t first = le32(e + kGptEntryFirst);
        if (fat_at(read, user, buf, first)) return first;
    }
    return kNoFatVolume;
}

} // namespace

bool is_fat_boot_sector(const uint8_t* s) {
    if (!signed_55aa(s) || !(s[0] == 0xeb || s[0] == 0xe9 || s[0] == 0xe8)) return false;
    if (le16(s + kBpbBytesPerSec) != hal::kSectorBytes || s[kBpbNumFats] == 0 || s[kBpbNumFats] > 2 || le16(s + kBpbReserved) == 0) return false;
    const uint8_t spc = s[kBpbSecPerClus];
    return spc != 0 && (spc & (spc - 1)) == 0;
}

uint32_t find_fat_volume(hal::SectorReadFn read, void* user, uint8_t* buf) {
    if (!read(user, 0, buf)) return kNoFatVolume;
    if (is_fat_boot_sector(buf)) return 0;
    if (!signed_55aa(buf)) return kNoFatVolume;
    uint8_t mbr[kMbrPartitionCount * kMbrEntryBytes];
    std::memcpy(mbr, buf + kMbrPartitionTable, sizeof(mbr));
    if (mbr[kMbrEntryType] == 0xee) return from_gpt(read, user, buf);
    for (uint32_t i = 0; i < kMbrPartitionCount; i++) {
        const uint8_t* e    = mbr + i * kMbrEntryBytes;
        const uint32_t base = le32(e + kMbrEntryFirstSector);
        if (e[kMbrEntryType] == 0 || base == 0) continue;
        if (!extended_type(e[kMbrEntryType])) {
            if (fat_at(read, user, buf, base)) return base;
            continue;
        }
        // Расширенный: в каждой EBR первая запись - логический раздел от
        // этой EBR, вторая - следующая EBR от начала расширенного.
        uint32_t ebr = base;
        for (uint32_t n = 0; n < kMaxLogical; n++) {
            if (!read(user, ebr, buf) || !signed_55aa(buf)) break;
            const uint8_t* e1      = buf + kMbrPartitionTable;
            const uint8_t* e2      = e1 + kMbrEntryBytes;
            const uint32_t logical = le32(e1 + kMbrEntryFirstSector);
            const uint32_t next    = le32(e2 + kMbrEntryFirstSector);
            const uint8_t next_t   = e2[kMbrEntryType];
            if (e1[kMbrEntryType] != 0 && logical != 0 && fat_at(read, user, buf, ebr + logical)) return ebr + logical;
            if (next == 0 || !extended_type(next_t)) break;
            ebr = base + next;
        }
    }
    return kNoFatVolume;
}

} // namespace devices::storage
