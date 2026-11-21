// SPDX-License-Identifier: MIT
#include "devices/storage/fat_check.h"

#include <cstring>

#include "core/util/le_bytes.h"

#include "devices/storage/fat_layout.h"
#include "devices/storage/partition.h"

namespace devices::storage {
namespace {

constexpr uint32_t kSector = hal::kSectorBytes;

using soundsinth::util::le16;
using soundsinth::util::le32;
using soundsinth::util::put_le32;

bool fsinfo_sector(const uint8_t* s) {
    return le32(s + kFsiLeadSig) == kFsiLead && le32(s + kFsiStructSig) == kFsiStruct && le32(s + kFsiTrailSig) == kFsiTrail;
}

void copy_label(char* out, const uint8_t* src) {
    std::memcpy(out, src, kBpbLabelBytes);
    out[kBpbLabelBytes] = 0;
    for (int i = kBpbLabelBytes - 1; i >= 0 && out[i] == ' '; --i) {
        out[i] = 0;
    }
    for (uint32_t i = 0; i < kBpbLabelBytes && out[i] != 0; ++i) {
        const auto c = static_cast<uint8_t>(out[i]);
        if (c < 0x20 || c >= 0x7f) out[i] = '?';
    }
}

} // namespace

bool fat_volume_open(const hal::SectorIo& io, uint8_t* sector, FatVolume& v) {
    v                  = FatVolume{};
    const uint32_t lba = find_fat_volume(io.read, io.user, sector);
    if (lba == kNoFatVolume || !io.read(io.user, lba, sector)) return false;
    v.part_lba               = lba;
    const uint32_t spc       = sector[kBpbSecPerClus];
    const uint32_t reserved  = le16(sector + kBpbReserved);
    const uint32_t fats      = sector[kBpbNumFats];
    const uint32_t root_ents = le16(sector + kBpbRootEntries);
    const uint32_t total     = le16(sector + kBpbTotal16) != 0 ? le16(sector + kBpbTotal16) : le32(sector + kBpbTotal32);
    const uint32_t fat_size  = le16(sector + kBpbFatSize16) != 0 ? le16(sector + kBpbFatSize16) : le32(sector + kBpbFatSize32);
    const uint32_t root_secs = (root_ents * 32u + kSector - 1u) / kSector;
    const uint32_t meta      = reserved + fats * fat_size + root_secs;
    if (fat_size == 0 || total <= meta) return false;
    v.clusters      = (total - meta) / spc;
    v.cluster_bytes = spc * kSector;
    v.fat_count     = static_cast<uint8_t>(fats);
    v.fat_lba       = v.part_lba + reserved;
    v.fat_size      = fat_size;
    v.fat_bits      = v.clusters < 4085u ? 12 : v.clusters < 65525u ? 16 : 32;
    v.fsinfo_free   = kFatNoValue;
    v.fsinfo_next   = kFatNoValue;
    // Записей в таблице - номера 0..clusters+1.
    const uint32_t entries = v.clusters + 2u;
    const uint32_t bytes   = v.fat_bits == 32 ? entries * 4u : v.fat_bits == 16 ? entries * 2u : (entries * 3u + 1u) / 2u;
    v.fat_sectors          = (bytes + kSector - 1u) / kSector;
    if (v.fat_sectors > fat_size) v.fat_sectors = fat_size;
    if (v.fat_bits == 32) {
        copy_label(v.label, sector + kBpbLabel32);
        const uint32_t fsi = le16(sector + kBpbFsInfo32);
        if (fsi != 0 && fsi < reserved && io.read(io.user, v.part_lba + fsi, sector) && fsinfo_sector(sector)) {
            v.fsinfo_lba  = v.part_lba + fsi;
            v.fsinfo_free = le32(sector + kFsiFree);
            v.fsinfo_next = le32(sector + kFsiNext);
        }
    } else {
        copy_label(v.label, sector + kBpbLabel16);
    }
    return true;
}

void fat_check_begin(FatCheck& c) {
    c                 = FatCheck{};
    c.first_free      = kFatNoValue;
    c.free_after_hint = kFatNoValue;
}

bool fat_check_step(const hal::SectorIo& io, const FatVolume& v, FatCheck& c, uint8_t* buf, uint32_t run) {
    if (v.fat_bits == 12 || c.io_error) return false;
    const bool wide       = v.fat_bits == 32;
    const uint32_t per    = wide ? kSector / 4u : kSector / 2u;
    const uint32_t last   = v.clusters + 1u;
    const uint32_t bad    = wide ? 0x0ffffff7u : 0xfff7u;
    const uint32_t eoc    = wide ? 0x0ffffff8u : 0xfff8u;
    uint8_t* const first  = buf;
    uint8_t* const second = buf + kSector;
    for (uint32_t n = 0; n < run && c.sector < v.fat_sectors; ++n, ++c.sector) {
        if (!io.read(io.user, v.fat_lba + c.sector, first)) {
            c.io_error = true;
            return false;
        }
        if (v.fat_count > 1) {
            if (!io.read(io.user, v.fat_lba + v.fat_size + c.sector, second)) {
                c.io_error = true;
                return false;
            }
            if (std::memcmp(first, second, kSector) != 0) ++c.copies_differ;
        }
        for (uint32_t e = 0; e < per; ++e) {
            const uint32_t idx = c.sector * per + e;
            if (idx > last) break;
            const uint32_t val = wide ? le32(first + e * 4u) & 0x0fffffffu : le16(first + e * 2u);
            if (idx == 1) {
                c.dirty = (val & (wide ? 0x08000000u : 0x8000u)) == 0;
                continue;
            }
            if (idx < 2) continue;
            if (val == 0) {
                ++c.free;
                if (c.first_free == kFatNoValue) c.first_free = idx;
                if (c.free_after_hint == kFatNoValue && idx >= v.fsinfo_next) c.free_after_hint = idx;
            } else if (val == bad) {
                ++c.bad;
            } else if (val < eoc && (val < 2u || val > last)) {
                ++c.invalid;
            }
        }
    }
    return c.sector < v.fat_sectors;
}

// Поиск с подсказки дольше стольких секторов FAT - подсказка мешает: на
// Спектруме сектор FAT перебирается около 40 мс.
constexpr uint32_t kHintSectors = 8;

bool fat_fsinfo_next_wrong(const FatVolume& v, const FatCheck& c) {
    if (v.fsinfo_lba == 0 || c.first_free == kFatNoValue) return false;
    const uint32_t n = v.fsinfo_next;
    if (n < 2u || n > v.clusters + 1u || c.free_after_hint == kFatNoValue) return true;
    const uint32_t per = v.fat_bits == 32 ? kSector / 4u : kSector / 2u;
    return c.free_after_hint - n > kHintSectors * per;
}

bool fat_fsinfo_wrong(const FatVolume& v, const FatCheck& c) {
    return v.fsinfo_lba != 0 && (v.fsinfo_free != c.free || fat_fsinfo_next_wrong(v, c));
}

bool fat_fix_fsinfo(const hal::SectorIo& io, FatVolume& v, FatCheck& c, uint8_t* sector) {
    if (v.fsinfo_lba == 0 || io.write == nullptr) return false;
    if (!io.read(io.user, v.fsinfo_lba, sector) || !fsinfo_sector(sector)) return false;
    put_le32(sector + kFsiFree, c.free);
    put_le32(sector + kFsiNext, c.first_free);
    if (!io.write(io.user, v.fsinfo_lba, sector)) return false;
    v.fsinfo_free     = c.free;
    v.fsinfo_next     = c.first_free;
    c.free_after_hint = c.first_free;
    return true;
}

} // namespace devices::storage
