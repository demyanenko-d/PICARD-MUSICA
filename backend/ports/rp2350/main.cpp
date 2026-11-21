// SPDX-License-Identifier: MIT
// Прошивка платы. Раскладка по ядрам:
//
//   Core1 - шина Z80 без ОС: PIO+DMA+IRQ (hostlink), HostProtocol,
//   session_orchestrator_run() (загрузка трека, телеметрия). Запускается
//   из main() одной лямбдой и не возвращается.
//
//   Core0 - main() делает раннюю инициализацию (GPIO, PSRAM, такты),
//   запускает Core1 и становится ядром FreeRTOS с задачами app_task
//   (звуковая цепочка плеера) и log_task. Саму цепочку порт не строит: он
//   даёт ей вывод и заводит задачу.
//
// Загрузка и разбор файла целиком на Core1: из ISR записи порта
// (bus_wr_isr) идут только байты команд и данных, сессию и сброс зовёт
// poll() основного потока, окно BusByteSource ждёт, обслуживая шину
// и карту. Кросс-ядерной блокирующей синхронизации нет: Core0 узнаёт о готовом
// треке по shared::g_song_generation и строит TrackerEngine поверх
// shared::g_song/g_track_memory.
//
// HostProtocol не потокобезопасен: весь доступ к нему (обработчики шины и
// session_orchestrator_run) на Core1.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator> // std::size
#include <new>

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
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdio_uart.h"
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "platform/boot_mode.h"
#include "platform/compiler.h"
#include "platform/config_flash.h"
#include "platform/debug_ring.h"
#include "platform/log.h"
#include "platform/log_rings.h"
#include "platform/memory.h"
#include "platform/mono_time.h"
#include "platform/usb_host.h"

#include "core/config/config_store.h"
#include "core/memory/track_memory.h"

#include "devices/storage/storage.h"
#include "devices/divmmc_card/divmmc_card.h"
#include "devices/zcontroller/zcontroller.h"
#include "devices/bdi/bdi.h"

#include "player/config/config_page.h"
#include "player/config/config_apply.h"
#include "player/config/config_service.h"
#include "player/config/config_term_service.h"
#include "player/nav/nav.h"
#include "player/audio/chain.h"
#include "player/banks.h"
#include "player/config.h"
#include "player/gs/bridge.h"
#include "player/hal/host_link.h"
#include "player/protocol/host_protocol.h"
#include "player/session.h"
#include "player/shared_state.h"

#include "wifi/esp_link.h"
#include "audio/i2s_sink.h"
#include "firmware_config.h"
#include "hal/boot_save.h"
#include "hal/host_signals.h"
#include "hostlink/bus.h"
#include "hostlink/divmmc.h"
#include "hostlink/hostlink.h"
#include "log.h"
#include "psram/psram_driver.h"
#include "psram/psram_pins.h"
#include "rtos/ticks.h"
#include "stack_paint.h"
#include "usb/usb_host.h"

namespace {

// Физический UART отладки: пины и скорость переходника.
constexpr int kDebugUartTxPin         = 32;
constexpr int kDebugUartRxPin         = 33;
constexpr uint32_t kDebugUartBaudRate = 115200;

using rp2350::kSysClockKhz;

// Паузы запуска: напряжение ядра до смены частоты и частота до настройки
// clk_peri.
constexpr uint32_t kVregSettleMs     = 20;
constexpr uint32_t kSysClockSettleMs = 20;

// clk_peri (UART и SPI) - от clk_sys с целым делителем 1..4: 150 МГц. Делитель
// SPI всегда чётный: карта получает 37.5, 25, 18.75, 15, 12.5 МГц делителями
// 4..12 (при двух тактах на бит около 1% секторов снимается повтором CRC).
// На 300 МГц UART модуля Wi-Fi не работает.
constexpr uint32_t kPeriClockDiv = 2;

player::protocol::HostProtocol s_protocol;

// Глобальный объект, а не static внутри main(): локальный static с
// нетривиальным конструктором даёт guard-код на атомиках и WFE, и вместе с
// работающим FreeRTOS вешает загрузку молча. Порядок инициализации в одной
// единице трансляции задан порядком объявления: s_protocol раньше s_orch.
player::SessionOrchestrator s_orch(s_protocol);

// Раскладка PSRAM в ядре (ядро собирается и на ПК) рассчитана на
// чип платы.
static_assert(psram::kSizeBytes == soundsinth::memory::kPsramChipBytes, "The board PSRAM (PICO_PSRAM_SIZE_BYTES) disagrees with kPsramChipBytes");

bool s_boot_held = false; // машина удержана в сбросе с начала загрузки
bool s_bdi       = false; // Beta Disk платы: решает загрузка, поднимает Core1

// Файлы ПЗУ с карты для конфигуратора: собираются до второго ядра, в
// страницу ложатся им.
// Файлы ПЗУ для диалога setup - блок PSRAM, только при загрузке в setup.
soundsinth::config::ConfigPageFiles* s_rom_files = nullptr;

// Держать ли машину в сбросе на загрузке платы. Решается до разбора
// настроек: вывод сброса настраивается первым делом. При RESET_SIGNAL=0
// сброс не выдаётся вовсе, и перезапуск по Ctrl+Alt+Del перезапускает
// только плату. Конфигуратор - исключение: войти в него без сброса машины
// нельзя. Порядок источников - как у загрузки: запрос прошлой загрузки,
// перемычка, блок во флеше, умолчания. Настройки с карты разбираются позже
// и на это решение не влияют: запись их во флеш делает загрузка.
bool boot_holds_host() {
    using soundsinth::config::Settings;
    platform::BootRequest req;
    if (platform::boot_request_peek(req) && req.configurator) return true;
    if (platform::boot_jumper_closed()) return true;
    Settings s;
    const uint8_t* const page0 = platform::config_flash_view(0);
    const uint8_t* const page1 = platform::config_flash_view(1);
    const int cur              = soundsinth::config::store_pick(page0, page1);
    if (cur >= 0) soundsinth::config::page_read(cur == 0 ? page0 : page1, s);
    soundsinth::config::settings_clamp(s);
    return s.reset_signal != 0;
}

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

