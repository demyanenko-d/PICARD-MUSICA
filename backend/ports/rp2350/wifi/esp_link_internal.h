// SPDX-License-Identifier: MIT
#pragma once

// Внутреннее для каталога wifi: UART1 модуля для инструментов
// конфигуратора. Звать только из задачи модуля на Core0.

#include <cstdint>

namespace rp2350::wifi {

bool link_uart_poll(uint8_t& b);
void link_uart_put(const uint8_t* p, uint32_t n);
void link_uart_drop();
// Скорость UART модуля; принятое до смены выбрасывается.
void link_uart_baud(uint32_t baud);
// Скорость, на которой модуль ответил на AT; 0 - молчит, UART на 115200.
uint32_t link_at_probe();
// Кольцо тела ответа: в конфигураторе сети нет, и оно свободно.
uint8_t* link_ring(uint32_t& bytes);
// Задача модуля в режиме конфигуратора.
void esp_tools_task(void*);

} // namespace rp2350::wifi
