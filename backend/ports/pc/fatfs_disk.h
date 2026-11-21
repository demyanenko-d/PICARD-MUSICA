// SPDX-License-Identifier: MIT
#pragma once

namespace platform_pc {

// Открывает файл-образ (например build/sd.img от scripts/build_sd.bat) и
// делает его физическим диском для FatFs (pdrv=0, единственный том -
// FF_VOLUMES=1). Вызывать до f_mount(). На MCU эту роль играет драйвер
// SD-карты, сам FatFs (third_party/fatfs) не меняется.
bool mount_disk_image(const char* path);

// Диск без файла: сектор отдаёт функция (false - ошибка чтения). Запись
// отвергается. nullptr - снять.
using SectorReader = bool (*)(unsigned long lba, unsigned char* out);
void mount_sector_reader(SectorReader reader);

} // namespace platform_pc
