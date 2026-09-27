#include "devices/hid/joystick.h"

#include "devices/hal/z80_ports.h"

namespace devices::hid {
namespace {

uint8_t s_bits = 0;
bool s_attached = false;

} // namespace

void joystick_reset() {
    s_bits = 0;
    if (s_attached) hal::z80_port_set_read(kJoystickPort, 0);
}

void joystick_set(uint8_t bits) {
    // Противоположные направления разом не отдаём: у настоящего джойстика
    // так не бывает, а игры на этом иногда спотыкаются.
    if ((bits & (kJoyLeft | kJoyRight)) == (kJoyLeft | kJoyRight)) bits &= static_cast<uint8_t>(~(kJoyLeft | kJoyRight));
    if ((bits & (kJoyUp | kJoyDown)) == (kJoyUp | kJoyDown)) bits &= static_cast<uint8_t>(~(kJoyUp | kJoyDown));
    if (bits == s_bits) return;
    s_bits = bits;
    if (s_attached) hal::z80_port_set_read(kJoystickPort, bits);
}

uint8_t joystick_state() {
    return s_bits;
}

void joystick_attach() {
    s_attached = true;
    hal::z80_port_set_read(kJoystickPort, s_bits);
}

void joystick_detach() {
    s_attached = false;
    hal::z80_port_clear_read(kJoystickPort);
}

} // namespace devices::hid
