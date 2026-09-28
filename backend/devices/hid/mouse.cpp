#include "devices/hid/mouse.h"

#include "devices/hal/z80_ports.h"

namespace devices::hid {
namespace {

// Заполняется mouse_reset: незанятые сочетания старшего байта отвечают
// 0xFF, три занятых - счётчиками и кнопками.
alignas(256) uint8_t s_page[256];

uint8_t s_x = 0;
uint8_t s_y = 0;

// Кнопки и колесо делят байт, поэтому хранятся врозь и сводятся при
// записи: иначе одно затирало бы другое.
uint8_t s_buttons = kMouseButtonsIdle;
uint8_t s_wheel = kMouseWheelMask; // до первого щелчка байт как неподведённая шина

void publish_buttons() {
    s_page[kMouseHiButtons] = static_cast<uint8_t>((s_buttons & ~kMouseWheelMask) | (s_wheel & kMouseWheelMask));
}

} // namespace

void mouse_reset() {
    for (uint8_t& b : s_page) b = 0xFF;
    s_x = 0;
    s_y = 0;
    s_buttons = kMouseButtonsIdle;
    s_wheel = kMouseWheelMask;
    s_page[kMouseHiX] = s_x;
    s_page[kMouseHiY] = s_y;
    publish_buttons();
}

void mouse_move(int8_t dx, int8_t dy) {
    s_x = static_cast<uint8_t>(s_x + static_cast<uint8_t>(dx));
    s_y = static_cast<uint8_t>(s_y + static_cast<uint8_t>(dy));
    // Порядок не важен: счётчики независимы, а чтение берёт один байт.
    s_page[kMouseHiX] = s_x;
    s_page[kMouseHiY] = s_y;
}

void mouse_set_buttons(uint8_t mask) {
    s_buttons = mask;
    publish_buttons();
}

void mouse_wheel(int8_t clicks) {
    if (clicks == 0) return;
    s_wheel = static_cast<uint8_t>(s_wheel + static_cast<uint8_t>(clicks) * kMouseWheelStep);
    publish_buttons();
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
