#pragma once

// GPIO платы в тестах ПК: значимо только gpio_put (CS карты), остальное -
// пустые настройки выводов.
#include <cstdint>

using uint = unsigned int;

enum gpio_function_rp2350 { GPIO_FUNC_SPI = 1 };
constexpr bool GPIO_OUT = true;

void gpio_put(uint pin, bool value);
inline void gpio_set_function(uint, gpio_function_rp2350) {}
inline void gpio_pull_up(uint) {}
inline void gpio_init(uint) {}
inline void gpio_set_dir(uint, bool) {}
