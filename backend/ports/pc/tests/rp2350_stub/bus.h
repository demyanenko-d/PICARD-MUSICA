// SPDX-License-Identifier: MIT
#pragma once

// Шина Z80 в тестах ПК: таблица портов вместо PIO. Тест сам зовёт
// обработчики записи и чтения, как это делают прерывания платы.
#include <cstdint>

namespace bus {

using PortWriteFn = void (*)(uint8_t port, uint8_t data);
using PortReadFn  = void (*)(uint8_t port);

bool divmmc_enabled();
void z80_bus_pio_register_wr(uint8_t port, PortWriteFn fn);
void z80_bus_pio_set_rd(uint8_t port, uint8_t value);
void z80_bus_pio_register_rd_done(uint8_t port, PortReadFn fn);

} // namespace bus