    // Значение первым: gpio_init обнуляет выходной регистр, и обратный порядок
    // выдал бы ноль, то есть погасил ПЗУ работающей машины.
    gpio_init(bus::PIN_ROM_BLK_N);
    bus::release_host_rom();
    gpio_set_dir(bus::PIN_ROM_BLK_N, GPIO_OUT);

    // Признак ПЗУ TR-DOS - тем же порядком и по той же причине: за ним вход
    // машины, плавающим его оставлять нельзя.
    gpio_init(bus::PIN_DOS_N);
    bus::release_host_trdos();
    gpio_set_dir(bus::PIN_DOS_N, GPIO_OUT);

    // Хост держится в сбросе всю загрузку МК: RP2350 стартует дольше хоста, и
    // хост начал бы обращаться к шине до её готовности. bus::release_host_reset()
    // зовёт session_orchestrator_run(), когда PIO/DMA/IRQ шины настроены.
    // Значение до направления, как у ROM_BLK_N: иначе после gpio_init пин
    // несколько тактов выдавал бы 0, то есть отпускал сброс.
    // При RESET_SIGNAL=0 машина не держится: работает, пока плата грузится.
    gpio_init(bus::PIN_RESET_N);
    s_boot_held = boot_holds_host();
    if (s_boot_held) {
        bus::assert_host_reset();
    } else {
        bus::release_host_reset();
    }
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

// Обязательный хук FreeRTOS (configCHECK_FOR_STACK_OVERFLOW=2): без него
// ссылка не разрешается при линковке. Обработка минимальная:
// configASSERT -> строка -> bkpt. Крючка отказа выделения нет - нет и кучи.
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char*) {
    configASSERT(0);
}

// configASSERT ядра FreeRTOS, os_freertos и хуки выше. Без строки assert
// остаётся голый bkpt: при C_DEBUGEN = 0 - HARDFAULT с hfsr DEBUGEVT без
// места, при 1 - ядро встаёт молча. pc - адрес возврата из места assert.
extern "C" SOUNDSINTH_NOINLINE void soundsinth_assert_failed(int line) {
    char m[80];
    snprintf(m, sizeof(m), "\n*** ASSERT core=%u line=%d pc=%08" PRIxPTR " ***\n", static_cast<unsigned>(get_core_num()), line,
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
    const uint32_t bfar               = (cfsr & M33_CFSR_BFSR_BFARVALID_BITS) ? scb_hw->bfar : 0u;
    const uint32_t mmfar              = (cfsr & kCfsrMmarValid) ? scb_hw->mmfar : 0u;

    platform::log_go_direct();

    char msg[192];
    // Кадр исключения ARM Cortex-M.
    enum { kR0, kR1, kR2, kR3, kR12, kLr, kPc, kXpsr };
    snprintf(msg, sizeof(msg),
             "\n*** HARDFAULT core=%" PRIu32 " primask=%" PRIu32 " t=%" PRIu32 "ms *** "
             "pc=%08" PRIx32 " lr=%08" PRIx32 " r0=%08" PRIx32 " r3=%08" PRIx32 " r12=%08" PRIx32 " "
             "xpsr=%08" PRIx32 " cfsr=%08" PRIx32 " hfsr=%08" PRIx32 "\n",
             core, primask, t_ms, stacked[kPc], stacked[kLr], stacked[kR0], stacked[kR3], stacked[kR12], stacked[kXpsr], cfsr, hfsr);
    platform::log_put_blocking(msg);
    // Задача FreeRTOS - только на Core0 и только после старта планировщика.
    const TaskHandle_t task = (core == 0) ? xTaskGetCurrentTaskHandle() : nullptr;
    snprintf(msg, sizeof(msg),
             "*** r1=%08" PRIx32 " r2=%08" PRIx32 " sp=%08" PRIx32 " exc_return=%08" PRIx32 " bfar=%08" PRIx32 " mmfar=%08" PRIx32 " task=%s ***\n",
             stacked[kR1], stacked[kR2], static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stacked)), exc_return, bfar, mmfar,
             task ? pcTaskGetName(task) : "-");
    platform::log_put_blocking(msg);
    // Отказ на Core0: задача логгера больше не выполнится - выгрузить то, что
    // оба ядра положили в кольца перед отказом; регистры уже напечатаны. Кольца
    // в PSRAM: отказ шины в окне XIP повторился бы на чтении колец. На Core1
    // выгрузку продолжает log_task.
    if (core == 0 && (bfar >> 28) != 1u) platform::log_flush_blocking();
    while (true) {
        tight_loop_contents();
    }
}

