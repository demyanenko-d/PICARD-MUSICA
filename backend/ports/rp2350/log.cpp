// SPDX-License-Identifier: MIT
// Журнал платы: консоль (UART0 на GPIO32/33) и задача логгера на Core0.
//
// Механизм колец - в platform/log_rings.h, здесь только то, чем он пишет,
// и расписание строк, которые печатают модули.
//
// Своей диагностики здесь нет - только когда и что позвать. Сами строки
// собирают те, кто владеет числами: шина, карта, эмуляция GS, протокол.

#include "log.h"

#include <cinttypes>
#include <cstdio>
#include <iterator> // std::size

#include "hardware/uart.h"
#include "pico.h"
#include "pico/time.h"

#include "FreeRTOS.h"
#include "task.h"

#include "platform/compiler.h"
#include "platform/log.h"
#include "platform/log_rings.h"
#include "platform/os.h"

#include "core/config.h"

#include "devices/sd/card_protocol.h"
#include "devices/sd/spi_emu.h"
#include "devices/storage/storage.h"

#include "player/gs/bridge.h"
#include "player/live/ay_tap.h"
#include "player/live/session.h"
#include "player/session.h"
#include "player/shared_state.h"

#include "firmware_config.h"
#include "hostlink/bus.h"
#include "hostlink/divmmc.h"
#include "hostlink/hostlink.h"
#include "rtos/ticks.h"
#include "stack_paint.h"
#include "usb/usb_host.h"

// --- Консоль ---

bool platform::log_put(uint8_t b) {
    if (!uart_is_writable(uart0)) return false;
    uart_get_hw(uart0)->dr = b;
    return true;
}

void platform::log_put_blocking(const char* msg) {
    uart_puts(uart0, msg);
}

void platform::log_wait_ready() {
    while (!uart_is_writable(uart0)) {
    }
}

bool platform::in_isr() {
    return __get_current_exception() != 0;
}

uint32_t platform::core_id() {
    return get_core_num();
}

// Только на Core0: счётчик приостановки у FreeRTOS общий с другим ядром, а
// на Core1 планировщика нет - там пишет один поток.
void platform::log_writer_lock() {
    if (get_core_num() == 0) vTaskSuspendAll();
}

void platform::log_writer_unlock() {
    if (get_core_num() == 0) xTaskResumeAll();
}

// --- Расписание ---

extern "C" uint32_t __StackOneBottom, __StackOneTop, __StackBottom, __StackTop;

namespace {

using rp2350::stack_unpainted_bytes;
using rp2350::ticks_for_us;

// Хэндл app_task - для запаса её стека в строке stack free:.
TaskHandle_t s_app_task = nullptr;

// Стоит ли Core1: обороты его цикла и обслуживания без ожидания не меняются
// дольше секунды - строка с фазой, где он ждёт, и строка при
// возобновлении. Обслуживание без ожидания спит до обращения хоста, поэтому "стоит, загрузка" - это и хост, ушедший посреди
// окна.
void log_core1_liveness() {
    static uint32_t s_seen    = 0;
    static uint32_t s_seen_us = 0;
    static bool s_stalled     = false;
    const uint32_t loops      = shared::g_core1_loops.load(std::memory_order_relaxed);
    const uint32_t now        = time_us_32();
    if (loops != s_seen) {
        if (s_stalled) {
            debug_logf("core1: alive again after %" PRIu32 " ms\n", (now - s_seen_us) / 1000u);
        }
        s_seen    = loops;
        s_seen_us = now;
        s_stalled = false;
        return;
    }
    constexpr uint32_t kCore1StallUs = 1000000;
    if (s_stalled || loops == 0 || now - s_seen_us < kCore1StallUs) return;
    const shared::Core1Phase phase = shared::g_core1_phase.load(std::memory_order_relaxed);
    const shared::Core1Step step   = shared::g_core1_step.load(std::memory_order_relaxed);
    debug_logf("core1: stalled %" PRIu32 " ms, phase %s, step %s\n", (now - s_seen_us) / 1000u, shared::core1_phase_name(phase), shared::core1_step_name(step));
    s_stalled = true;
}

// Запас стеков: байт, ни разу не тронутых, у каждой задачи и у обоих
// стеков прерываний. Кучи у ядра нет - всё размещено статически, и мерить
// нечего, кроме стеков; они же и есть то, что при нехватке SRAM режут
// первым. Строка - при изменении и не реже раза в kStackPulseCalls
// вызовов, как пульс Core0.
void log_stack_marks() {
    constexpr uint32_t kStackPulseCalls = 4;
    struct Marks {
        uint32_t app, log, render, seq, idle, msp, core1;
        bool operator==(const Marks& o) const {
            return app == o.app && log == o.log && render == o.render && seq == o.seq && idle == o.idle && msp == o.msp && core1 == o.core1;
        }
    };
    static Marks s_last     = {};
    static uint32_t s_quiet = 0;
    const Marks m           = {static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_app_task)) * sizeof(StackType_t),
                               static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) * sizeof(StackType_t),
                               platform::os_task_stack_unused_bytes(platform::TaskRole::Render),
                               platform::os_task_stack_unused_bytes(platform::TaskRole::Sequencer),
                               static_cast<uint32_t>(uxTaskGetStackHighWaterMark(xTaskGetIdleTaskHandle())) * sizeof(StackType_t),
                               stack_unpainted_bytes(&__StackBottom, &__StackTop),
                               stack_unpainted_bytes(&__StackOneBottom, &__StackOneTop)};
    if (m == s_last && ++s_quiet < kStackPulseCalls) return;
    s_quiet = 0;
    s_last  = m;
    char h[192];
    snprintf(h, sizeof(h),
             "stack free: app %" PRIu32 " log %" PRIu32 " render %" PRIu32 " seq %" PRIu32 " idle %" PRIu32 " msp %" PRIu32 " core1 %" PRIu32 "\n", m.app,
             m.log, m.render, m.seq, m.idle, m.msp, m.core1);
    debug_log(h);
}

