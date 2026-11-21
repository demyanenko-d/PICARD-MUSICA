// SPDX-License-Identifier: MIT
#pragma once

// Сведения о плате для экрана конфигуратора. Реализует порт.

#include <cstdint>

namespace platform {

struct BoardInfo {
    uint8_t chip_revision;
    uint32_t sys_hz;
    uint32_t flash_bytes;
    uint32_t psram_bytes;
};

BoardInfo board_info();

} // namespace platform