// Хвост SRAM за .bss: от __end__ до конца основного SRAM (__HeapLimit),
// стеки ядер выше (SCRATCH_X и SCRATCH_Y). Кучи нет (PICO_HEAP_SIZE=0) -
// это свободная SRAM.
extern "C" char __end__;
extern "C" char __HeapLimit;
// Конец образа во флеше (линкер-скрипт SDK). Нужен сторожу раскладки флеша:
// доросший образ стёр бы блоки настроек, и первая же запись настроек убила
// бы гранулу собственного кода - при этом вернув "записано", потому что
// сверка читает ровно то, что просили записать.
extern "C" char __flash_binary_end;

// Стеки вне задач FreeRTOS, их никто не стережёт: Core1 (SCRATCH_X) и MSP
// Core0 (SCRATCH_Y) - main до планировщика, потом прерывания.
extern "C" uint32_t __StackOneBottom, __StackOneTop, __StackBottom, __StackTop;
extern "C" uint8_t __busmem_start__[], __busmem_end__[];
extern "C" uint8_t __bdi_code_start__[], __bdi_code_end__[], __bdi_code_source__[];

// Перехват HardFault: переопределяет слабый isr_hardfault из crt0.S (там
// bkpt). Кадр исключения берётся с MSP или PSP по биту 2 LR.
extern "C" __attribute__((naked)) void isr_hardfault() {
    pico_default_asm_volatile("tst lr, #4 \n"
                              "ite eq \n"
                              "mrseq r0, msp \n"
                              "mrsne r0, psp \n"
                              "mov r1, lr \n" // EXC_RETURN - вторым аргументом
                              "b hardfault_handler_c \n");
}

namespace {

// Миллисекунды от входа в main(), отметки t= в строках загрузки.
uint32_t boot_ms() {
    return (time_us_32() - s_boot_start_us) / 1000u;
}

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
    printf("boot: reset%s (0x%08" PRIx32 "), run %" PRIu32 ", C_DEBUGEN=%u\n", why, reset, run,
           static_cast<unsigned>(m33_hw->dhcsr & M33_DHCSR_C_DEBUGEN_BITS));
}

// Настройки: откуда взяты и что сделано с картой. Строка одна - смотреть
// её будут при каждом непонятном поведении платы.
void log_config_boot(const player::config::BootReport& r) {
    const soundsinth::config::Settings& s = player::config::settings();
    printf("boot: settings %s", r.flash_valid ? "from flash" : "DEFAULTS (no valid block in flash)");
    if (r.file_applied) {
        printf(", set_config.txt: applied %u, unknown %u, invalid %u", static_cast<unsigned>(r.applied), static_cast<unsigned>(r.unknown),
               static_cast<unsigned>(r.bad));
        if (r.file_kept) printf(", NOT RENAMED (applies again next boot)");
    }
    if (r.from_configurator) printf(", saved by the configurator");
    if (r.gui_invite) printf(", open.gui present");
    if (r.flash_written) {
        // С причиной: настройки теряются молча, и по журналу должно быть
        // видно, кто их переписал.
        printf(", block %u written (%s)", static_cast<unsigned>(r.slot), r.file_applied ? "set_config.txt" : "no valid block");
    } else if (!r.flash_valid || r.applied != 0) {
        printf(", BLOCK WRITE FAILED");
    }
    if (r.file_saved) printf(", get_config.done saved");
    printf("\n");
    printf("boot: settings by step, ms: mount %u, read %u, flash %u, write %u, rename %u,"
           " unmount %u\n",
           static_cast<unsigned>(r.mount_ms), static_cast<unsigned>(r.set_ms), static_cast<unsigned>(r.flash_ms), static_cast<unsigned>(r.save_write_ms),
           static_cast<unsigned>(r.save_rename_ms), static_cast<unsigned>(r.unmount_ms));
    static const char* const kDiskSys[] = {"none", "divmmc", "trdos"};
    static_assert(std::size(kDiskSys) == static_cast<size_t>(soundsinth::config::DiskSys::TrDos) + 1u, "a name for every disk system");
    static const char* const kLiveMidi[] = {"none", "hook", "wire"};
    static const char* const kMedia[]    = {"sd", "usb", "none"};
    static_assert(std::size(kMedia) == static_cast<size_t>(soundsinth::config::Media::None) + 1u, "a name for every medium");
    printf("boot: disk %s, divmmc cards %s %s, z-controller %s, gs %u, live midi %s, reset %u, wifi %u\n", kDiskSys[s.disksys], kMedia[s.divmmc_card1],
           kMedia[s.divmmc_card2], kMedia[s.zcontroller], static_cast<unsigned>(s.gs), kLiveMidi[s.live_midi], static_cast<unsigned>(s.reset_signal),
           static_cast<unsigned>(s.wifi));
    printf("boot: keyboard %u, mouse %u (speed %u), gamepad %u\n", static_cast<unsigned>(s.keyboard_layout), static_cast<unsigned>(s.usb_mouse),
           static_cast<unsigned>(s.mouse_speed), static_cast<unsigned>(s.usb_gamepad));
}

