// SPDX-License-Identifier: MIT
#pragma once

// SPI платы в тестах ПК: обмены реализует тест (на том конце - эмулятор
// карты), частота хранится, чтобы часы шли со скоростью линии.
#include <cstddef>
#include <cstdint>

using uint = unsigned int;

struct spi_inst_t {
    uint baud;
};
extern spi_inst_t g_test_spi1;
#define spi1 (&g_test_spi1)

enum spi_cpol_t { SPI_CPOL_0 = 0 };
enum spi_cpha_t { SPI_CPHA_0 = 0 };
enum spi_order_t { SPI_MSB_FIRST = 1 };

inline uint spi_set_baudrate(spi_inst_t* spi, uint baud) {
    return spi->baud = baud;
}
inline uint spi_get_baudrate(const spi_inst_t* spi) {
    return spi->baud;
}
inline uint spi_init(spi_inst_t* spi, uint baud) {
    return spi_set_baudrate(spi, baud);
}
inline void spi_set_format(spi_inst_t*, uint, spi_cpol_t, spi_cpha_t, spi_order_t) {}

int spi_write_read_blocking(spi_inst_t* spi, const uint8_t* src, uint8_t* dst, size_t len);
int spi_read_blocking(spi_inst_t* spi, uint8_t repeated_tx, uint8_t* dst, size_t len);
int spi_write_blocking(spi_inst_t* spi, const uint8_t* src, size_t len);
