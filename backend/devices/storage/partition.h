// SPDX-License-Identifier: MIT
#pragma once

// Где на носителе лежит том FAT. FatFs сам понимает только сектор 0 без
// таблицы и четыре основных раздела MBR; здесь ещё логические разделы в
// расширенном и GPT. Найденный сектор переходник FatFs делает нулевым, и
// FatFs видит раздел как диск без таблицы.
//
// Порядок: сектор 0 - сам том; иначе MBR по записям, логические - по
// цепочке расширенного; защитная запись GPT (0xEE) - разделы "основные
// данные" по таблице GPT. Первый том FAT и берётся. Смещение 32-разрядное:
// носители до 2 ТБ.

#include <cstdint>

#include "devices/hal/media.h"

namespace devices::storage {

inline constexpr uint32_t kNoFatVolume = 0xffffffffu;

// buf - сектор под чтение.
uint32_t find_fat_volume(hal::SectorReadFn read, void* user, uint8_t* buf);

// Сектор - загрузочный сектор тома FAT (12/16/32).
bool is_fat_boot_sector(const uint8_t* s);

// Где на носителе тома FatFs (kVolumeCard, kVolumeUsb) начинается его том:
// сектор тома + это = сектор носителя. Знает переходник FatFs с последнего
// монтирования.
uint32_t fat_volume_base(uint8_t drive);

} // namespace devices::storage
