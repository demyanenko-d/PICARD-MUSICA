// SPDX-License-Identifier: MIT
#include "devices/hid/joystick.h"

#include "devices/hal/z80_ports.h"
#include "platform/log.h"

namespace devices::hid {
namespace {

uint8_t s_pad    = 0; // геймпад
uint8_t s_keys   = 0; // клавиатура в режиме Kempston
uint8_t s_bits   = 0; // что отдаётся на порт
bool s_pad_held  = false;
bool s_keys_held = false;
bool s_allowed   = false; // настройка USB_GAMEPAD
bool s_wide      = false; // настройка JOY_8BIT

bool attached() {
    return s_pad_held || s_keys_held;
}

// Свести источники и отдать на порт. Противоположные направления разом не
// отдаём: у настоящего джойстика так не бывает, а игры на этом иногда
// спотыкаются. Чистится сумма, а не слагаемые - иначе стрелка с клавиатуры
// и ось геймпада гасили бы друг друга порознь, и до порта не доехало бы
// ничего.
void publish() {
    uint8_t bits = static_cast<uint8_t>(s_pad | s_keys);
    if (!s_wide) bits &= static_cast<uint8_t>(kJoyRight | kJoyLeft | kJoyDown | kJoyUp | kJoyFire);
    if ((bits & (kJoyLeft | kJoyRight)) == (kJoyLeft | kJoyRight)) bits &= static_cast<uint8_t>(~(kJoyLeft | kJoyRight));
    if ((bits & (kJoyUp | kJoyDown)) == (kJoyUp | kJoyDown)) bits &= static_cast<uint8_t>(~(kJoyUp | kJoyDown));
    if (bits == s_bits) return;
    s_bits = bits;
    if (attached()) hal::z80_port_set_read(kJoystickPort, bits);
    // На каждое изменение: строк столько же, сколько нажатий и отпусканий.
    // Держит ли плата порт - главное, чем отказ "игра не слушается" отличается
    // от "игра не опрашивает Kempston".
    debug_logf("hid: kempston bits %02X (keys %02X, pad %02X), port held: keys %u pad %u\n", bits, s_keys, s_pad, s_keys_held ? 1u : 0u, s_pad_held ? 1u : 0u);
}

} // namespace

void joystick_reset() {
    s_pad  = 0;
    s_keys = 0;
    s_bits = 0;
    if (attached()) hal::z80_port_set_read(kJoystickPort, 0);
}

void joystick_set_pad(uint8_t bits) {
    s_pad = bits;
    publish();
}

void joystick_set_keys(uint8_t bits) {
    s_keys = bits;
    publish();
}

uint8_t joystick_state() {
    return s_bits;
}

void joystick_attach() {
    s_pad_held = true;
    hal::z80_port_set_read(kJoystickPort, s_bits);
}

void joystick_detach() {
    s_pad_held = false;
    s_pad      = 0;
    publish();
    if (!attached()) hal::z80_port_clear_read(kJoystickPort);
}

void joystick_keys_attach() {
    s_keys_held = true;
    hal::z80_port_set_read(kJoystickPort, s_bits);
}

void joystick_keys_detach() {
    s_keys_held = false;
    s_keys      = 0;
    publish();
    if (!attached()) hal::z80_port_clear_read(kJoystickPort);
}

bool joystick_keys_held() {
    return s_keys_held;
}

bool joystick_pad_held() {
    return s_pad_held;
}

void joystick_set_allowed(bool on) {
    s_allowed = on;
}

bool joystick_allowed() {
    return s_allowed;
}

void joystick_set_wide(bool on) {
    s_wide = on;
    publish();
}

} // namespace devices::hid
