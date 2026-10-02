// SPDX-License-Identifier: MIT
// Реализация diskio.h (third_party/fatfs) для PC: физический диск - это
// файл-образ (build/sd.img), читаемый и записываемый посекторно через
// <cstdio>. На MCU этот файл заменяется драйвером SD-карты, ff.c
// (third_party/fatfs) не меняется.

#include "pc/fatfs_disk.h"

#include <cstdio>

#ifdef _WIN32
// ff.h само делает #include <windows.h> внутри своего extern "C" { ... }
// (см. ff.h: "#if defined(_WIN32) /* for development only */") - под
// C++ это ломает перегруженные функции из <windows.h> (BitScanForward и
// т.п.), потому что они попадают под C-линковку. Подключаем windows.h
// заранее, снаружи любого extern "C" - инклюд-гард сделает повторное
// подключение из ff.h пустой операцией.
#include <windows.h>
#endif

#include "ff.h" // определяет BYTE/LBA_t/... - должен идти раньше diskio.h
#include "diskio.h"

namespace {
constexpr long kSectorSize = 512;
std::FILE* g_image_file    = nullptr;
} // namespace

namespace platform_pc {

bool mount_disk_image(const char* path) {
    if (g_image_file) {
        std::fclose(g_image_file);
        g_image_file = nullptr;
    }
    g_image_file = std::fopen(path, "r+b");
    return g_image_file != nullptr;
}

} // namespace platform_pc

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0 || !g_image_file) {
        return STA_NOINIT;
    }
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0 || !g_image_file) {
        return STA_NOINIT;
    }
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !g_image_file) {
        return RES_NOTRDY;
    }
    if (std::fseek(g_image_file, static_cast<long>(sector) * kSectorSize, SEEK_SET) != 0) {
        return RES_ERROR;
    }
    if (std::fread(buff, kSectorSize, count, g_image_file) != count) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    // FF_FS_READONLY=1 (ffconf.h) - FatFs сам не должен звать это в
    // штатной работе; оставлено на случай будущей записи (см. ffconf.h).
    if (pdrv != 0 || !g_image_file) {
        return RES_NOTRDY;
    }
    if (std::fseek(g_image_file, static_cast<long>(sector) * kSectorSize, SEEK_SET) != 0) {
        return RES_ERROR;
    }
    if (std::fwrite(buff, kSectorSize, count, g_image_file) != count) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0 || !g_image_file) {
        return RES_NOTRDY;
    }
    switch (cmd) {
        case CTRL_SYNC:
            std::fflush(g_image_file);
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(buff) = static_cast<WORD>(kSectorSize);
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

// FF_FS_READONLY=1 отключает всё, что ставит метку времени при записи,
// но символ нужен для линковки. Значение фиксированное: на PC нет RTC,
// для чтения это не важно.
DWORD get_fattime(void) {
    return 0;
}

} // extern "C"
