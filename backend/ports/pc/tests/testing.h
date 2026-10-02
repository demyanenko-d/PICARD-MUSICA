// SPDX-License-Identifier: MIT
#pragma once

// Минимальный тестовый каркас без зависимостей. В soundsinth_core ничего из
// этого не попадает. GoogleTest/Catch2 ради нескольких десятков проверок
// не тянем.

#include <cstdio>

namespace testing {
inline int g_checks   = 0;
inline int g_failures = 0;
} // namespace testing

#define CHECK(cond)                                                                                                                                            \
    do {                                                                                                                                                       \
        ++testing::g_checks;                                                                                                                                   \
        if (!(cond)) {                                                                                                                                         \
            ++testing::g_failures;                                                                                                                             \
            std::fprintf(stderr, "  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                                                   \
        }                                                                                                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                                                         \
    do {                                                                                                                                                       \
        ++testing::g_checks;                                                                                                                                   \
        const auto _check_a = (a);                                                                                                                             \
        const auto _check_b = (b);                                                                                                                             \
        if (!(_check_a == _check_b)) {                                                                                                                         \
            ++testing::g_failures;                                                                                                                             \
            std::fprintf(stderr, "  CHECK_EQ failed: %s != %s (%s:%d)\n", #a, #b, __FILE__, __LINE__);                                                         \
        }                                                                                                                                                      \
    } while (0)
