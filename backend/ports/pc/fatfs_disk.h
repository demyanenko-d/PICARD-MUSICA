#pragma once

namespace platform_pc {

// Открывает файл-образ (например build/sd.img от scripts/build_sd.bat) и
// делает его физическим диском для FatFs (pdrv=0, единственный том -
// FF_VOLUMES=1). Вызывать до f_mount(). На MCU эту роль играет драйвер
// SD-карты, сам FatFs (third_party/fatfs) не меняется.
bool mount_disk_image(const char* path);

} // namespace platform_pc
