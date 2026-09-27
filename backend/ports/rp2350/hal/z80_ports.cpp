// Порты шины Z80 для устройств (devices/hal/z80_ports.h): переходник к
// таблицам ответов и обработчикам этой платы.
//
// Типы указателей у контракта и у шины совпадают по сигнатуре, но это
// разные типы: приведение здесь, а не в устройстве, - устройство про шину
// не знает.

#include "devices/hal/z80_ports.h"

#include "bus.h"
#include "platform/hot_path.h"

SOUNDSINTH_HOT_PATH_ATTR("z80_port_set_read")
void devices::hal::z80_port_set_read(uint8_t port, uint8_t value) { bus::z80_bus_pio_set_rd(port, value); }

void devices::hal::z80_port_set_read_page(uint8_t port, const uint8_t* page) { bus::rom_emu_set_port_page(port, page); }

void devices::hal::z80_port_clear_read(uint8_t port) { bus::rom_emu_clear_port(port); }

void devices::hal::z80_port_on_write(uint8_t port, PortWriteFn fn) {
    bus::z80_bus_pio_register_wr(port, reinterpret_cast<bus::PortWriteFn>(fn));
}

void devices::hal::z80_port_on_read_done(uint8_t port, PortReadDoneFn fn) {
    bus::z80_bus_pio_register_rd_done(port, reinterpret_cast<bus::PortReadFn>(fn));
}
