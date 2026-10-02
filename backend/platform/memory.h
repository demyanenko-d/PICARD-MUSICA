// SPDX-License-Identifier: MIT
#pragma once

// Память платформы под структуры ядра.

#include <cstddef>
#include <cstdint>

namespace platform {

// Память резидентной арены трека (memory::Arena), выровненная на
// memory::kArenaBaseAlign. ПК - malloc/free; RP2350 - статический пул
// прошивки, один раз за работу, второй запрос - panic.
uint8_t* resident_storage_acquire(size_t bytes);
void resident_storage_release(uint8_t* storage);

// PSRAM (PsramStore). На MCU за ней стоит не куча, а адресное
// пространство PSRAM через QMI XIP: указатель не двигается, содержимым
// управляет PsramStore.
// PC: malloc(total_bytes) / free().
// RP2350: total_bytes игнорируется, возвращается фиксированный адрес XIP,
// psram_base_release() ничего не делает - QMI настроен один раз при старте.
uint8_t* psram_base_acquire(size_t total_bytes);
void psram_base_release(uint8_t* base);

// Адрес для записи участка PSRAM целиком, пока его никто не читает (новая
// страница сэмпла). RP2350: строки участка выброшены из кэша XIP, адрес -
// алиас без кэша: запись мимо кэша вдвое быстрее (плата: 44.7 МБ/с против
// 22.7), а старая изменённая строка не затрёт её при вытеснении. Писать
// выровненными 32-битными словами. p и bytes кратны 8. ПК - p.
uint8_t* psram_write_alias(uint8_t* p, size_t bytes);

} // namespace platform
