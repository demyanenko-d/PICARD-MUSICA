// SPDX-License-Identifier: MIT
// Счётчики кэша XIP: CTR_ACC и CTR_HIT, запись обнуляет. Обращения между
// чтением и обнулением теряются - единицы на миллионы.

#include "hardware/structs/xip_ctrl.h"

#include "player/hal/flash_cache.h"

namespace {
uint64_t s_acc = 0;
uint64_t s_hit = 0;
} // namespace

void player::hal::flash_cache_poll() {
    const uint32_t acc    = xip_ctrl_hw->ctr_acc;
    const uint32_t hit    = xip_ctrl_hw->ctr_hit;
    xip_ctrl_hw->ctr_acc  = 0;
    xip_ctrl_hw->ctr_hit  = 0;
    s_acc                += acc;
    s_hit                += hit;
}

void player::hal::flash_cache_take(uint64_t& accesses, uint64_t& hits) {
    flash_cache_poll();
    accesses = s_acc;
    hits     = s_hit;
    s_acc = s_hit = 0;
}
