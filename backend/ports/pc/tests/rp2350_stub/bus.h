// SPDX-License-Identifier: MIT
#pragma once

// Шина Z80 в тестах ПК: таблица портов вместо PIO. Тест сам зовёт
// обработчики записи и чтения, как это делают прерывания платы.
#include <cstdint>

namespace bus {

using PortWriteFn = void (*)(uint8_t port, uint8_t data);
using PortReadFn  = void (*)(uint8_t port);

bool rom_emu_enabled();
void port_on_write(uint8_t port, PortWriteFn fn);
void port_set_read(uint8_t port, uint8_t value);
void port_on_read_done(uint8_t port, PortReadFn fn);

} // namespace bus
