// SPDX-License-Identifier: MIT
// Платформа для кода платы, собираемого в тестах: часы и журнал.
//
// Часы двигает сам тест - правила по времени (отъём шины у ушедшего
// владельца, окно переинициализации карты) иначе не проверить. Журнал идёт
// в stdout и в хвост g_test_log, по которому тест сверяет напечатанные
// счётчики.

#include "platform/log.h"
#include "platform/mono_time.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern uint32_t g_test_time_us;
extern char g_test_log[8192];

// Место, где тест вклинивается внутрь чужого вызова. Часы спрашивают и
// эмулятор карты посреди такта, и это единственный способ изобразить
// вытеснение обработчика обработчиком, не заводя крюк в самом эмуляторе.
// Крюк одноразовый: снимается до вызова, иначе он позвал бы сам себя.
void (*g_mono_hook)() = nullptr;

uint32_t platform::mono_us() {
    if (g_mono_hook != nullptr) {
        void (*hook)() = g_mono_hook;
        g_mono_hook    = nullptr;
        hook();
    }
    return g_test_time_us;
}

// Ожидание просто двигает часы теста.
void platform::busy_wait(uint32_t us) {
    g_test_time_us += us;
}

void platform::debug_log(const char* msg) {
    std::fputs(msg, stdout);
    const size_t have = std::strlen(g_test_log);
    const size_t add  = std::strlen(msg);
    if (have + add < sizeof(g_test_log)) std::memcpy(g_test_log + have, msg, add + 1);
}

void platform::debug_logf(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || static_cast<unsigned>(n) >= sizeof(buf)) return;
    platform::debug_log(buf);
}
