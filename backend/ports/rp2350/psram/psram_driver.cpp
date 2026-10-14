// SPDX-License-Identifier: MIT
#include "psram_driver.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico.h"

#include "psram_pins.h"

namespace psram {

namespace {

// Команды APS6404L.
constexpr uint32_t kCmdQuadEnd     = 0xf5u; // выйти из QPI
constexpr uint32_t kCmdResetEnable = 0x66u;
constexpr uint32_t kCmdReset       = 0x99u;
constexpr uint32_t kCmdEnterQpi    = 0x35u;
constexpr uint32_t kCmdQuadRead    = 0xebu; // быстрое чтение в quad, с ожиданием
constexpr uint32_t kCmdQuadWrite   = 0x38u;

// Делитель SCK прямого режима: команды сброса идут медленно, на любой sys.
constexpr uint32_t kDirectClkdiv = 30;
// Пауза CS high после команды прямого режима, в командах nop.
constexpr int kCsHighNops = 20;

// Одна команда прямого режима: CS1 вниз, слово в direct_tx, ждать конца,
// CS1 вверх, пауза, снять принятое. Только встраиваемая: в прямом режиме
// окно флеша закрыто, и вызов во флеш - отказ.
__force_inline void direct_cmd(uint32_t tx) {
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx   = tx;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    for (int i = 0; i < kCsHighNops; ++i) {
        __asm volatile("nop");
    }
    (void)qmi_hw->direct_rx;
}

} // namespace

// Целиком из SRAM (__not_in_flash_func): в direct mode окно флеш XIP
// (M0) заблокировано, выборка из 0x10xxxxxx - fault. Тайминги считаются
// до входа в direct mode.
void __not_in_flash_func(psram_init)() {
    gpio_set_function(kCsGpio, GPIO_FUNC_XIP_CS1);

    const uint32_t sys_hz = clock_get_hz(clk_sys);

    // Потолок SCK выше паспорта APS6404L (109 МГц при 3.3 В): при sys 300
    // SCK 150 МГц. Делитель округляется вверх. При зависаниях и битых
    // сэмплах - 109 МГц или sys 327. 156 МГц - граница, при которой делитель
    // 2 держится до sys 312 МГц.
    const uint32_t max_sck_hz = 156000000u;
    uint32_t clkdiv           = (sys_hz + max_sck_hz - 1u) / max_sck_hz;
    if (clkdiv < 2u) clkdiv = 2u;
    if (clkdiv > 255u) clkdiv = 255u;

    // RXDELAY - сдвиг выборки в полутактах sys, от clkdiv; +1 выше 100 МГц
    // SCK и ещё +1 выше 133 МГц - пороги из опыта пользователей RP2350, не
    // из паспорта.
    uint32_t rxdelay = clkdiv;
    if (sys_hz / clkdiv > 100000000u) rxdelay += 1u;
    if (sys_hz / clkdiv > 133000000u) rxdelay += 1u;

    const uint32_t fs_per_cycle = static_cast<uint32_t>(1000000000000000ull / sys_hz);

    // MAX_SELECT ограничивает удержание CS (единица - 64 такта), чтобы чип
    // успевал регенерацию DRAM. 7 мкс, а не 8: идущее к истечению обращение
    // QMI дожидается, и паспорт велит закладывать это время сверху.
    const uint64_t kMaxSelectFs = 7000000000ull; // 7 мкс в фемтосекундах
    const uint8_t max_select    = static_cast<uint8_t>(kMaxSelectFs / (64ull * fs_per_cycle));

    // MIN_DESELECT: tCPH >= 50 нс, единица - tSYS, округление вверх.
    const uint8_t min_deselect = static_cast<uint8_t>((50000000u + fs_per_cycle - 1u) / fs_per_cycle);

    const uint32_t intr_stash = save_and_disable_interrupts();

    qmi_hw->direct_csr = (kDirectClkdiv << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }

    // Шаг 0: QUAD_END в quad-ширине - выйти из QPI, если чип уже в нём после
    // тёплого сброса (PSRAM при сбросе RP2350 не сбрасывается).
    direct_cmd(QMI_DIRECT_TX_OE_BITS | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) | kCmdQuadEnd);

