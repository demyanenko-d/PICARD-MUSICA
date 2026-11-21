// SPDX-License-Identifier: MIT
#include "devices/hid/joystick.h"

#include "platform/log.h"

#include "devices/hal/z80_ports.h"

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
    // По port held видно, занят ли порт ответом: игра не слушается или не
    // опрашивает Kempston.
    debug_logf("hid: kempston bits %02X (keys %02X, pad %02X), port held: keys %u pad %u\n", bits, s_keys, s_pad, s_keys_held ? 1u : 0u, s_pad_held ? 1u : 0u);
}

// Источник занимает порт или отпускает его. Отпущенный источник гасит свои
// разряды; порт снимается, когда его не держит никто.
void hold(bool& held, uint8_t& source, bool on) {
    held = on;
    if (on) {
        hal::z80_port_set_read(kJoystickPort, s_bits);
        return;
    }
    source = 0;
    publish();
    if (!attached()) hal::z80_port_clear_read(kJoystickPort);
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
    hold(s_pad_held, s_pad, true);
}

void joystick_detach() {
    hold(s_pad_held, s_pad, false);
}

void joystick_keys_attach() {
    hold(s_keys_held, s_keys, true);
}

void joystick_keys_detach() {
    hold(s_keys_held, s_keys, false);
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
