// SPDX-License-Identifier: MIT
// Время шины и поток устройства: прерывание будильника таймера на Core1,
// приоритет ниже обработчиков шины; внеочередной запуск - то же прерывание,
// выставленное в ожидание через NVIC. Вызываемое с пути шины - в .bdi_code
// (банки BDI в SRAM), не во флеше.

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/structs/nvic.h"
#include "hardware/timer.h"

#include "platform/compiler.h"

#include "devices/hal/bus_worker.h"

#include "hw_config.h"

namespace {

// Время шины - полупериоды 2 МГц, по 0.5 мкс; таймер RP2350 считает
// микросекунды. Перевод обратно - вверх: "не позже" не должно стать
// "раньше".
constexpr uint32_t kBusTicksPerUs = 2;
constexpr uint64_t us_to_bus(uint64_t us) {
    return us * kBusTicksPerUs;
}
constexpr uint32_t bus_to_us_ceil(uint64_t t) {
    return static_cast<uint32_t>((t + kBusTicksPerUs - 1u) / kBusTicksPerUs);
}
// Дальний будильник: 2^30 мкс, 18 минут.
constexpr uint64_t kAlarmFarUs = 1ull << 30;

uint s_irq     = 0;
uint s_alarm   = 0;
void (*s_fn)() = nullptr;

SOUNDSINTH_BDI_CODE("bus_worker_isr") void bus_worker_isr() {
    timer_hw->intr = 1u << s_alarm;
    s_fn();
}

} // namespace

// Копия timer_time_us_64 SDK в полумикросекундах: та во флеше.
SOUNDSINTH_BDI_CODE("bus_clock_now") uint64_t devices::hal::bus_clock_now() {
    uint32_t hi = timer_hw->timerawh;
    uint32_t lo;
    for (;;) {
        lo                 = timer_hw->timerawl;
        const uint32_t hi2 = timer_hw->timerawh;
        if (hi2 == hi) break;
        hi = hi2;
    }
    return us_to_bus((static_cast<uint64_t>(hi) << 32) | lo);
}

SOUNDSINTH_BDI_CODE("bus_clock_stamp") uint32_t devices::hal::bus_clock_stamp() {
    return timer_hw->timerawl * kBusTicksPerUs;
}

void devices::hal::bus_worker_init(void (*fn)()) {
    s_fn    = fn;
    s_alarm = static_cast<uint>(hardware_alarm_claim_unused(true));
    s_irq   = timer_hardware_alarm_get_irq_num(timer_hw, s_alarm);
    irq_set_exclusive_handler(s_irq, bus_worker_isr);
    irq_set_priority(s_irq, bus::IRQ_PRIO_BDI);
    hw_set_bits(&timer_hw->inte, 1u << s_alarm);
    irq_set_enabled(s_irq, true);
}

// Копия irq_set_pending SDK: та во флеше.
SOUNDSINTH_BDI_CODE("bus_worker_kick") void devices::hal::bus_worker_kick() {
    nvic_hw->ispr[s_irq / 32u] = 1u << (s_irq % 32u);
}

// Будильник сравнивает 32 младших разряда микросекунд; прошедший момент
// уже не сработает - тогда пинок. Срок дальше полудиапазона показался бы
// прошедшим: дальний - не дальше kAlarmFarUs, поток проснётся впустую и
// поставит снова.
SOUNDSINTH_BDI_CODE("bus_worker_alarm") void devices::hal::bus_worker_alarm(uint64_t at) {
    const uint64_t far = bus_clock_now() + us_to_bus(kAlarmFarUs);
    if (at > far) at = far;
    const uint32_t us        = bus_to_us_ceil(at);
    timer_hw->alarm[s_alarm] = us;
    if (static_cast<int32_t>(us - timer_hw->timerawl) <= 0) bus_worker_kick();
}

SOUNDSINTH_BDI_CODE("bus_worker_lock") uint32_t devices::hal::bus_worker_lock() {
    return save_and_disable_interrupts();
}

SOUNDSINTH_BDI_CODE("bus_worker_unlock") void devices::hal::bus_worker_unlock(uint32_t saved) {
    restore_interrupts(saved);
}
