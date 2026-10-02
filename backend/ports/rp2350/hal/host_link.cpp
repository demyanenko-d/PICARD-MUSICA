// SPDX-License-Identifier: MIT
// Связь сеанса с хостом и платой.
//
// Переходник: сам сеанс про PIO, DMA и мост GS не знает, а они про сеанс -
// только через колбэки протокола.

#include "player/hal/host_link.h"

#include "hardware/irq.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico.h"

#include "bus.h"
#include "divmmc.h"
#include "hostlink/hostlink.h"
#include "hw_config.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"

namespace {

// Будильник холостого витка. Прерывание разрешается в NVIC того ядра,
// которое крутит цикл сеанса, поэтому init зовётся оттуда же.
int s_sleep_alarm = -1;

void __not_in_flash_func(sleep_alarm_isr)() {
    timer_hw->intr = 1u << static_cast<uint>(s_sleep_alarm);
    // Событие защёлкивается, а не теряется: если будильник сработал
    // до того, как ядро дошло до ожидания, ждать будет уже нечего.
    __sev();
}

} // namespace

// Захват будильника идёт через hw_claim_lock, а тот - через спинлок с
// гашением прерываний. Поэтому не лениво из idle_wait: та лежит на горячем
// пути и зовётся, когда машина уже вышла из сброса, а окно с погашенными
// прерываниями на живой шине - это пропущенный ответ на IN.
void player::hal::idle_wait_init() {
    if (s_sleep_alarm >= 0) return;
    s_sleep_alarm    = hardware_alarm_claim_unused(true);
    const auto alarm = static_cast<uint>(s_sleep_alarm);
    const uint irq   = hardware_alarm_get_irq_num(alarm);
    irq_set_exclusive_handler(irq, sleep_alarm_isr);
    irq_set_priority(irq, bus::IRQ_PRIO_RELAXED);
    hw_set_bits(&timer_hw->inte, 1u << alarm);
    irq_set_enabled(irq, true);
}

SOUNDSINTH_HOT_PATH_ATTR("host_service")
void player::hal::host_service() {
    hostlink::service();
}

SOUNDSINTH_HOT_PATH_ATTR("host_service_and_wait")
void player::hal::host_service_and_wait() {
    hostlink::service_and_wait();
}

void player::hal::host_set_file_info(uint8_t minutes, uint8_t seconds, uint16_t samples, uint16_t patterns, uint16_t instruments) {
    hostlink::set_file_info(minutes, seconds, samples, patterns, instruments);
}

void player::hal::host_hold_for_reboot() {
    bus::assert_host_reset();
}

void player::hal::host_release_reset() {
    bus::release_host_reset();
}

SOUNDSINTH_HOT_PATH_ATTR("host_last_seen_us")
uint32_t player::hal::host_last_seen_us() {
    return bus::host_status_read_us();
}

uint32_t player::hal::bus_lost_port_writes() {
    return bus::z80_bus_pio_get_stats().wr_fifo_overruns;
}

uint32_t player::hal::bus_lost_rom_writes() {
    return bus::divmmc_lost_writes();
}

// Ждёт следующего прерывания или будильника.
//
// Ждёт по событию, а не по прерыванию: у wfi есть гонка, из-за
// которой ядро встаёт на секунды (замер платы: "core1: стоит"). Если
// будильник сработал между взводом и ожиданием, обработчик снимает
// признак, и wfi засыпает до чужого прерывания - а на тихой шине его может
// не быть долго. Регистр события защёлкивает срабатывание и гонки не имеет.
void __not_in_flash_func(player::hal::idle_wait)(uint32_t max_us) {
    // Старое событие снимается до взвода, иначе ожидание вернётся сразу.
    __sev();
    __wfe();
    timer_hw->alarm[s_sleep_alarm] = timer_hw->timerawl + max_us;
    __wfe();
}
