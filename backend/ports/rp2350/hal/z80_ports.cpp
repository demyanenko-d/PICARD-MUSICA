// SPDX-License-Identifier: MIT
// Порты шины Z80 для устройств: переходник к таблицам ответов и
// обработчикам этой платы.
//
// Тип обработчика у устройства и у шины один (одна сигнатура): разойдутся -
// сборка остановится.

#include "platform/compiler.h"
#include "platform/hot_path.h"

#include "devices/hal/z80_ports.h"

#include "bus.h"
#include "hw_config.h"
#include "pentagon.pio.h" // IRQ_DOS

SOUNDSINTH_HOT_PATH_ATTR("z80_port_set_read")
void devices::hal::z80_port_set_read(uint8_t port, uint8_t value) {
    bus::port_set_read(port, value);
}

void devices::hal::z80_port_set_read_page(uint8_t port, const uint8_t* page) {
    bus::port_answer_page(port, page);
}

void devices::hal::z80_port_clear_read(uint8_t port) {
    bus::port_release(port);
}

void devices::hal::z80_port_on_write(uint8_t port, PortWriteFn fn) {
    bus::port_on_write(port, fn);
}

void devices::hal::z80_port_on_read_done(uint8_t port, PortReadDoneFn fn) {
    bus::port_on_read_done(port, fn);
}

bool devices::hal::z80_port_dos_cell(uint8_t port, const uint8_t* cell) {
    return bus::port_dos_cell(port, cell);
}

void devices::hal::z80_port_dos_on_read_done(PortReadDoneFn fn) {
    bus::port_dos_on_read_done(fn);
}

bool devices::hal::z80_port_dos_hold(uint8_t port, PortReadDoneFn fn) {
    return bus::port_dos_hold(port, fn);
}

bool devices::hal::z80_port_hold(uint8_t port, PortReadDoneFn fn) {
    return bus::port_hold(port, fn);
}

bool devices::hal::z80_wait_late_ready() {
    return bus::wait_late_ready();
}

void __not_in_flash_func(devices::hal::z80_wait_late)(bool on) {
    bus::wait_late(on);
}

// С пути шины Beta Disk: в его банках, не во флеше.
SOUNDSINTH_BDI_CODE("z80_port_hold_answer") void devices::hal::z80_port_hold_answer(uint8_t v) {
    bus::port_hold_answer(v);
}

// С пути шины Beta Disk: в его банках, не во флеше.
SOUNDSINTH_BDI_CODE("z80_dos_active") bool devices::hal::z80_dos_active() {
    return (bus::PIO_DETECT->irq & (1u << IRQ_DOS)) != 0u;
}

// С потока Beta Disk: в его банках, не во флеше.
SOUNDSINTH_BDI_CODE("z80_nav_open") bool devices::hal::z80_nav_open() {
    return bus::nav_open();
}

uint8_t* devices::hal::z80_nav_page() {
    return bus::nav_page();
}

uint8_t devices::hal::z80_port_7ffd() {
    return bus::port_7ffd();
}
