// SPDX-License-Identifier: MIT
#include "core/chips/blep.h"

namespace soundsinth::chips {

const int16_t kBlepTable[kBlepPhases][kBlepTaps] = {
#include "core/chips/blep_table.inc"
};

} // namespace soundsinth::chips
