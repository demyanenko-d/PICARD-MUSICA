// SPDX-License-Identifier: MIT
// Порты шины Z80 для устройств: переходник к
// таблицам ответов и обработчикам этой платы.
//
// Типы указателей у контракта и у шины совпадают по сигнатуре, но это
// разные типы: приведение здесь, а не в устройстве, - устройство про шину
// не знает.

#include "devices/hal/z80_ports.h"

#include "platform/hot_path.h"

#include "bus.h"

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
    bus::port_on_write(port, reinterpret_cast<bus::PortWriteFn>(fn));
}

void devices::hal::z80_port_on_read_done(uint8_t port, PortReadDoneFn fn) {
    bus::port_on_read_done(port, reinterpret_cast<bus::PortReadFn>(fn));
}
