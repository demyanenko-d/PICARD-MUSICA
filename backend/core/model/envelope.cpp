// SPDX-License-Identifier: MIT
#include "core/model/envelope.h"

namespace soundsinth::model {

void sanitize_tracker_envelope(Envelope& env) {
    if (env.point_count == 0) {
        env.loop_start = env.loop_end = env.sustain_point = env.sustain_end = 0;
        return;
    }
    EnvelopePoint* p = env.points;
    for (uint8_t i = 1; i < env.point_count; ++i) {
        if (p[i].tick < p[i - 1].tick && (p[i].tick & 0xFF00u) == 0) {
            p[i].tick = static_cast<uint16_t>(p[i].tick | (p[i - 1].tick & 0xFF00u));
            if (p[i].tick < p[i - 1].tick) p[i].tick = static_cast<uint16_t>(p[i].tick + 0x100u);
        }
    }
    p[0].tick = 0;
    for (uint8_t i = 0; i < env.point_count; ++i) {
        if (i > 0 && p[i].tick < p[i - 1].tick) p[i].tick = p[i - 1].tick;
        if (p[i].value < 0) p[i].value = 0;
        if (p[i].value > 64) p[i].value = 64;
    }
    const uint8_t last = static_cast<uint8_t>(env.point_count - 1);
    if (env.loop_end > last) env.loop_end = last;
    if (env.loop_start > env.loop_end) env.loop_start = env.loop_end;
    if (env.sustain_end > last) env.sustain_end = last;
    if (env.sustain_point > env.sustain_end) env.sustain_point = env.sustain_end;
}

} // namespace soundsinth::model
