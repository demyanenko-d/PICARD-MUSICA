// Прошивка платы. Раскладка по ядрам:
//
//   Core1 - шина Z80 без ОС: PIO+DMA+IRQ (hostlink), HostProtocol,
//   session_orchestrator_run() (загрузка трека, телеметрия). Запускается
//   из main() одной лямбдой и не возвращается.
//
//   Core0 - main() делает раннюю инициализацию (GPIO, PSRAM, такты),
//   запускает Core1 и становится ядром FreeRTOS с задачами app_task
//   (звуковая цепочка плеера) и log_task. Саму цепочку порт не строит: он
//   даёт ей вывод (player/hal/audio_out.h) и заводит задачу.
//
// Загрузка и разбор файла целиком на Core1: из ISR записи порта
// (bus_wr_isr) идут только байты команд и данных, сессию и сброс зовёт
// poll() основного потока, окно BusByteSource забирает насос в ожидании
// WFI. Кросс-ядерной блокирующей синхронизации нет: Core0 узнаёт о готовом
// треке по shared::g_song_generation и строит TrackerEngine поверх
// shared::g_song/g_track_memory.
//
// HostProtocol не потокобезопасен: весь доступ к нему (обработчики шины и
// session_orchestrator_run) на Core1.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator> // std::size

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/m33.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/watchdog.h"
#include "hardware/uart.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdio_uart.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "audio/i2s_sink.h"
#include "player/session.h"
#include "platform/log.h"
#include "platform/log_rings.h"
#include "player/audio/chain.h"
#include "player/banks.h"
#include "log.h"
#include "stack_paint.h"
#include "rtos/ticks.h"
#include "firmware_config.h"
#include "hostlink/bus.h"
#include "hostlink/divmmc.h"
#include "player/gs/bridge.h"
#include "hostlink/hostlink.h"
#include "usb/usb_host.h"
#include "psram/psram_driver.h"
#include "psram/psram_pins.h"
#include "player/shared_state.h"
#include "devices/storage/storage.h"
#include "devices/zcontroller/zcontroller.h"

#include "platform/compiler.h"
#include "player/config.h"
#include "player/protocol/host_protocol.h"
#include "core/memory/track_memory.h"

namespace {

// Физический UART отладки: пины и скорость переходника.
constexpr int kDebugUartTxPin = 32;
constexpr int kDebugUartRxPin = 33;
constexpr uint32_t kDebugUartBaudRate = 115200;

using rp2350::kRtosTickUs;
using rp2350::kSysClockKhz;
using rp2350::ticks_for_us;

// Паузы запуска: напряжение ядра до смены частоты и частота до настройки
// clk_peri.
constexpr uint32_t kVregSettleMs = 20;
constexpr uint32_t kSysClockSettleMs = 20;

// clk_peri (UART и SPI) - от clk_sys с целым делителем 1..4: 300/3 = 100 МГц.
// Делитель SPI всегда чётный; от 100 МГц карта получает 25 МГц делителем 4,
// 4 такта clk_peri на бит.
constexpr uint32_t kPeriClockDiv = 3;

player::protocol::HostProtocol s_protocol;

// Глобальный объект, а не static внутри main(): локальный static с
// нетривиальным конструктором даёт guard-код на атомиках и WFE, и вместе с
// работающим FreeRTOS вешает загрузку молча. Порядок инициализации в одной
// единице трансляции задан порядком объявления: s_protocol раньше s_orch.
player::SessionOrchestrator s_orch(s_protocol);


// Раскладка PSRAM в ядре (ядро собирается и на ПК) рассчитана на
// чип платы.
static_assert(psram::kSizeBytes == soundsinth::memory::kPsramChipBytes,
              "PSRAM платы (PICO_PSRAM_SIZE_BYTES) разошлась с kPsramChipBytes");

void reserved_pins_safe_init() {
    // RP2350 Errata E9: плавающий вход (даже без подтяжек) может защёлкнуться
    // на промежуточном напряжении и тянуть ток. Обход от Raspberry Pi - входной
    // буфер (IE) выключен и включается только перед чтением. Эти пины не
    // читаются вовсе.
    for (uint pin : bus::RESERVED_PINS) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
        gpio_set_input_enabled(pin, false);
    }

