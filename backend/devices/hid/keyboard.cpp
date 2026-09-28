#include "devices/hid/keyboard.h"

#include "devices/hal/z80_ports.h"

namespace devices::hid {
namespace {

// Разряды 0..4 - клавиши, старшие три подняты, чтобы И по строкам их не
// гасило.
uint8_t s_rows[kKeyboardRows] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Нули значат "нажато всё", поэтому страницу заполняет keyboard_reset и
// каждая смена состояния.
alignas(256) uint8_t s_page[256];

// Клавиатура подключена; порт занимается только под нажатой клавишей.
bool s_present = false;
bool s_held = false;

void rebuild() {
    for (uint32_t hi = 0; hi < 256u; ++hi) {
        uint8_t v = 0xFF;
        for (uint32_t r = 0; r < kKeyboardRows; ++r) {
            if ((hi & (1u << r)) == 0u) v = static_cast<uint8_t>(v & s_rows[r]);
        }
        s_page[hi] = v;
    }
}

// Порт 0xFE занимается только пока нажата хоть одна клавиша: на машине
// со своей клавиатурой его ведёт ULA, и занятый постоянно порт отдавал бы
// нашу страницу вместо неё на каждом чтении.
void hold_port(bool hold) {
    if (hold == s_held) return;
    s_held = hold;
    if (hold) {
        hal::z80_port_set_read_page(kKeyboardPort, s_page);
    } else {
        hal::z80_port_clear_read(kKeyboardPort);
    }
}

bool any_pressed() {
    for (uint8_t r = 0; r < kKeyboardRows; ++r) {
        if ((s_rows[r] & 0x1Fu) != 0x1Fu) return true;
    }
    return false;
}

} // namespace

void keyboard_reset() {
    for (uint8_t& r : s_rows) r = 0xFF;
    rebuild();
    hold_port(false);
}

void keyboard_set_key(uint8_t row, uint8_t key, bool pressed) {
    if (row >= kKeyboardRows || key >= kKeyboardKeysPerRow) return;
    const uint8_t mask = static_cast<uint8_t>(1u << key);
    const uint8_t next = pressed ? static_cast<uint8_t>(s_rows[row] & ~mask) : static_cast<uint8_t>(s_rows[row] | mask);
    if (next == s_rows[row]) return;
    s_rows[row] = next;
    rebuild();
    hold_port(s_present && any_pressed());
}

void keyboard_set_rows(const uint8_t rows[kKeyboardRows]) {
    bool changed = false;
    for (uint32_t r = 0; r < kKeyboardRows; ++r) {
        const uint8_t next = static_cast<uint8_t>(rows[r] | kKeyboardIdleBits);
        if (next == s_rows[r]) continue;
        s_rows[r] = next;
        changed = true;
    }
    if (changed) rebuild();
    hold_port(s_present && any_pressed());
}

const uint8_t* keyboard_page() {
    return s_page;
}

void keyboard_attach() {
    s_present = true;
    keyboard_reset();
}

void keyboard_detach() {
    s_present = false;
    hold_port(false);
}

} // namespace devices::hid
