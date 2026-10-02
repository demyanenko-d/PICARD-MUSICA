// SPDX-License-Identifier: MIT
// Переходник FatFs к арбитру носителя.
//
// Единственный диск - та же карта через тот же арбитр: кэш секторов общий
// с эмуляторами, сектор, прочитанный хостом, достаётся даром.
//
// Запись разрешена (FF_FS_READONLY=0): ею пользуется только механизм
// настроек при старте.

#include "ff.h" // определяет BYTE/LBA_t/..., должен идти раньше diskio.h
#include "diskio.h"

#include "devices/storage/storage.h"

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == 0 && devices::storage::storage_present(devices::storage::Client::Board)) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    // Карта поднята storage_init() при старте, здесь только состояние.
    // Повторная инициализация сбросила бы обмен под эмуляторами.
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !devices::storage::storage_present(devices::storage::Client::Board)) return RES_NOTRDY;
    for (UINT i = 0; i < count; ++i) {
        // Фоновый клиент: заказы хоста арбитр пропускает вперёд сам.
        if (!devices::storage::storage_read_background(static_cast<uint32_t>(sector) + i, buff + i * devices::storage::kSectorBytes)) {
            return RES_ERROR;
        }
        // Заказанный блок читается тут же, а не ждёт витка Core1: таблицы
        // банка читаются при загрузке, когда витка ещё нет, и без этого
        // каждый сектор идёт к карте в одиночку. Единица арбитража от этого
        // не меняется - блок наполняется тем же вызовом, что и из цикла.
        devices::storage::storage_prefetch_step();
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !devices::storage::storage_present(devices::storage::Client::Board)) return RES_NOTRDY;
    for (UINT i = 0; i < count; ++i) {
        if (!devices::storage::storage_write_board(static_cast<uint32_t>(sector) + i, buff + i * devices::storage::kSectorBytes)) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *static_cast<LBA_t*>(buff) = devices::storage::storage_sector_count(devices::storage::Client::Board);
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(buff) = devices::storage::kSectorBytes;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *static_cast<DWORD*>(buff) = 1; // единица стирания неизвестна
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

} // extern "C"
