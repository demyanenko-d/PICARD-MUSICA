// SPDX-License-Identifier: MIT
// Опоры libstdc++ для прошивки без кучи.
//
// std::vector в сохранении состояния ymfm (в прошивке не зовётся) ссылается
// на std::__throw_length_error. Определение из libstdc++ приводит в образ пул
// исключений, а он при старте просит память у malloc.

#include "pico.h"

namespace std {

// Определение в std - нарочно: это и есть подмена функции libstdc++.
// NOLINTNEXTLINE(cert-dcl58-cpp)
[[noreturn]] void __throw_length_error(const char* what) {
    panic("%s", what);
}

} // namespace std
