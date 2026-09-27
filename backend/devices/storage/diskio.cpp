// Переходник FatFs к арбитру носителя (storage.h).
//
// Единственный диск - та же карта через тот же арбитр: кэш секторов общий
// с эмуляторами, сектор, прочитанный хостом, достаётся даром.
//
// Запись не реализована: FF_FS_READONLY=1, FatFs её не вызывает, а если
// вызовет - ошибка лучше порчи чужой файловой системы (у карты есть
// писатель на стороне Z80).

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
        if (!devices::storage::storage_read_background(static_cast<uint32_t>(sector) + i,
                                              buff + i * devices::storage::kSectorBytes)) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_write(BYTE, const BYTE*, LBA_t, UINT) {
    return RES_WRPRT;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT:
            *static_cast<LBA_t*>(buff) = devices::storage::storage_sector_count(devices::storage::Client::Board);
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(buff) = devices::storage::kSectorBytes;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *static_cast<DWORD*>(buff) = 1; // единица стирания неизвестна
            return RES_OK;
        default: return RES_PARERR;
    }
}

} // extern "C"
