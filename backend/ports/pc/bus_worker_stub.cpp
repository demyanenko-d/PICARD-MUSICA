// SPDX-License-Identifier: MIT
// Шины Z80 на ПК нет, Beta Disk не поднимается: контракт devices/hal/
// bus_worker.h, окно навигатора и порты при DOS из z80_ports.h закрываются
// пустышкой. Цикл
// сеанса зовёт обслуживание Beta Disk на обеих сборках, и без этого
// хостовая не собиралась бы.

#include "devices/hal/bus_worker.h"
#include "devices/hal/z80_ports.h"

uint8_t* devices::hal::z80_nav_page() {
    return nullptr;
}

bool devices::hal::z80_nav_open() {
    return false;
}

uint8_t devices::hal::z80_port_7ffd() {
    return 0;
}

uint64_t devices::hal::bus_clock_now() {
    return 0;
}

uint32_t devices::hal::bus_clock_stamp() {
    return 0;
}

void devices::hal::bus_worker_init(void (*)()) {}

void devices::hal::bus_worker_kick() {}

void devices::hal::bus_worker_alarm(uint64_t) {}

uint32_t devices::hal::bus_worker_lock() {
    return 0;
}

void devices::hal::bus_worker_unlock(uint32_t) {}

bool devices::hal::z80_port_dos_cell(uint8_t, const uint8_t*) {
    return false;
}

void devices::hal::z80_port_dos_on_read_done(PortReadDoneFn fn) {
    (void)fn;
}

bool devices::hal::z80_dos_active() {
    return false;
}
