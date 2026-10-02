// SPDX-License-Identifier: MIT
// Отвод записей AY.

#include "player/live/ay_tap.h"

#include <atomic>

#include "platform/hot_path.h"

namespace player::live {

namespace {

// Один писатель (обработчик записи на Core1), один читатель (Core0).
// Ёмкость - kAyTapRing из заголовка: её показывает лог живого режима.
constexpr uint32_t kRing = kAyTapRing;
uint16_t s_ring[kRing];
std::atomic<uint32_t> s_head{0}; // пишет Core1
std::atomic<uint32_t> s_tail{0}; // пишет Core0
uint32_t s_lost = 0;             // пишет Core1

} // namespace

void SOUNDSINTH_HOT_PATH(ay_tap_push)(uint16_t ay_write) {
    const uint32_t h = s_head.load(std::memory_order_relaxed);
    if (h - s_tail.load(std::memory_order_acquire) >= kRing) {
        ++s_lost;
        return;
    }
    s_ring[h & (kRing - 1u)] = ay_write;
    s_head.store(h + 1u, std::memory_order_release);
}

bool ay_tap_pop(uint16_t& ay_write) {
    const uint32_t t = s_tail.load(std::memory_order_relaxed);
    if (t == s_head.load(std::memory_order_acquire)) return false;
    ay_write = s_ring[t & (kRing - 1u)];
    s_tail.store(t + 1u, std::memory_order_release);
    return true;
}

uint32_t ay_tap_lost() {
    return s_lost;
}

} // namespace player::live
