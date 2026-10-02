// SPDX-License-Identifier: MIT
#include "core/memory/arena.h"

namespace soundsinth::memory {

void arena_init(Arena& arena, uint8_t* base, size_t capacity) {
    arena.base     = base;
    arena.capacity = capacity;
    arena.offset   = 0;
    arena.floor    = capacity;
    arena.peak     = 0;
}

void* arena_alloc(Arena& arena, size_t size, size_t align) {
    const size_t aligned_offset = (arena.offset + (align - 1)) & ~(align - 1);
    // Граница - не потолок пула, а низ занятого сценария: они растут
    // навстречу.
    if (aligned_offset + size > arena.floor) {
        return nullptr; // исчерпана, вызывающий проверяет nullptr
    }
    arena.offset = aligned_offset + size;
    arena_note_peak(arena);
    return arena.base + aligned_offset;
}

void* arena_alloc_array(Arena& arena, uint32_t count, size_t size, size_t align) {
    const uint64_t bytes = static_cast<uint64_t>(count) * size;
    if (bytes > arena.capacity) return nullptr;
    return arena_alloc(arena, static_cast<size_t>(bytes), align);
}

} // namespace soundsinth::memory
