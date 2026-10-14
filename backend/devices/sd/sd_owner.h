// SPDX-License-Identifier: MIT
#pragma once

// Хозяин эмулятора карты. Без зависимостей: номер хозяина принимает и
// эмулятор карты, и арбитр носителя ниже него.

#include <cstdint>

namespace devices::sd {

// Эмуляторов два: DivMMC (порты 0xE7/0xEB) и Z-Controller (0x77/0x57).
// Автомат у каждого свой, общий только носитель; невыбранная сторона
// отдаёт 0xFF.
//
// Значение хозяина - индекс автомата и носителя, поэтому хозяева идут
// первыми: пустой нулевой элемент держал бы лишний автомат с двумя
// буферами сектора. None - признак "писателя нет", не индекс.
enum class SdOwner : uint8_t { DivMmc = 0, ZController = 1, None = 2 };

// Число хозяев, у которых есть автомат и носитель.
inline constexpr uint8_t kSdOwnerCount = 2;
static_assert(static_cast<uint8_t>(SdOwner::DivMmc) == 0, "SdOwner is an index: DivMMC first");
static_assert(static_cast<uint8_t>(SdOwner::ZController) == 1, "SdOwner is an index: Z-Controller second");
static_assert(static_cast<uint8_t>(SdOwner::None) == kSdOwnerCount, "None follows the owners and is not an index");

} // namespace devices::sd