    // Блокировка ПЗУ - выход в неактивном состоянии до выхода хоста из сброса,
    // иначе первая выборка команды попадёт в погашенное ПЗУ. Плавающим входом
    // (в RESERVED_PINS) держать нельзя: на линии перемычка до платы.
    // Перемычка выбора режима читается здесь, раньше всех, кто спрашивает
    // режим. Замкнута на землю - эмуляция ПЗУ.
    bus::latch_divmmc_jumper();

    // Значение первым: gpio_init обнуляет выходной регистр, и обратный порядок
    // выдал бы ноль, то есть погасил ПЗУ работающей машины.
    gpio_init(bus::PIN_ROM_BLK_N);
    bus::release_host_rom();
    gpio_set_dir(bus::PIN_ROM_BLK_N, GPIO_OUT);

    // Хост держится в сбросе всю загрузку МК: RP2350 стартует дольше хоста, и
    // хост начал бы обращаться к шине до её готовности. bus::release_host_reset()
    // зовёт session_orchestrator_run(), когда PIO/DMA/IRQ шины настроены.
    // Значение до направления, как у ROM_BLK_N: иначе после gpio_init пин
    // несколько тактов выдавал бы 0, то есть отпускал сброс.
    gpio_init(bus::PIN_RESET_N);
    bus::assert_host_reset();
    gpio_set_dir(bus::PIN_RESET_N, GPIO_OUT);

    // NMI - вход, линию ведёт машина. Выход в единице работает встречно: в
    // режиме эмуляции ПЗУ 30-40 тысяч входов в 0x0066 в секунду. Входной
    // буфер погашен (Errata E9).
    gpio_init(bus::PIN_NMI_N);
    gpio_set_dir(bus::PIN_NMI_N, GPIO_IN);
    gpio_disable_pulls(bus::PIN_NMI_N);
    gpio_set_input_enabled(bus::PIN_NMI_N, false);
}


} // namespace

// Обязательные хуки FreeRTOS (configUSE_MALLOC_FAILED_HOOK,
// configCHECK_FOR_STACK_OVERFLOW=2):
// без них ссылки не разрешаются при линковке. Обработка минимальная:
// configASSERT -> строка -> bkpt.
extern "C" void vApplicationMallocFailedHook() { configASSERT(0); }

extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char*) { configASSERT(0); }

// configASSERT ядра FreeRTOS, os_freertos и хуки выше. Без строки assert
// кончался голым bkpt: при C_DEBUGEN = 0 - HARDFAULT с hfsr DEBUGEVT без
// места, при 1 - ядро вставало молча. pc - адрес возврата из места assert.
extern "C" SOUNDSINTH_NOINLINE void soundsinth_assert_failed(int line) {
    char m[80];
    snprintf(m, sizeof(m), "\n*** ASSERT core=%" PRIu32 " line=%d pc=%08" PRIxPTR " ***\n", get_core_num(), line,
             reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    platform::log_go_direct();
    platform::log_put_blocking(m);
    pico_default_asm_volatile("bkpt #0");
}

namespace {

// Время входа в main, для отметок t= в строках загрузки.
uint32_t s_boot_start_us;

} // namespace

// Печатает регистры отказа напрямую в UART, чтобы сбой не разбирать вслепую.
extern "C" void hardfault_handler_c(uint32_t* stacked, uint32_t exc_return) {
    uint32_t primask;
    pico_default_asm_volatile("mrs %0, primask" : "=r"(primask));

    const uint32_t cfsr = scb_hw->cfsr;
    const uint32_t hfsr = scb_hw->hfsr;
    // От 64-битного счётчика, как t= в строках работы: time_us_32() через
    // 71.6 минуты заворачивается.
    const uint32_t t_ms = to_ms_since_boot(get_absolute_time());
    const uint32_t core = get_core_num();
    // Адреса отказа годны только при своих битах CFSR.
    constexpr uint32_t kCfsrMmarValid = 1u << 7;
    const uint32_t bfar = (cfsr & M33_CFSR_BFSR_BFARVALID_BITS) ? scb_hw->bfar : 0u;
    const uint32_t mmfar = (cfsr & kCfsrMmarValid) ? scb_hw->mmfar : 0u;

    // Отказ на Core0: задача логгера больше не выполнится - сначала то, что
    // оба ядра положили в кольца перед отказом. На Core1 выгрузку колец
    // продолжает log_task.
    platform::log_go_direct();
    if (core == 0) platform::log_flush_blocking();

    char msg[192];
    // Кадр исключения ARM Cortex-M.
    enum { kR0, kR1, kR2, kR3, kR12, kLr, kPc, kXpsr };
    snprintf(msg, sizeof(msg),
             "\n*** HARDFAULT core=%" PRIu32 " primask=%" PRIu32 " t=%" PRIu32 "ms *** "
             "pc=%08" PRIx32 " lr=%08" PRIx32 " r0=%08" PRIx32 " r3=%08" PRIx32 " r12=%08" PRIx32 " "
             "xpsr=%08" PRIx32 " cfsr=%08" PRIx32 " hfsr=%08" PRIx32 "\n",
             core, primask, t_ms,
             stacked[kPc], stacked[kLr], stacked[kR0],
             stacked[kR3], stacked[kR12], stacked[kXpsr],
             cfsr, hfsr);
    platform::log_put_blocking(msg);
    // Задача FreeRTOS - только на Core0 и только после старта планировщика.
    const TaskHandle_t task = (core == 0) ? xTaskGetCurrentTaskHandle() : nullptr;
    snprintf(msg, sizeof(msg),
             "*** r1=%08" PRIx32 " r2=%08" PRIx32 " sp=%08" PRIx32 " exc_return=%08" PRIx32 " bfar=%08" PRIx32
             " mmfar=%08" PRIx32 " task=%s ***\n",
             stacked[kR1], stacked[kR2], static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stacked)), exc_return, bfar,
             mmfar, task ? pcTaskGetName(task) : "-");
    platform::log_put_blocking(msg);
    while (true) {
        tight_loop_contents();
    }
}

