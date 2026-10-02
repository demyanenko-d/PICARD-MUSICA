// SPDX-License-Identifier: MIT
#pragma once

// Пины I2S.

#include "pico/types.h"

namespace rp2350::audio {

inline constexpr uint kI2sData = 44;
inline constexpr uint kI2sBck  = 45; // база side-set: BCK - бит 0, WS - бит 1 (соседний пин)
inline constexpr uint kI2sWs   = 46;
static_assert(kI2sWs == kI2sBck + 1, "side-set drives BCK and WS on adjacent pins");

} // namespace rp2350::audio
