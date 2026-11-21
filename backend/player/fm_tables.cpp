// SPDX-License-Identifier: MIT
#include "player/fm_tables.h"

#include "ymfm_batch.h"

namespace player {

namespace {
ymfm::fm_tables s_tables;
bool s_built = false;
} // namespace

const ymfm::fm_tables& fm_tables() {
    if (!s_built) {
        ymfm::build_fm_tables(s_tables);
        s_built = true;
    }
    return s_tables;
}

} // namespace player
