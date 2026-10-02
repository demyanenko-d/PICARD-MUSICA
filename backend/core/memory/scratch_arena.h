// SPDX-License-Identifier: MIT
#pragma once

// Bump-аллокатор поверх готового буфера для временных буферов загрузчика
// постоянного размера: на стеке Core1 им тесно. Выделения каждого места
// проверяет static_assert формулой ScratchPlan.

#include <cstdint>
#include <new>

#include "core/memory/arena.h"

namespace soundsinth::memory {

// Наибольшее заполнение арены за прогон (строка packer у pc_player).
inline uint32_t g_max_scratch_used = 0;

struct ScratchArena {
    uint8_t* buffer;
    uint32_t size;
    uint32_t offset = 0;

    ScratchArena(uint8_t* buffer, uint32_t size) : buffer(buffer), size(size) {}

    // nullptr - переполнение scratch: вызывающий возвращает понятную ошибку.
    template <typename T>
    T* alloc(uint32_t count) {
        const uint32_t aligned_offset = align_up(offset, alignof(T));
        // uint64: count*sizeof может переполнить uint32 на патологическом файле.
        const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(T);
        if (aligned_offset + bytes > size) return nullptr;
        T* ptr = reinterpret_cast<T*>(buffer + aligned_offset);
        offset = static_cast<uint32_t>(aligned_offset + bytes);
        if (offset > g_max_scratch_used) g_max_scratch_used = offset;
        // Значение-инициализация, как у T arr[N]{}.
        for (uint32_t i = 0; i < count; ++i) {
            new (&ptr[i]) T();
        }
        return ptr;
    }
};

// Конец выделений ScratchArena::alloc подряд, той же формулой - для
// static_assert у выделений постоянного размера:
// ScratchPlan{}.add<A>(n).add<B>(1).end.
struct ScratchPlan {
    uint32_t end = 0;

    template <typename T>
    constexpr ScratchPlan add(uint32_t count) const {
        return ScratchPlan{align_up(end, alignof(T)) + count * static_cast<uint32_t>(sizeof(T))};
    }
};

} // namespace soundsinth::memory
