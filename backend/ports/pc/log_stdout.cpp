// SPDX-License-Identifier: MIT
// Журнал платы для сборки на ПК: вместо UART - стандартный вывод.
//
// Без него ядро и устройства, пишущие в журнал, не компонуются в приложение
// для ПК.
//
// Буфер той же длины, что на плате: строка, которая там не влезет,
// обязана не влезть и здесь.

#include "platform/log.h"

#include <cstdarg>
#include <cstdio>

namespace platform {

void debug_log(const char* msg) {
    std::fputs(msg, stdout);
}

void debug_logf(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) std::fputs(buf, stdout);
}

} // namespace platform