// Обороты цикла Core1 в секунду. Холостой виток просыпается на каждом
// прерывании шины; число против числа событий показывает, сколько
// проходов впустую. Счётчик уже есть, Core1 ничего не добавляет.
void log_core1_loop_rate(uint32_t now_ms) {
    static uint32_t s_loops = 0;
    static uint32_t s_at_ms = 0;
    const uint32_t loops    = shared::g_core1_loops.load(std::memory_order_relaxed);
    const uint32_t span     = now_ms - s_at_ms;
    if (s_at_ms != 0 && span != 0) {
        debug_logf("core1: loops per second %" PRIu32 "\n", static_cast<uint32_t>((static_cast<uint64_t>(loops - s_loops) * 1000u) / span));
    }
    s_loops = loops;
    s_at_ms = now_ms;
}

// Периодические строки log_task - по времени: проход длится от тика до 10
// мс по потоку лога. gs: и sd: с divmmc: разнесены со stack free:, чтобы не идти
// пачкой.
constexpr uint32_t kStackPeriodMs = 1000u * SOUNDSINTH_LOG_RARITY;
constexpr uint32_t kSlowPeriodMs  = 4u * kStackPeriodMs;
constexpr uint32_t kGsFirstMs     = 1000;
constexpr uint32_t kSdFirstMs     = 3000;

// Настал ли срок at (с заворотом счёта мс).
bool due(uint32_t now, uint32_t at) {
    return static_cast<int32_t>(now - at) >= 0;
}

// Потребитель колец лога: в UART пишет только она. За проход - сколько
// влезет в TX FIFO, без ожидания (на 115200 уходит 11-12 байт в мс).
void log_task(void* /*arg*/) {
    uint32_t stack_at = kStackPeriodMs;
    uint32_t gs_at    = kGsFirstMs;
    uint32_t sd_at    = kSdFirstMs;
    // Живой режим - своим сроком и отдельно от плотного блока карты: там
    // строки не влезали в кольцо лога.
    uint32_t live_at = 2000;
    for (;;) {
        // Выгрести и доложить о потерях самого журнала: это его дело, не платы.
        const bool pending = platform::log_service(64);
        // Не печать: отметки опоздания автомата выдачи байта копятся в FIFO на
        // четыре, забирать их надо часто, а log_task просыпается чаще всех.
        bus::bus_queues_poll();
        log_core1_liveness();
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        // Эмуляция GS: печать, только когда с ней говорили.
        if (due(now, gs_at)) {
            gs_at = now + kSlowPeriodMs;
            player::gs::bridge_log_stats();
        }
        if (due(now, sd_at)) {
            sd_at = now + kSlowPeriodMs;
            // Карта и её владелец, подстановка DivMMC, здоровье носителя.
            devices::sd::sd_spi_log_stats();
            bus::divmmc_log_stats();
            devices::storage::storage_log_health();
            rp2350::usb::host_log_health();
            // Банка во флеше нет: .mid не играет ни файлом, ни живым потоком.
            // Строка повторяется в каждом разборе - строку загрузки застаёт
            // не всякий, а причина отказа у неё одна на все проявления.
            if (!shared::g_flash_bank.valid()) {
                debug_log("WARNING: no bank in flash - MIDI does not play, flash bank.uf2\n");
            }
        }
        devices::sd::sd_spi_log_trace_if_new();

        if (player::live::session_active() && due(now, live_at)) {
            live_at = now + 5000;
            player::live::session_log_stats();
        } else if (player::live::ay_tap_seen() != 0 && due(now, live_at)) {
            // Записи в порты AY идут, а живой режим не включился.
            live_at = now + 5000;
            player::live::ay_tap_log_stats();
        }
        if (due(now, stack_at)) {
            stack_at = now + kStackPeriodMs;
            log_stack_marks();
            log_core1_loop_rate(now);
        }

        // Осталось в кольцах - через 2 тика (FIFO на 32 байта пустеет за 2.8
        // мс), пусто - через 10 мс.
        vTaskDelay(pending ? 2u : ticks_for_us(10000));
    }
}

} // namespace

namespace rp2350 {

void log_task(void* arg) {
    ::log_task(arg);
}

void log_task_set_app_handle(TaskHandle_t h) {
    s_app_task = h;
}

} // namespace rp2350
