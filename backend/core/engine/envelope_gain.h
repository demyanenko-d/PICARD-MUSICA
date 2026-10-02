// SPDX-License-Identifier: MIT
#pragma once

// Множитель огибающей громкости канала: общий у сведения и у выбора голоса
// по громкости.

#include <cstdint>

#include "core/engine/channel_state.h"
#include "core/engine/engine_defs.h"

namespace soundsinth::engine {

// Огибающая в децибелах (kQuirkEnvelopeDecibel): 0..64 - шаг 1.5 дБ, 64 =
// 1.0, 0 = тишина; 96 дБ против 36 у линейной. Таблица, а не pow: 260 байт.
inline constexpr uint32_t kEnvelopeDbGainQ16[65] = {
    0,    1,    1,    2,    2,    2,    3,    3,    4,    5,    6,     7,     8,     10,    12,    14,    16,    20,    23,    28,    33,    39,
    46,   55,   66,   78,   93,   110,  131,  155,  185,  220,  261,   310,   369,   438,   521,   619,   735,   874,   1039,  1234,  1467,  1744,
    2072, 2463, 2927, 3479, 4135, 4915, 5841, 6942, 8250, 9806, 11654, 13851, 16462, 19565, 23253, 27636, 32846, 39037, 46396, 55142, 65536,
};

inline uint32_t envelope_db_gain_q16(int32_t v) {
    if (v <= 0) return 0;
    if (v >= 64) return kQ16One;
    return kEnvelopeDbGainQ16[v];
}

// Множитель огибающей громкости Q0.16: амплитуда 0..64 у трекеров, децибелы
// (таблица) у .mid.
inline uint32_t envelope_gain_q16(const ChannelState& cs, bool envelope_db) {
    return envelope_db ? envelope_db_gain_q16(cs.envelope_volume) : static_cast<uint32_t>(cs.envelope_volume) * (kQ16One / 64);
}

} // namespace soundsinth::engine
