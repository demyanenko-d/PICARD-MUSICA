#pragma once

// PSRAM: AP Memory APS6404L-3SQR, 8 МБ QPI через QMI/XIP.

#include <cstddef>
#include <cstdint>

#include "pico.h" // PICO_PSRAM_CS_PIN, PICO_PSRAM_SIZE_BYTES из заголовка платы

namespace psram {

inline constexpr uint kCsGpio = PICO_PSRAM_CS_PIN; // GPIO0 -> GPIO_FUNC_XIP_CS1

// Окно M1 (CS1) - 16 МБ выше флеша, кэшируемый write-back алиас.
inline constexpr uintptr_t kXipBase = XIP_BASE + 0x01000000u;
inline constexpr size_t kSizeBytes = PICO_PSRAM_SIZE_BYTES;

} // namespace psram
