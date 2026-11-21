// SPDX-License-Identifier: MIT
// Переходник FatFs к арбитру носителя.
//
// Диска два: kVolumeCard - карта платы (тот же арбитр, что у эмуляторов: сектор,
// прочитанный хостом, достаётся даром), kVolumeUsb - флешка USB. На каждом том FAT
// ищется сам: найденный раздел FatFs видит диском без
// таблицы, его первый сектор - нулевой.

#include "ff.h" // определяет BYTE/LBA_t/..., должен идти раньше diskio.h
#include "diskio.h"

#include "devices/hal/media.h"
#include "devices/storage/partition.h"
#include "devices/storage/storage.h"
#include "devices/storage/volumes.h"

namespace {

using devices::hal::Medium;
namespace st = devices::storage;

constexpr BYTE kDrives = st::kVolumeCount;
static_assert(FF_VOLUMES == kDrives, "drive 0 is the card, drive 1 is the USB stick");

uint32_t s_base[kDrives];

Medium medium(BYTE pdrv) {
    return st::volume_medium(pdrv);
}

bool present(BYTE pdrv) {
    return pdrv < kDrives && st::volume_present(pdrv);
}

bool read_raw(BYTE pdrv, uint32_t lba, BYTE* dst) {
    if (pdrv == st::kVolumeUsb) return st::storage_tool_read(Medium::Usb, lba, dst);
    // Фоновый клиент: заказы хоста арбитр пропускает вперёд сам.
    if (!st::storage_read_background(lba, dst)) return false;
    // Заказанный блок читается тут же, а не ждёт витка Core1: таблицы банка
    // читаются при загрузке, когда витка ещё нет, и без этого каждый сектор
    // идёт к карте в одиночку.
    st::storage_prefetch_step_board();
    return true;
}

bool read_for_search(void* user, uint32_t lba, uint8_t* buf) {
    return read_raw(static_cast<BYTE>(reinterpret_cast<uintptr_t>(user)), lba, buf);
}

} // namespace

uint32_t devices::storage::fat_volume_base(uint8_t drive) {
    return drive < kDrives ? s_base[drive] : 0;
}

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    // Другой носитель под тем же диском - STA_NOINIT: FatFs монтирует заново,
    // а не пишет по FAT и каталогу прежнего.
    return present(pdrv) && st::volume_same_medium(pdrv) ? 0 : STA_NOINIT;
}

// Носитель поднимают драйверы, здесь - только поиск тома: FatFs зовёт это
// при каждом монтировании, и сменённая флешка найдётся заново.
DSTATUS disk_initialize(BYTE pdrv) {
    if (!present(pdrv)) return STA_NOINIT;
    BYTE sector[devices::hal::kSectorBytes];
    const uint32_t base = st::find_fat_volume(&read_for_search, reinterpret_cast<void*>(static_cast<uintptr_t>(pdrv)), sector);
    s_base[pdrv]        = base == st::kNoFatVolume ? 0 : base;
    st::volume_remember_medium(pdrv);
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (!present(pdrv)) return RES_NOTRDY;
    for (UINT i = 0; i < count; ++i) {
        if (!read_raw(pdrv, s_base[pdrv] + static_cast<uint32_t>(sector) + i, buff + i * devices::hal::kSectorBytes)) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    // Окно FatFs сбрасывается и без disk_status: на другой носитель - никогда.
    if (disk_status(pdrv) != 0) return RES_NOTRDY;
    for (UINT i = 0; i < count; ++i) {
        const uint32_t lba = s_base[pdrv] + static_cast<uint32_t>(sector) + i;
        const BYTE* src    = buff + i * devices::hal::kSectorBytes;
        if (!(pdrv == st::kVolumeCard ? st::storage_write_board(lba, src) : st::storage_tool_write(medium(pdrv), lba, src))) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv >= kDrives) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *static_cast<LBA_t*>(buff) = devices::hal::media_state(medium(pdrv)).sectors - s_base[pdrv];
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(buff) = devices::hal::kSectorBytes;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *static_cast<DWORD*>(buff) = 1; // единица стирания неизвестна
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

} // extern "C"
