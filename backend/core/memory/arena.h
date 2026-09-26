#pragma once

// Линейный распределитель без освобождения отдельных объектов,
// сбрасывается целиком. Память под него даёт платформа
// (platform::resident_storage_acquire).

#include <cstddef>
#include <cstdint>
#include <new>

namespace soundsinth::memory {

// Смещение выравнивается от базы, поэтому база выровнена не хуже любого
// типа, который кладут в арену.
inline constexpr size_t kArenaBaseAlign = alignof(std::max_align_t);

// Вверх до кратного a; a - степень двойки.
inline constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

struct Arena {
    uint8_t* base = nullptr;
    size_t capacity = 0;
    size_t offset = 0;
    // Сверху того же пула вниз растут временные сценарии трека
    // (TrackScratch): буфер упаковщика паттернов, перепаковка PCM, линии
    // ревербератора. Арена не заходит за эту границу, сценарий - за
    // offset; встретились - отказ, а не молчаливая порча.
    //
    // capacity - пока сценария нет.
    size_t floor = 0;
    // Пик занятого пула за трек: метаданные плюс сценарий, который стоял в
    // тот момент. Отвечает на вопрос "сколько на самом деле нужно": сумма
    // максимумов по отдельности завышена, они не всегда совпадают по времени.
    size_t peak = 0;
};

// Отметить занятое: зовут после каждой выдачи, и арены, и сценария.
inline void arena_note_peak(Arena& arena) {
    const size_t busy = arena.offset + (arena.capacity - arena.floor);
    if (busy > arena.peak) arena.peak = busy;
}

void arena_init(Arena& arena, uint8_t* base, size_t capacity);
// nullptr, если арена исчерпана; align - степень двойки, не больше
// kArenaBaseAlign.
void* arena_alloc(Arena& arena, size_t size, size_t align);
// Сброс на новый трек: и метаданные, и граница сценария. К этому моменту
// движок прошлого трека снесён, значит и его сценарий мёртв - иначе
// граница осталась бы заниженной навсегда, и арена не выросла бы обратно.
inline void arena_reset(Arena& arena) {
    arena.offset = 0;
    arena.floor = arena.capacity;
    arena.peak = 0;
}
// Сколько осталось между ареной и занятым сценарием.
inline size_t arena_free(const Arena& arena) { return arena.floor > arena.offset ? arena.floor - arena.offset : 0; }
inline size_t arena_used(const Arena& arena) { return arena.offset; }

// count объектов по size байт: count * size считается в 64 битах. nullptr -
// арена исчерпана; count == 0 - указатель на текущий конец, как arena_alloc(0).
void* arena_alloc_array(Arena& arena, uint32_t count, size_t size, size_t align);

// count объектов T с value-init, как T arr[N]{}.
template <typename T>
T* arena_new(Arena& arena, uint32_t count = 1) {
    static_assert(alignof(T) <= kArenaBaseAlign, "выравнивание не больше базы арены");
    T* p = static_cast<T*>(arena_alloc_array(arena, count, sizeof(T), alignof(T)));
    if (p == nullptr) return nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        new (&p[i]) T();
    }
    return p;
}

} // namespace soundsinth::memory
