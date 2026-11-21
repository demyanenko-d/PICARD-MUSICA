// SPDX-License-Identifier: MIT
#include "devices/bdi/drive.h"

namespace devices::bdi {

Track* Drive::track(int side, uint64_t now, bool& started) {
    started = false;
    if (disk == nullptr) return nullptr;
    if (tracks == nullptr) {
        tracks = disk->cylinder(cyl);
        if (tracks == nullptr && !waiting) {
            waiting = true;
            wait_t  = now;
            started = true;
        }
    }
    return tracks != nullptr ? &tracks[side] : nullptr;
}

bool Drive::step(int dir, int max_cyl) {
    const int c   = cyl + dir;
    const int top = disk != nullptr && disk->cylinders() > max_cyl ? disk->cylinders() - 1 : max_cyl;
    if (c < 0 || c > top || c == cyl) return false;
    cyl = c;
    drop();
    return true;
}

} // namespace devices::bdi