// Такты флеша и PSRAM - из регистров: окно m[0] (флеш, CS0) настраивает
// boot2, m[1] (PSRAM, CS1) - драйвер от clock_get_hz(clk_sys). Флеш и PSRAM
// делят один QMI, промах кэша XIP тем дороже, чем медленнее флеш.
void log_boot_clocks() {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const uint32_t d0     = (qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    const uint32_t d1     = (qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS) >> QMI_M1_TIMING_CLKDIV_LSB;
    const uint32_t r0     = (qmi_hw->m[0].timing & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB;
    printf("boot: QMI sys %" PRIu32 " MHz | flash div %" PRIu32 " -> %" PRIu32 " MHz (rxdelay %" PRIu32 ")"
           " | PSRAM div %" PRIu32 " -> %" PRIu32 " MHz | clk_peri %" PRIu32 " MHz\n",
           sys_hz / 1000000u, d0, d0 ? sys_hz / d0 / 1000000u : 0u, r0, d1, d1 ? sys_hz / d1 / 1000000u : 0u, clock_get_hz(clk_peri) / 1000000u);
    // Все поля окна PSRAM: их подбирают по одному на прошивку.
    const uint32_t t1 = qmi_hw->m[1].timing;
    const auto f      = [t1](uint32_t bits, uint32_t lsb) { return (t1 & bits) >> lsb; };
    printf("boot: PSRAM timing 0x%08" PRIx32 ": rxdelay %" PRIu32 ", max_select %" PRIu32 ", min_deselect %" PRIu32 ", cooldown %" PRIu32 ", pagebreak %" PRIu32
           ", select_setup %" PRIu32 ", select_hold %" PRIu32 "\n",
           t1, f(QMI_M1_TIMING_RXDELAY_BITS, QMI_M1_TIMING_RXDELAY_LSB), f(QMI_M1_TIMING_MAX_SELECT_BITS, QMI_M1_TIMING_MAX_SELECT_LSB),
           f(QMI_M1_TIMING_MIN_DESELECT_BITS, QMI_M1_TIMING_MIN_DESELECT_LSB), f(QMI_M1_TIMING_COOLDOWN_BITS, QMI_M1_TIMING_COOLDOWN_LSB),
           f(QMI_M1_TIMING_PAGEBREAK_BITS, QMI_M1_TIMING_PAGEBREAK_LSB), f(QMI_M1_TIMING_SELECT_SETUP_BITS, QMI_M1_TIMING_SELECT_SETUP_LSB),
           f(QMI_M1_TIMING_SELECT_HOLD_BITS, QMI_M1_TIMING_SELECT_HOLD_LSB));
}

} // namespace

int main() {
    // Первым действием: сторож мог включить boot ROM, загрузчик или
    // отладочная сессия; сами мы watchdog_enable() не вызываем. Снимается
    // безусловно.
    watchdog_disable();
    // Память шины вне .bss, crt0 её не обнуляет; нулевая запись таблицы
    // портов значит "порт не наш".
    std::memset(__busmem_start__, 0, static_cast<size_t>(__busmem_end__ - __busmem_start__));
    // Узор на стек Core0 ниже main - запас в строках "boot: main stack free" и "stack free:".
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
    clock_configure_int_divider(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys), kPeriClockDiv);

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
    log_boot_reason();
#if SOUNDSINTH_DIVMMC_TRACE
    // 128 строк на 115200 - 440 мс, и печать до планировщика блокирующая.
    bus::divmmc_trace_report();
#endif

    // До запуска Core1: Core1 (session_orchestrator_run) должен увидеть
    // готовую PSRAM.
    psram::psram_init();
    printf("boot: psram_init done t=%" PRIu32 "ms\n", boot_ms());

    log_boot_clocks();
    // Хвост SRAM за .bss, кучи нет.
    printf("boot: SRAM past .bss %u B\n", static_cast<unsigned>(&__HeapLimit - &__end__));

    // Образ обязан кончаться ниже областей своих ПЗУ (за ними настройки);
    // запас - в журнал.
    {
        const char* end   = &__flash_binary_end;
        const char* slots = reinterpret_cast<const char*>(XIP_BASE + SOUNDSINTH_ROM_SLOTS_FLASH_OFFSET);
        if (end >= slots) panic("the image grew into the ROM slots: writing a ROM would erase its own code");
        printf("boot: flash before ROM slots %u B\n", static_cast<unsigned>(slots - end));
    }

    // 256 байт подряд, не весь чип: вопрос один - держит ли PSRAM данные на
    // текущей частоте (SCK 150 МГц против паспортных 109). Разводку он не
    // проверяет и не должен: загрузка платы и без того долгая.
    const uint32_t psram_errors = psram::psram_selftest();
    printf("boot: psram_selftest %s (errors=%" PRIu32 "/%" PRIu32 " words) t=%" PRIu32 "ms\n", psram_errors == 0 ? "PASS" : "FAIL", psram_errors,
           psram::kSelftestWords, boot_ms());

    // shared::g_track_memory: резидентная арена из статического пула
    // (platform::resident_storage_acquire) и PSRAM. До всех, кто берёт
    // буфер сценарием: без неё у сценариев нет арены, и scratch_take
    // отдаёт nullptr. До multicore_launch_core1: Core1 пользуется ею с
    // первой загрузки. Дальше только track_memory_reset_for_new_track,
    // без выделений.
    soundsinth::memory::track_memory_create(shared::g_track_memory);
    printf("boot: track_memory_create done t=%" PRIu32 "ms\n", boot_ms());

    // Блочный кэш носителей - в PSRAM: обращений к нему на порядки меньше,
    // чем к сэмплам, а SRAM дороже. Берётся до первого трека, чтобы лечь с
    // краю чипа и не разрезать середину.
    {
        uint8_t* const blocks = soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, devices::storage::kBlockCacheBytes);
        devices::storage::storage_attach_block_cache(blocks, devices::storage::kBlockCacheBytes, &platform::psram_write_alias);
        printf("boot: block cache %" PRIu32 " KB in PSRAM: %s\n", devices::storage::kBlockCacheBytes / 1024u, blocks != nullptr ? "yes" : "NO ROOM");
    }

    // Носитель нужен и Z-Controller, и DivMMC. На Core0 до
    // multicore_launch_core1: единственное обращение Core0 к носителю.
    // Дальше им владеет цикл Core1; арбитр однопоточный, без блокировок,
    // трогать карту с двух ядер нельзя.
    //
    // Буфер сектора не на стеке: стек Core0 до планировщика 4 КБ
    // (PICO_STACK_SIZE). Буфер упаковки паттернов трека до первой загрузки
    // свободен, а арена пуста - место на сектор есть заведомо.
    {
        uint8_t* const sector =
            soundsinth::memory::scratch_take(shared::g_track_memory.scratch, soundsinth::memory::Scratch::BootSector, devices::storage::kSectorBytes);
        static_assert(devices::storage::kSectorBytes <= soundsinth::memory::kBootSectorBytes);
        devices::storage::storage_boot_check(sector);
        soundsinth::memory::scratch_leave(shared::g_track_memory.scratch, soundsinth::memory::Scratch::BootSector);
    }
    printf(" t=%" PRIu32 "ms\n", boot_ms());

    // Настройки: два блока флеша плюс файлы на карте. До банка, потому что
    // банк монтирует том для себя и держит его открытым, а записи на карту
    // настройкам нужны свои. До подъёма шины и второго ядра: запись во
    // флеш на время стирания отнимает XIP.
    const player::config::BootReport boot = player::config::config_boot(shared::g_track_memory.scratch);
    log_config_boot(boot);

    // Кольцо приёма Wi-Fi: модуль шлёт ответ без ожидания.
    if (player::config::settings().wifi != 0) {
        uint8_t* const ring = soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, rp2350::wifi::kLinkBlockBytes);
        rp2350::wifi::esp_link_attach(ring);
        printf("boot: wifi receive ring %" PRIu32 " KB in PSRAM: %s\n", rp2350::wifi::kBodyRingBytes / 1024u, ring != nullptr ? "yes" : "no");
    }
    printf("boot: settings parsed t=%" PRIu32 "ms\n", boot_ms());
    player::config::config_apply(player::config::settings());
    // /WAIT через SIO - только при Wi-Fi; в TR-DOS вывод берёт автомат
    // port_join_hold.
    if (player::config::settings().wifi != 0) bus::wait_line_init();
    // Сброс по F12 и Ctrl+Alt+Del - только при RESET_SIGNAL != 0.
    rp2350::hal::host_signals_allow_reset(player::config::settings().reset_signal != 0);
    // Конфигуратор настроек - режим загрузки, и решается он здесь, до
    // выбора режима шины. Входа три: запрос от клавиш Win+F12, перемычка на
    // плате и файл-приглашение на карте.
    const bool jumper      = platform::boot_jumper_closed();
    const bool want_config = boot.configurator || jumper || boot.gui_invite;
    if (want_config) {
        debug_logf("boot: configurator (%s)\n", boot.configurator ? "key" : (jumper ? "jumper" : "open.gui"));
        // Ключ дисковой системы не спрашивается: подстановка нужна любой
        // машине, в том числе той, у которой своя (DISKSYS=NONE).
        bus::config_rom_set_enabled(true);
        // Файлы ПЗУ для диалога выбора - сейчас: дальше карта у второго ядра.
        void* const files = soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, sizeof(soundsinth::config::ConfigPageFiles));
        if (files != nullptr) {
            s_rom_files = new (files) soundsinth::config::ConfigPageFiles{};
            player::config::rom_files_scan(shared::g_track_memory.scratch, *s_rom_files);
        }
        debug_logf("boot: configurator ROM files %u\n", s_rom_files != nullptr ? static_cast<unsigned>(s_rom_files->count) : 0u);
        // Меню конфигуратора на плате - тоже PSRAM и тоже только здесь.
        player::config::config_term_reserve(soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, player::config::config_term_mem_bytes()));
        // Утилита USB HID TEST: копии дескрипторов и отчётов - только здесь.
        rp2350::usb::hid_diag_attach(soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, rp2350::usb::hid_diag_bytes()));
        // Инструменты модуля Wi-Fi держат своё в кольце приёма: без сети оно
        // свободно, а при выключенном Wi-Fi его ещё нет.
        if (player::config::settings().wifi == 0) {
            rp2350::wifi::esp_link_attach(soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, rp2350::wifi::kLinkBlockBytes));
        }
        // Сброс машины обязателен даже при RESET_SIGNAL=0: иначе войти
        // можно было бы только снятием питания, а это явное действие
        // пользователя, а не самоуправство платы. Приглашение с карты
        // известно только здесь - машину держим с этого места.
        rp2350::hal::host_signals_allow_reset(true);
        bus::assert_host_reset();
    } else if (!s_boot_held && soundsinth::config::divmmc_selected(player::config::settings())) {
        // DivMMC выбран файлом с карты, а по блоку флеша машину не держали:
        // без сброса она уже стоит в своём ПЗУ, и эмуляция встала бы под
        // работающую машину. RESET_SIGNAL при DivMMC всегда 1.
        debug_log("boot: settings from the card need a machine reset (DISKSYS=DIVMMC), holding it\n");
        bus::assert_host_reset();
    }

    // Карты HID - в PSRAM и только когда хост будет поднят (условие host_init).
    if (want_config || soundsinth::config::usb_host_used(player::config::settings())) {
        rp2350::usb::maps_attach(soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, rp2350::usb::maps_bytes()));
    }

    // Режим шины - до подъёма Core1: он решает, кто захватывает автоматы PIO.
    const uint8_t disksys = player::config::settings().disksys;
    const bool trdos      = disksys == static_cast<uint8_t>(soundsinth::config::DiskSys::TrDos);
    bus::rom_emu_set_enabled(want_config || disksys == static_cast<uint8_t>(soundsinth::config::DiskSys::DivMmc));
    // Своё ПЗУ - из области флеша, пустая область оставляет ПЗУ машины.
    // DOS у него свой, триггер TR-DOS не нужен.
    bool pentagon = false;
    if (const uint32_t slot = soundsinth::config::custom_rom_slot(player::config::settings()); slot != 0u && !bus::rom_emu_enabled()) {
        static_assert(bus::kPentagonRomBytes == platform::kRomSlotBytes, "a custom ROM is one flash slot");
        const uint8_t* image = platform::rom_slot_view(slot - 1u);
        pentagon             = bus::pentagon_rom_load(image);
        printf("boot: ROM slot %u: %s\n", static_cast<unsigned>(slot), pentagon ? "loaded" : "empty, the machine ROM stays");
    }
    // Beta Disk платы: в конфигураторе не поднимается; со своим ПЗУ пока
    // тоже - удержание /WAIT есть только у склейщика режима TR-DOS. PSRAM
    // дисководов и навигатора - до треков.
    const bool bdi_wanted = !want_config && trdos;
    if (bdi_wanted && pentagon) printf("boot: beta disk needs ROM=BOARD for now, off\n");
    const auto psram = [](uint32_t bytes) { return soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, bytes); };
    s_bdi            = bdi_wanted && !pentagon && devices::bdi::bdi_reserve(psram) && player::nav::nav_reserve(psram);
    if (bdi_wanted && !pentagon && !s_bdi) printf("boot: bdi - no PSRAM for the drives\n");
    bus::pentagon_rom_set_enabled(pentagon, trdos);
    // Beta Disk платы берёт DOS от того же триггера: он же включает ПЗУ TR-DOS
    // машины, когда своего ПЗУ нет.
    bus::trdos_set_enabled(!want_config && trdos && !pentagon);
    bus::nav_set_enabled(s_bdi);
    // Сброс машины по тишине на M1 - всегда, какой бы ни была настройка
    // сброса: и кнопку машины, и свой сброс плата должна видеть одинаково.
    // С DivMMC, своим ПЗУ и TR-DOS места нет: pio0 занят; своё ПЗУ и TR-DOS
    // считают M1 на PWM.
    bus::reset_watch_set_enabled(!bus::rom_emu_enabled() && !pentagon && !bus::trdos_enabled());

    printf("boot: main stack free %" PRIu32 " of %u B\n", rp2350::stack_unpainted_bytes(&__StackBottom, &__StackTop),
           static_cast<unsigned>((&__StackTop - &__StackBottom) * sizeof(uint32_t)));

    // На кольца лога - до запуска Core1: иначе до старта планировщика оба ядра
    // печатали бы напрямую и строки перемешивались. Строки копятся в кольце до
    // появления потребителя.
    // Кольца - в PSRAM: 16 КБ SRAM рабочей прошивке нужнее.
    platform::log_rings_attach(soundsinth::memory::psram_take_permanent(shared::g_track_memory.psram, 2u * debug_ring::kRingBytes), debug_ring::kRingBytes);
    platform::log_use_rings();

    // Core1 - шина целиком: irq_set_exclusive_handler()/irq_set_enabled()
    // регистрируют прерывания PIO на вызывающем ядре, поэтому шина
    // поднимается отсюда, и не раньше psram_init(): в прямом режиме QMI флеш
    // XIP недоступен, а с ним код обработчиков во флеше. Не возвращается.
    rp2350::hal::boot_save_init();
    multicore_launch_core1([]() {
        rp2350::stack_paint_below_sp(&__StackOneBottom);
        // Эмуляция ПЗУ поднимается первой: она захватывает автоматы PIO по явным
        // номерам, шина плагина берёт свои следом.
        if (bus::rom_emu_enabled()) {
            bus::rom_emu_init();
            debug_log("core1: DivMMC on, ROM emulated\n");
        }
        // Страница настроек заполняется до того, как машина выйдет из
        // сброса: ПЗУ стартует раньше, чем плата успела бы ответить на
        // команду, и отдельной команды "прочитать" поэтому нет.
        if (bus::config_rom_enabled() && player::config::config_term_begin(bus::config_rom_page(), s_rom_files)) {
            debug_log("config: terminal page, the menu runs on the board\n");
        } else if (bus::config_rom_enabled()) {
            // Без памяти меню ПЗУ не дождётся страницы и скажет, что плата молчит.
            debug_log("config: no PSRAM for the menu\n");
        }

        hostlink::init(s_protocol, player::session_orchestrator_callbacks(s_orch));
        // Автомат карты свой у каждой стороны, общий только носитель.
        //
        // Выключенные настройками карты DivMMC и Z-Controller портов не
        // занимают: на машине со своим интерфейсом карты два устройства на
        // одном чтении дали бы столкновение на шине. При DISKSYS=DIVMMC
        // одна из карт включена всегда (settings_clamp).
        const soundsinth::config::Settings& cfg = player::config::settings();
        const bool card1                        = soundsinth::config::device_on(cfg.divmmc_card1);
        const bool card2                        = soundsinth::config::device_on(cfg.divmmc_card2);
        if (card1 || card2) {
            devices::divmmc_card::divmmc_card_init(card1, card2);
            debug_logf("core1: divmmc cards %u %u on 0xE7/0xEB\n", static_cast<unsigned>(card1), static_cast<unsigned>(card2));
        }
        if (soundsinth::config::device_on(cfg.zcontroller)) {
            devices::zcontroller::zcontroller_init();
            debug_log("core1: z-controller on 0x57/0x77\n");
        } else {
            debug_log("core1: z-controller disabled by settings\n");
        }
        // Beta Disk: код автомата ВГ93 и обработчиков - из флеша в банки 8-15
        // ОЗУ DivMMC (без DivMMC свободны), за ним BdiHost.
        if (s_bdi) {
            const auto code = static_cast<size_t>(__bdi_code_end__ - __bdi_code_start__);
            std::memcpy(__bdi_code_start__, __bdi_code_source__, code);
            // Код записан данными: выборка команд - только после записи.
            __dsb();
            __isb();
            uint8_t* const free_at = __bdi_code_end__;
            const bool ok          = devices::bdi::bdi_start(free_at, static_cast<size_t>(&bus_divmmc_ram[0][0] + sizeof(bus_divmmc_ram) - free_at));
            debug_logf("core1: beta disk %s, code %u B in banks\n", ok ? "on" : "FAILED", static_cast<unsigned>(code));
        }
        // Эмуляция General Sound на родных портах 0xBB/0xB3 (наш протокол на
        // 0x63/0x67). Регистрация портов после инициализации шины, как у
        // остальных; выключенная настройками, она портов не занимает, и
        // машина её не видит.
        // Живой AY/YM: включается до первой записи в порт, на шину не
        // отвечает - только слушает.
        player::live_chips_configure(true);
        if (player::config::settings().gs != 0) {
            player::gs::bridge_init();
        } else {
            debug_log("gs: disabled by settings\n");
        }
        // USB-хост - последним: прерывание регистрируется на вызывающем
        // ядре, и приоритет ему ставится ниже всех шинных. Виток крутит
        // цикл сеанса.
        //
        // В конфигураторе - всегда: управление с клавиатуры USB, даже если
        // она выключена в настройках. В обычном режиме - только когда USB
        // кому-то нужен: виток стека занимает цикл Core1. Без стека вход в конфигуратор - перемычкой или
        // файлом-приглашением на карте.
        if (platform::config_rom_active() || soundsinth::config::usb_host_used(player::config::settings())) {
            rp2350::usb::host_init();
        } else {
            debug_log("usb: host not started - no USB consumer in settings\n");
        }

        // Банк .mid: файл на карте (до 113 МБ), иначе регион флеша (15 МБ, по
        // XIP). Где он лежит, знает только раскладка прошивки.
        //
        // Здесь, а не на Core0 до запуска этого ядра: чтение таблиц с карты -
        // сотни миллисекунд, и ждать их незачем. USB к этому месту поднят, а
        // перечисление двигается витком стека между кусками чтения. Трека
        // нет, и распределитель PSRAM видит одно ядро. При RESET_SIGNAL=0
        // машина уже работает.
        debug_logf("core1: opening banks t=%" PRIu32 " ms\n", platform::mono_ms());
        player::banks_open(reinterpret_cast<const uint8_t*>(XIP_BASE + SOUNDSINTH_BANK_FLASH_OFFSET), PICO_FLASH_SIZE_BYTES - SOUNDSINTH_BANK_FLASH_OFFSET);
        debug_logf("core1: banks opened t=%" PRIu32 " ms\n", platform::mono_ms());
        // Будильник холостого витка - до сеанса: его захват гасит
        // прерывания, а дальше шина уже живая.
        player::hal::idle_wait_init();
        debug_log("core1: bus PIO/DMA/IRQ ready, entering session_orchestrator_run\n");
        player::session_orchestrator_run(s_orch);
    });

    // Core0 с этой точки - ядро FreeRTOS синтезатора.
    // log_task - единственный, кто трогает UART после старта планировщика.
    // Приоритет как у app_task, ниже рендера
    // (SOUNDSINTH_RENDER_TASK_PRIORITY = IDLE+2): лог не отбирает время у
    // звука.
    // Место обеих задач статическое: кучи у ядра нет, нехватка SRAM должна
    // быть ошибкой компоновки, а не отказом выделения на старте.
    //
    // Цикл звука - 1024 слова, 4 КБ: трогает до 1696 Б. Журнал - 768 слов,
    // 3 КБ: худший путь печати около 1210 Б (session_log_stats), путь отказа
    // (configASSERT под debug_log) около 1390 - запас больше двух раз.
    // Проверять строкой "stack free:".
    static StackType_t s_log_stack[768];
    static StaticTask_t s_log_tcb;
    static StackType_t s_app_stack[1024];
    static StaticTask_t s_app_tcb;
    xTaskCreateStatic(rp2350::log_task, "log_task", std::size(s_log_stack), nullptr, tskIDLE_PRIORITY + 1, s_log_stack, &s_log_tcb);
    TaskHandle_t app = xTaskCreateStatic(player::audio_chain_task, "app_task", std::size(s_app_stack), nullptr, tskIDLE_PRIORITY + 1, s_app_stack, &s_app_tcb);
    rp2350::log_task_set_app_handle(app);
    // Wi-Fi: приём UART1 и задача связи, тоже на Core0.
    // В конфигураторе задача модуля ведёт его инструменты и к сети не
    // подключается.
    if (want_config) {
        rp2350::wifi::esp_link_start_tools();
    } else if (player::config::settings().wifi != 0) {
        rp2350::wifi::esp_link_start(player::config::settings());
    }
    debug_log("boot: app_task created, entering FreeRTOS scheduler (Core0)\n");
    vTaskStartScheduler(); // никогда не возвращается

    for (;;) {
    } // недостижимо
}
