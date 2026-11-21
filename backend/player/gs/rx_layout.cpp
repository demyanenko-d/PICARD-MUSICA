// SPDX-License-Identifier: MIT
#include "player/gs/rx_layout.h"

#include <cstring>

namespace player::gs {
namespace {

using soundsinth::memory::kPsramPageBytes;

// Заголовок ProTracker: 20 байт названия, затем 31 запись о сэмплах по 30
// байт; длина сэмпла в словах - байты 22 и 23 записи, старшим вперёд.
constexpr uint32_t kTitleBytes      = 20;
constexpr uint32_t kSampleInfoBytes = 30;
constexpr uint32_t kSampleWordsAt   = 22;
// Метка формата - последние четыре байта заголовка.
constexpr uint32_t kTagAt = RxLayout::kHeaderBytes - 4u;
// Последний номер паттерна в таблице порядка; дальше - конец таблицы.
constexpr uint8_t kPatternMax = 0x7fu;

uint32_t page_up(uint32_t v) {
    return (v + kPsramPageBytes - 1u) & ~(kPsramPageBytes - 1u);
}

} // namespace

void RxLayout::decide(const uint8_t* p, bool mod) {
    decided_ = true;
    on_      = false;
    if (!mod) return;
    const uint8_t* const tag = p + kTagAt;
    if (std::memcmp(tag, "M.K.", 4) != 0 && std::memcmp(tag, "M!K!", 4) != 0 && std::memcmp(tag, "4CHN", 4) != 0) return;
    const uint8_t* const order = p + devices::gs::kModOrderTable;
    uint32_t top               = 0;
    bool any                   = false;
    for (uint32_t i = 0; i < devices::gs::kModOrderCount && order[i] <= kPatternMax; ++i) {
        any = true;
        if (order[i] > top) top = order[i];
    }
    if (!any) return;
    for (uint32_t i = 0; i < kSamples; ++i) {
        const uint8_t* const w = p + kTitleBytes + kSampleInfoBytes * i + kSampleWordsAt;
        words_[i]              = static_cast<uint16_t>((w[0] << 8) | w[1]);
    }
    head_ = kHeaderBytes + (top + 1u) * devices::gs::kPatternBytes;
    on_   = true;
}

uint32_t RxLayout::place(uint32_t at, uint32_t* room, uint32_t* sample) const {
    if (sample) *sample = kSamples;
    if (!decided_ && at < kHeaderBytes) {
        *room = kHeaderBytes - at;
        return at;
    }
    *room = UINT32_MAX;
    if (!on_ || at < head_) {
        if (on_) *room = head_ - at;
        return at;
    }
    uint32_t f = head_, b = head_;
    for (uint32_t i = 0; i < kSamples; ++i) {
        const uint32_t len = sample_bytes(i);
        if (words_[i] > 1u) b = page_up(b); // одно слово ProTracker - пустой сэмпл
        if (at < f + len) {
            *room = f + len - at;
            if (sample) *sample = i;
            return b + (at - f);
        }
        f += len;
        b += len;
    }
    return b + (at - f);
}

} // namespace player::gs
