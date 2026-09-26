// Шина SPI под карту (devices/hal/spi.h): выводы платы и блок SPI1.

#include "devices/hal/spi.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "platform/hot_path.h"

namespace {

// Подключение по таблице функций pico-sdk (io_bank0.h):
//   GPIO40 SPI1_RX   - вывод 7 карты, DAT0 (MISO)
//   GPIO41 SPI1_SS_N - вывод 2, CD/DAT3 (CS), ведётся как обычный GPIO
//   GPIO42 SPI1_SCLK - вывод 5, CLK
//   GPIO43 SPI1_TX   - вывод 3, CMD (MOSI)
constexpr uint kPinMiso = 40;
constexpr uint kPinCs = 41;
constexpr uint kPinSck = 42;
constexpr uint kPinMosi = 43;

spi_inst_t* const kSpi = spi1;

} // namespace

uint32_t devices::hal::spi_open(uint32_t hz) {
    spi_init(kSpi, hz);
    spi_set_format(kSpi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(kPinMiso, GPIO_FUNC_SPI);
    gpio_set_function(kPinSck, GPIO_FUNC_SPI);
    gpio_set_function(kPinMosi, GPIO_FUNC_SPI);
    gpio_pull_up(kPinMiso);
    gpio_init(kPinCs);
    gpio_set_dir(kPinCs, GPIO_OUT);
    gpio_put(kPinCs, 1);
    return spi_get_baudrate(kSpi);
}

uint32_t devices::hal::spi_set_hz(uint32_t hz) { return spi_set_baudrate(kSpi, hz); }

SOUNDSINTH_HOT_PATH_ATTR("spi_select")
void devices::hal::spi_select(bool on) { gpio_put(kPinCs, on ? 0 : 1); }

SOUNDSINTH_HOT_PATH_ATTR("spi_xfer")
uint8_t devices::hal::spi_xfer(uint8_t out) {
    uint8_t in = 0;
    spi_write_read_blocking(kSpi, &out, &in, 1);
    return in;
}

SOUNDSINTH_HOT_PATH_ATTR("spi_read")
void devices::hal::spi_read(uint8_t idle, uint8_t* dst, uint32_t n) {
    spi_read_blocking(kSpi, idle, dst, n);
}

SOUNDSINTH_HOT_PATH_ATTR("spi_write")
void devices::hal::spi_write(const uint8_t* src, uint32_t n) {
    spi_write_blocking(kSpi, src, n);
}
