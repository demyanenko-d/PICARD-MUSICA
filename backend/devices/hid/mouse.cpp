#include "devices/hid/mouse.h"

#include "devices/hal/z80_ports.h"

namespace devices::hid {
namespace {

// Заполняется mouse_reset: незанятые сочетания старшего байта отвечают
// 0xFF, три занятых - счётчиками и кнопками.
alignas(256) uint8_t s_page[256];

uint8_t s_x = 0;
uint8_t s_y = 0;

} // namespace

void mouse_reset() {
    for (uint8_t& b : s_page) b = 0xFF;
    s_x = 0;
    s_y = 0;
    s_page[kMouseHiX] = s_x;
    s_page[kMouseHiY] = s_y;
    s_page[kMouseHiButtons] = kMouseButtonsIdle;
}

void mouse_move(int8_t dx, int8_t dy) {
    s_x = static_cast<uint8_t>(s_x + static_cast<uint8_t>(dx));
    s_y = static_cast<uint8_t>(s_y + static_cast<uint8_t>(dy));
    // Порядок не важен: счётчики независимы, а чтение берёт один байт.
    s_page[kMouseHiX] = s_x;
    s_page[kMouseHiY] = s_y;
}

void mouse_set_buttons(uint8_t mask) {
    s_page[kMouseHiButtons] = mask;
}

const uint8_t* mouse_page() {
    return s_page;
}

void mouse_attach() {
    mouse_reset();
    hal::z80_port_set_read_page(kMousePort, s_page);
}

void mouse_detach() {
    hal::z80_port_clear_read(kMousePort);
}

} // namespace devices::hid
