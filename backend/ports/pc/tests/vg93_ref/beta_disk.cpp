// SPDX-License-Identifier: MIT
#include "beta_disk.h"

#include <cstring>

#include "platform/compiler.h"

namespace devices::bdi {

namespace {

constexpr int kDriveCylinders = 84; // головка встаёт на упор за 80-м цилиндром

} // namespace

void BetaDisk::init(uint8_t* cache) {
    cache_[0]             = cache;
    cache_[1]             = cache + kTrackBytes;
    face_                 = SlotDrive{};
    face_.track_for       = &BetaDisk::track_for;
    face_.user            = this;
    face_.select_from_hld = true;
    face_.ncyl            = kDriveCylinders;
    for (int i = 0; i < kDrives; i++) {
        slot_[i]       = Slot{};
        slot_[i].phase = static_cast<uint32_t>(i) * 23456u % kSlotsPerRev;
    }
    cache_drive_ = -1;
    cache_cyl_   = -1;
    cur_         = 0;
    vg93_.init(&face_);
    vg93_.ready_from_hld  = true;
    vg93_.hlt_follows_hld = false;
    reset();
}

void BetaDisk::reset() {
    sysreg_ = 0xff; // любое отличие от 0 - чтобы write_system применил всё
    write_system(0);
}

void BetaDisk::advance(uint64_t clk) {
    if (vg93_.clk() < clk) vg93_.run_half(2 * clk - 1 - vg93_.t);
}

uint8_t BetaDisk::read_reg(unsigned reg) {
    return vg93_.read(reg & 3);
}

void BetaDisk::write_reg(unsigned reg, uint8_t v) {
    vg93_.write(reg & 3, v);
}

void BetaDisk::write_system(uint8_t v) {
    const uint8_t old = sysreg_;
    sysreg_           = v;
    if ((v & kSysDriveMask) != (old & kSysDriveMask)) select(v & kSysDriveMask);
    vg93_.mr       = (v & kSysResetNBit) == 0;
    vg93_.hlt      = (v & kSysHltBit) != 0;
    face_.ip_force = vg93_.hlt ? Force::Drive : Force::On; // HLT = 0: /IP прижат к нулю
    face_.set_side((v & kSysSideBit) != 0 ? 0 : 1);
    vg93_.pins_changed();
}

// Запись в сторону, которой источник ещё не отдал (разметка сразу после
// шага), идёт в чистую дорожку: прочитанное позже её не затирает.
uint8_t* BetaDisk::track_for(void* user, SlotDrive& d, int cyl, int side, bool create) {
    auto* b = static_cast<BetaDisk*>(user);
    Slot& s = b->slot_[b->cur_];
    s.cyl   = d.cyl;
    if (s.src == nullptr || cyl < 0 || cyl >= kMaxCylinders || side < 0 || side > 1) return nullptr;
    if (b->cache_drive_ != b->cur_ || b->cache_cyl_ != cyl) {
        b->store_cache();
        b->cache_drive_ = b->cur_;
        b->cache_cyl_   = cyl;
        b->cache_ready_ = 0;
        b->fill_cache();
    }
    const auto bit = static_cast<uint8_t>(1u << side);
    if ((b->cache_ready_ & bit) == 0 && create) {
        std::memset(b->cache_[side], 0, kTrackBytes);
        b->cache_ready_ = static_cast<uint8_t>(b->cache_ready_ | bit);
    }
    return (b->cache_ready_ & bit) != 0 ? b->cache_[side] : nullptr;
}

SOUNDSINTH_NOINLINE void BetaDisk::fill_cache() {
    const unsigned want = 3u & ~static_cast<unsigned>(cache_ready_);
    if (want == 0) return;
    if (slot_[cache_drive_].src->cylinder(cache_cyl_, cache_, want)) {
        cache_ready_     = 3;
        const uint32_t w = static_cast<uint32_t>(vg93_.t) - wait_from_;
        if (waiting_ && w > wait_max_) wait_max_ = w;
        waiting_ = false;
    } else if (!waiting_) {
        waiting_   = true;
        wait_from_ = static_cast<uint32_t>(vg93_.t);
        waits_++;
    }
}

void BetaDisk::source_ready(int drive) {
    if (drive != cache_drive_ || cache_ready_ == 3 || slot_[drive].src == nullptr) return;
    fill_cache();
    if (drive != cur_) return;
    face_.refresh();
    vg93_.pins_changed();
}

bool BetaDisk::store_cache() {
    if (cache_drive_ < 0 || face_.written == 0) return true;
    DiskSource* src = slot_[cache_drive_].src;
    bool ok         = true;
    for (int side = 0; side < 2 && src != nullptr; side++) {
        if ((face_.written & (1u << side)) != 0) ok = src->written(cache_cyl_, side, cache_[side]) && ok;
    }
    face_.written = 0;
    if (!ok) store_failed_ = true;
    return ok;
}

void BetaDisk::select(int drive) {
    store_cache();
    cur_ = drive;
    apply_drive();
}

// Поля дисковода глазами контроллера - по выбранному: угол от общего
// вращения (слот = 4 полупериода CLK), цилиндр, диск, защита записи.
void BetaDisk::apply_drive() {
    const Slot& s = slot_[cur_];
    face_.cyl     = s.cyl;
    face_.disk    = s.src != nullptr;
    face_.wprot   = s.src == nullptr || s.src->write_protect();
    face_.pos     = static_cast<uint32_t>((vg93_.t / 4 + s.phase) % kSlotsPerRev);
    face_.refresh();
}

void BetaDisk::insert(int drive, DiskSource* src) {
    if (drive == cache_drive_) {
        store_cache();
        cache_drive_ = -1;
    }
    slot_[drive].src = src;
    if (drive == cur_) {
        apply_drive();
        vg93_.pins_changed();
    }
}

void BetaDisk::swap(int a, int b) {
    store_cache();
    cache_drive_        = -1;
    DiskSource* const t = slot_[a].src;
    slot_[a].src        = slot_[b].src;
    slot_[b].src        = t;
    apply_drive();
    vg93_.pins_changed();
}

void BetaDisk::eject(int drive) {
    insert(drive, nullptr);
}

bool BetaDisk::flush() {
    store_cache();
    const bool ok = !store_failed_;
    store_failed_ = false;
    return ok;
}

} // namespace devices::bdi
