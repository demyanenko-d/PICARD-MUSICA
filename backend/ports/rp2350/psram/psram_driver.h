// SPDX-License-Identifier: MIT
#pragma once

// Драйвер PSRAM AP Memory APS6404L-3SQR (8 МБ, QPI, QMI XIP CS1).
//
// Подключение: CS - GPIO psram::kCsGpio, функция
// GPIO_FUNC_XIP_CS1; SCK/DQ0-3 - встроенные пины QMI, драйвер их не
// трогает.
//
// Адрес (XIP M1): psram::kXipBase, кэшируемый write-back алиас 0x11000000.
// Читает и пишет PSRAM ядро (через platform::psram_base_acquire), у
// драйвера - только старт, сброс кэша и самотест.
//
// psram_init() - один раз при старте на Core0, после установки
// системной частоты (тайминги QMI считаются от clock_get_hz(clk_sys)) и
// до multicore_launch_core1 (psram_base_acquire() на Core1 должен видеть
// готовую PSRAM).
//
// Выполняется целиком из SRAM (__not_in_flash_func): пока QMI в direct
// mode (сброс и настройка чипа), флеш XIP (M0) недоступен, а прошивка не
// использует copy_to_ram.

#include <cstdint>

namespace psram {

void psram_init();

// Запись через кэшируемый алиас идёт в кэш XIP (грязные строки);
// psram_flush() сбрасывает их в PSRAM. Между ядрами сброс не нужен: кэш XIP
// один на оба, и сейчас его не зовёт никто - мимо кэша PSRAM не читает ни
// ядро, ни DMA. Нужен будет читателю мимо кэша (DMA из PSRAM), и тогда -
// xip_cache_clean_range по его страницам: xip_cache_clean_all() на RP2350
// заодно инвалидирует весь кэш (обход RP2350-E11 в pico-sdk), и строки кода
// Core0 во флеше тоже - под звуком щелчки.
void psram_flush();

// Сколько 16-битных слов проверяет psram_selftest (256 байт).
inline constexpr uint32_t kSelftestWords = 128;

// Быстрая проверка записи и чтения первых 256 байт PSRAM (не всего
// чипа), единицы мс: чип подключён и отвечает. Возвращает число
// несовпавших слов, 0 - чип исправен.
uint32_t psram_selftest();

} // namespace psram
