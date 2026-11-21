// SPDX-License-Identifier: MIT
#include "player/live/live_clock.h"

#include "platform/mono_time.h"

namespace player::live {

namespace {

// Не bus_clock_now: тот живёт в банках Beta Disk и вне TR-DOS не существует.
// Перенос 32 разрядов микросекунд считается здесь: рендер заходит чаще раза
// в час.
uint32_t s_us_last  = 0;
uint32_t s_us_wraps = 0;
uint64_t board_clock() {
    const uint32_t us = platform::mono_us();
    if (us < s_us_last) ++s_us_wraps;
    s_us_last = us;
    return ((static_cast<uint64_t>(s_us_wraps) << 32) | us) * 2u;
}
uint64_t (*s_clock_fn)() = &board_clock;

} // namespace

uint64_t live_clock_now() {
    return s_clock_fn();
}

void live_clock_set(uint64_t (*clock)()) {
    s_clock_fn = clock != nullptr ? clock : &board_clock;
}

int32_t g_live_out_skew_q32 = 0;

void live_clock_set_out_rate(uint64_t actual_mhz, uint32_t nominal_hz) {
    const uint64_t nominal_mhz = static_cast<uint64_t>(nominal_hz) * 1000u;
    if (nominal_mhz == 0) return;
    g_live_out_skew_q32 = static_cast<int32_t>(static_cast<int64_t>((actual_mhz << 32) / nominal_mhz) - (int64_t{1} << 32));
}

} // namespace player::live
