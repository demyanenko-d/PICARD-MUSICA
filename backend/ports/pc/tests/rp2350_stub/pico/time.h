// SPDX-License-Identifier: MIT
#pragma once

// Часы платы в тестах ПК: тест двигает их сам (правила по времени, например
// отъём шины у ушедшего владельца или окно переинициализации карты, иначе не
// проверить). Ожидание busy_wait_ms просто сдвигает часы.
#include <cstdint>

extern uint32_t g_test_time_us;

using absolute_time_t = uint64_t;

inline uint32_t time_us_32() {
    return g_test_time_us;
}
inline absolute_time_t get_absolute_time() {
    return g_test_time_us;
}
inline absolute_time_t make_timeout_time_ms(uint32_t ms) {
    return get_absolute_time() + ms * 1000ull;
}
inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) {
    return static_cast<int64_t>(to) - static_cast<int64_t>(from);
}
inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return static_cast<uint32_t>(t / 1000u);
}
inline void busy_wait_ms(uint32_t ms) {
    g_test_time_us += ms * 1000u;
}