    // RSTEN -> RST -> EQI в режиме SPI (1 бит).
    direct_cmd(kCmdResetEnable);
    direct_cmd(kCmdReset);
    direct_cmd(kCmdEnterQpi);

    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
    restore_interrupts(intr_stash);

    // --- Настройка QMI CS1 для непрерывного доступа XIP ---

    qmi_hw->m[1].timing = (clkdiv << QMI_M1_TIMING_CLKDIV_LSB) | (rxdelay << QMI_M1_TIMING_RXDELAY_LSB) | (1u << QMI_M1_TIMING_COOLDOWN_LSB) |
                          (max_select << QMI_M1_TIMING_MAX_SELECT_LSB) | (min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB) |
                          (3u << QMI_M1_TIMING_SELECT_HOLD_LSB) | (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB);

    qmi_hw->m[1].rfmt = (QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB) | (QMI_M1_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_RFMT_SUFFIX_LEN_LSB) |
                        (QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB) | (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
                        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB) | (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB) |
                        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB) | (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB);

    qmi_hw->m[1].rcmd = (kCmdQuadRead << QMI_M1_RCMD_PREFIX_LSB);

    qmi_hw->m[1].wfmt = (QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB) | (QMI_M1_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_WFMT_SUFFIX_LEN_LSB) |
                        (QMI_M1_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M1_WFMT_DUMMY_LEN_LSB) | (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
                        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB) | (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB) |
                        (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB) | (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB);

    qmi_hw->m[1].wcmd = (kCmdQuadWrite << QMI_M1_WCMD_PREFIX_LSB);

    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
}

void __not_in_flash_func(psram_flush)() {
    __dsb();
    xip_cache_clean_all();
}

namespace {

// hash16: детерминированный, равномерно распределяет 65536 значений.
uint16_t hash16(uint16_t x) {
    x = static_cast<uint16_t>(x ^ (x >> 7u));
    x = static_cast<uint16_t>(x * 0x4f1bu);
    x = static_cast<uint16_t>(x ^ (x >> 8u));
    return x;
}

// Проверяется 256 байт, а не весь чип (4194304 слова, около 2.1 с
// загрузки): запись и чтение тем же путём XIP на малом диапазоне ловят
// неподключённый или не отвечающий чип.
constexpr uint32_t kWordCount = kSelftestWords;
static_assert(kWordCount * sizeof(uint16_t) == 256u, "the selftest is 256 bytes");

// Во флеше, как и psram_selftest: прямого режима QMI самотест не включает,
// ходит через XIP-алиас. Запись в M1 разрешила psram_init.
void selftest_write(uint16_t start) {
    volatile uint16_t* base = reinterpret_cast<volatile uint16_t*>(kXipBase);
    uint16_t counter        = start;
    for (uint32_t j = 0; j < kWordCount; ++j) {
        base[j] = hash16(counter++);
    }
    __dsb();
    xip_cache_clean_all();
}

// Проверка через тот же кэшируемый алиас, что и запись: xip_cache_clean_all()
// на RP2350 заодно инвалидирует строки (обход RP2350-E11 в pico-sdk), и
// чтение идёт в PSRAM. С xip_cache_clean_range() проверка читала бы кэш.
uint32_t selftest_verify(uint16_t start) {
    volatile uint16_t* base = reinterpret_cast<volatile uint16_t*>(kXipBase);
    uint16_t counter        = start;
    uint32_t errors         = 0;
    for (uint32_t j = 0; j < kWordCount; ++j) {
        if (base[j] != hash16(counter++)) ++errors;
    }
    return errors;
}

} // namespace

uint32_t psram_selftest() {
    constexpr uint16_t kStart = 0x1234; // начало последовательности hash16
    selftest_write(kStart);
    return selftest_verify(kStart);
}

} // namespace psram
