#pragma once

// Лог платы в тестах ПК: строка в stdout и в хвост g_test_log (тест сверяет
// по нему счётчики, которые код печатает, но не отдаёт).
#include <cstdio>
#include <cstring>

extern char g_test_log[8192];

inline void debug_log(const char* s) {
    std::fputs(s, stdout);
    const size_t have = std::strlen(g_test_log);
    const size_t add = std::strlen(s);
    if (have + add < sizeof(g_test_log)) std::memcpy(g_test_log + have, s, add + 1);
}
