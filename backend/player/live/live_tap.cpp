// SPDX-License-Identifier: MIT
// Отвод записей живых устройств.

#include "player/live/live_tap.h"

#include <atomic>

#include "platform/hot_path.h"

namespace player::live {

namespace {

TapRing<kTapRing> s_ring;
uint32_t s_seen = 0; // пишет Core1

} // namespace

void SOUNDSINTH_HOT_PATH(tap_push)(uint32_t stamp, TapPort port, uint8_t data) {
    ++s_seen;
    s_ring.push(stamp, static_cast<uint8_t>(port), data);
}

bool tap_peek(TapReader& r, uint32_t& e) {
    return s_ring.peek(r, e);
}

uint32_t tap_head() {
    return s_ring.head_now();
}

uint32_t tap_seen() {
    return s_seen;
}

} // namespace player::live