// Куча newlib из линкер-скрипта SDK: от конца .bss (__end__) до конца
// основного SRAM (__HeapLimit), стеки ядер выше (SCRATCH_X и SCRATCH_Y).
// malloc в образе нет - это свободная SRAM.
extern "C" char __end__;
extern "C" char __HeapLimit;

// Стеки вне задач FreeRTOS, их никто не стережёт: Core1 (SCRATCH_X) и MSP
// Core0 (SCRATCH_Y) - main до планировщика, потом прерывания.
extern "C" uint32_t __StackOneBottom, __StackOneTop, __StackBottom, __StackTop;


// Перехват HardFault: переопределяет слабый isr_hardfault из crt0.S (там
// bkpt). Кадр исключения берётся с MSP или PSP по биту 2 LR.
extern "C" __attribute__((naked)) void isr_hardfault() {
    pico_default_asm_volatile(
        "tst lr, #4 \n"
        "ite eq \n"
        "mrseq r0, msp \n"
        "mrsne r0, psp \n"
        "mov r1, lr \n" // EXC_RETURN - вторым аргументом
        "b hardfault_handler_c \n");
}

namespace {

// Миллисекунды от входа в main(), отметки t= в строках загрузки.
uint32_t boot_ms() { return (time_us_32() - s_boot_start_us) / 1000u; }

// Почему плата стартовала и какой это запуск: самопроизвольный перезапуск
// по питанию иначе выглядит как свежая загрузка. Номер запуска - в
// регистре сторожа (scratch[0]), сброс по питанию его обнуляет;
// scratch[4..7] занимает watchdog_reboot. C_DEBUGEN = 1 - отладчик держит
// ядро: bkpt при assert тогда останавливает его молча, без HARDFAULT.
void log_boot_reason() {
    struct ResetBit {
        uint32_t mask;
        const char* name;
    };
    static constexpr ResetBit kResetBits[] = {
        {POWMAN_CHIP_RESET_HAD_POR_BITS, "POR"},
        {POWMAN_CHIP_RESET_HAD_BOR_BITS, "BOR"},
        {POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS, "RUN"},
        {POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS, "DP"},
        {POWMAN_CHIP_RESET_HAD_RESCUE_BITS, "RESCUE"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS, "WDT_POWMAN_ASYNC"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS, "WDT_POWMAN"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS, "WDT_SWCORE"},
        {POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS, "SWCORE_PD"},
        {POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS, "GLITCH"},
        {POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS, "SYSRESETREQ"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_PSM_BITS, "WDT_PSM"},
    };
    const uint32_t reset = powman_hw->chip_reset;
    char why[96];
    int off = 0;
    for (const ResetBit& b : kResetBits) {
        if ((reset & b.mask) != 0 && off < static_cast<int>(sizeof(why))) {
            off += snprintf(why + off, sizeof(why) - static_cast<size_t>(off), " %s", b.name);
        }
    }
    if (off == 0) snprintf(why, sizeof(why), " -");
    const uint32_t run = ++watchdog_hw->scratch[0];
    printf("boot: сброс%s (0x%08" PRIx32 "), запуск %" PRIu32 ", C_DEBUGEN=%u\n", why, reset, run,
           static_cast<unsigned>(m33_hw->dhcsr & M33_DHCSR_C_DEBUGEN_BITS));
}

// Такты флеша и PSRAM - из регистров: окно m[0] (флеш, CS0) настраивает
// boot2, m[1] (PSRAM, CS1) - драйвер от clock_get_hz(clk_sys). Флеш и PSRAM
// делят один QMI, промах кэша XIP тем дороже, чем медленнее флеш.
void log_boot_clocks() {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const uint32_t d0 = (qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    const uint32_t d1 = (qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS) >> QMI_M1_TIMING_CLKDIV_LSB;
    const uint32_t r0 = (qmi_hw->m[0].timing & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB;
    printf("boot: QMI sys %" PRIu32 " MHz | flash div %" PRIu32 " -> %" PRIu32 " MHz (rxdelay %" PRIu32 ")"
           " | PSRAM div %" PRIu32 " -> %" PRIu32 " MHz | clk_peri %" PRIu32 " MHz\n",
           sys_hz / 1000000u,
           d0, d0 ? sys_hz / d0 / 1000000u : 0u,
           r0,
           d1, d1 ? sys_hz / d1 / 1000000u : 0u,
           clock_get_hz(clk_peri) / 1000000u);
    // Все поля окна PSRAM: их подбирают по одному на прошивку.
    const uint32_t t1 = qmi_hw->m[1].timing;
    const auto f = [t1](uint32_t bits, uint32_t lsb) { return (t1 & bits) >> lsb; };
    printf("boot: PSRAM timing 0x%08" PRIx32 ": rxdelay %" PRIu32 ", max_select %" PRIu32 ", min_deselect %" PRIu32
           ", cooldown %" PRIu32 ", pagebreak %" PRIu32 ", select_setup %" PRIu32 ", select_hold %" PRIu32 "\n",
           t1, f(QMI_M1_TIMING_RXDELAY_BITS, QMI_M1_TIMING_RXDELAY_LSB),
           f(QMI_M1_TIMING_MAX_SELECT_BITS, QMI_M1_TIMING_MAX_SELECT_LSB),
           f(QMI_M1_TIMING_MIN_DESELECT_BITS, QMI_M1_TIMING_MIN_DESELECT_LSB),
           f(QMI_M1_TIMING_COOLDOWN_BITS, QMI_M1_TIMING_COOLDOWN_LSB),
           f(QMI_M1_TIMING_PAGEBREAK_BITS, QMI_M1_TIMING_PAGEBREAK_LSB),
           f(QMI_M1_TIMING_SELECT_SETUP_BITS, QMI_M1_TIMING_SELECT_SETUP_LSB),
           f(QMI_M1_TIMING_SELECT_HOLD_BITS, QMI_M1_TIMING_SELECT_HOLD_LSB));
}

} // namespace

int main() {
    // Первым действием: сторож мог включить boot ROM, загрузчик или
    // отладочная сессия; сами мы watchdog_enable() не вызываем. Снимается
    // безусловно.
    watchdog_disable();
    // Узор на стек Core0 ниже main - запас в строке "boot: стек main" и в heap:.
    rp2350::stack_paint_below_sp(&__StackBottom);
    s_boot_start_us = time_us_32();
    systick_hw->csr = 0; // FreeRTOS перепрограммирует его на Core0 сама
    // Снять унаследованные от бутрома PendSV и SysTick pending, если есть.
    scb_hw->icsr = M33_ICSR_PENDSVCLR_BITS | M33_ICSR_PENDSTCLR_BITS;

    gpio_set_dir_in_masked64((1ull << NUM_BANK0_GPIOS) - 1u); // все GPIO во вход

    // Буфер шины выключен (OE_N = 1). Значение до направления: после gpio_init
    // выходной регистр ноль, и обратный порядок на миг включил бы буфер.
    gpio_init(bus::PIN_BUFF_OE_N);
    gpio_put(bus::PIN_BUFF_OE_N, 1);
    gpio_set_dir(bus::PIN_BUFF_OE_N, GPIO_OUT);

    // Сброс хоста, NMI, блокировка ПЗУ, неиспользуемые пины.
    reserved_pins_safe_init();

    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(kVregSettleMs);
    // Системная частота 300 МГц, выше не идти. PIO шины работает на полной
    // sysclk (clkdiv 1.0), делители PSRAM и I2S считаются от
    // clock_get_hz(clk_sys). Делитель флеша от boot2 при смене частоты не
    // меняется, и частота флеша растёт вместе с системной: на 300 МГц
    // делитель 3, то есть 100 МГц.
    set_sys_clock_khz(kSysClockKhz, true);
    sleep_ms(kSysClockSettleMs);
    // До UART: его делитель считается от текущей частоты clk_peri.
    clock_configure_int_divider(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys),
                                kPeriClockDiv);

    stdio_uart_init_full(uart0, kDebugUartBaudRate, kDebugUartTxPin, kDebugUartRxPin);
    // stdio_uart_init_full() включает RX-прерывание UART0_IRQ на этом ядре
    // (Core0). UART только на вывод, прерывание гасится: лишний источник
    // прерываний не нужен.
    uart_set_irq_enables(uart0, false, false);
    irq_set_enabled(UART0_IRQ, false);
    // printf() допустим, только пока Core1 не запущен, дальше печать через
    // debug_log().
    printf("boot: stdio_init_all + UART(GPIO32/33 @ 115200) done t=%" PRIu32 "ms\n", boot_ms());
    printf("boot: gpio failsafe done t=%" PRIu32 "ms\n", boot_ms());
    // До любых программ в блоке звука: позже база не ставится, а блок с
    // загрузки делит шина.
    rp2350::audio::i2s_sink_set_block_base();
    printf("boot: mode -- %s (jumper GPIO%u)\n",
           bus::divmmc_selected() ? "DivMMC" : "normal",
           bus::PIN_DIVMMC_JUMPER);
    log_boot_reason();
    bus::divmmc_trace_report();

    // До запуска Core1: Core1 (session_orchestrator_run) должен увидеть
    // готовую PSRAM.
    psram::psram_init();
    printf("boot: psram_init done t=%" PRIu32 "ms\n", boot_ms());

    log_boot_clocks();
    // В режиме DivMMC конец SRAM занимает карта трапов.
    const char* const sram_top =
        bus::divmmc_selected() ? reinterpret_cast<const char*>(bus::kTrapAddrMapAt) : &__HeapLimit;
    printf("boot: SRAM за .bss %u Б\n", static_cast<unsigned>(sram_top - &__end__));

    // Быстрая проверка (256 байт, не весь чип): чип подключён и отвечает.
    const uint32_t psram_errors = psram::psram_selftest();
    printf("boot: psram_selftest %s (errors=%" PRIu32 "/%" PRIu32 " words) t=%" PRIu32 "ms\n",
           psram_errors == 0 ? "PASS" : "FAIL", psram_errors, psram::kSelftestWords, boot_ms());

    // Носитель нужен и Z-Controller, и DivMMC. На Core0 до
    // multicore_launch_core1: единственное обращение Core0 к носителю.
    // Дальше им владеет цикл Core1; арбитр однопоточный, без блокировок,
    // трогать карту с двух ядер нельзя. Так же сделано с
    // track_memory_create.
    //
    // Буфер сектора не на стеке: стек Core0 до планировщика 4 КБ
    // (PICO_STACK_SIZE). Буфер упаковки паттернов трека до первой загрузки
    // свободен, а арена пуста - место на сектор есть заведомо.
    {
        uint8_t* const sector = soundsinth::memory::scratch_take(
            shared::g_track_memory.scratch, soundsinth::memory::Scratch::BootSector, devices::storage::kSectorBytes);
        static_assert(devices::storage::kSectorBytes <= soundsinth::memory::kBootSectorBytes);
        devices::storage::storage_boot_check(sector);
        soundsinth::memory::scratch_leave(shared::g_track_memory.scratch, soundsinth::memory::Scratch::BootSector);
    }
    printf(" t=%" PRIu32 "ms\n", boot_ms());

    // shared::g_track_memory: резидентная арена из статического пула
    // (platform::resident_storage_acquire) и PSRAM. До
    // multicore_launch_core1: Core1 пользуется ею с первой загрузки. Дальше
    // только track_memory_reset_for_new_track, без выделений.
    soundsinth::memory::track_memory_create(shared::g_track_memory);
    printf("boot: track_memory_create done t=%" PRIu32 "ms\n", boot_ms());

    // Банк .mid: файл на карте (до 113 МБ), иначе регион флеша (15 МБ, по
    // XIP). Где он лежит, знает только раскладка прошивки.
    player::open_banks(reinterpret_cast<const uint8_t*>(XIP_BASE + SOUNDSINTH_BANK_FLASH_OFFSET),
                       PICO_FLASH_SIZE_BYTES - SOUNDSINTH_BANK_FLASH_OFFSET);
    printf("boot: стек main свободно %" PRIu32 " из %u Б\n", rp2350::stack_unpainted_bytes(&__StackBottom, &__StackTop),
           static_cast<unsigned>((&__StackTop - &__StackBottom) * sizeof(uint32_t)));

    // На кольца лога - до запуска Core1: иначе до старта планировщика оба ядра
    // печатали бы напрямую и строки перемешивались. Строки копятся в кольце до
    // появления потребителя.
    platform::log_use_rings();

    // Core1 - шина целиком: irq_set_exclusive_handler()/irq_set_enabled()
    // регистрируют прерывания PIO на вызывающем ядре, поэтому шина
    // поднимается отсюда, и не раньше psram_init(): в прямом режиме QMI флеш
    // XIP недоступен, а с ним код обработчиков во флеше. Не возвращается.
    multicore_launch_core1([]() {
        rp2350::stack_paint_below_sp(&__StackOneBottom);
        // Перемычка включает и выключает только DivMMC. Z-Controller и синтезатор
        // работают в обоих положениях.
        //
        // Эмуляция ПЗУ поднимается первой: она захватывает автоматы PIO по явным
        // номерам, шина плагина берёт свои следом.
        if (bus::divmmc_selected()) {
            bus::rom_emu_init();
            debug_log("core1: DivMMC on, ROM emulated\n");
        }

        hostlink::init(s_protocol, player::session_orchestrator_callbacks(s_orch));
#if SOUNDSINTH_RP2350_ZCONTROLLER
        // Z-Controller поднимается в обоих положениях перемычки. Эмулятор SPI
        // один, хозяев два; у шины карты есть владелец - кто выбрал кристалл, тот
        // владеет до снятия выбора (SdOwner).
        // В режиме DivMMC эмулятор карты уже поднял divmmc_reset.
        devices::zcontroller::zcontroller_init(bus::divmmc_selected());
        debug_log("core1: z-controller on 0x57/0x77\n");
#endif
        // Эмуляция General Sound на родных портах 0xBB/0xB3 (наш протокол на
        // 0x63/0x67) - во всех сборках и в обоих положениях перемычки.
        // Регистрация портов после инициализации шины, как у остальных.
        player::gs::init();
        // USB-хост - последним: прерывание регистрируется на вызывающем
        // ядре, и приоритет ему ставится ниже всех шинных. Виток крутит
        // цикл сеанса.
        rp2350::usb::host_init();
        debug_log("core1: bus PIO/DMA/IRQ ready, entering session_orchestrator_run\n");
        player::session_orchestrator_run(s_orch);
    });

    // Core0 с этой точки - ядро FreeRTOS синтезатора.
    // log_task - единственный, кто трогает UART после старта планировщика.
    // Приоритет как у app_task, ниже рендера
    // (SOUNDSINTH_RENDER_TASK_PRIORITY = IDLE+2): лог не отбирает время у
    // звука.
    // Стек 2048 слов: буферы строк до 320 байт и snprintf.
    xTaskCreate(rp2350::log_task, "log_task", 2048, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    TaskHandle_t app = nullptr;
    xTaskCreate(player::audio_chain_task, "app_task", 4096, nullptr, tskIDLE_PRIORITY + 1, &app);
    rp2350::log_task_set_app_handle(app);
    debug_log("boot: app_task created, entering FreeRTOS scheduler (Core0)\n");
    vTaskStartScheduler(); // никогда не возвращается

    for (;;) {} // недостижимо
}
