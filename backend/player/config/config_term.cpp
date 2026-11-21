// SPDX-License-Identifier: MIT
#include "player/config/config_term.h"

#include <atomic>
#include <cstring>

namespace soundsinth::config {

namespace {

TermHeader& head(uint8_t* page) {
    return *reinterpret_cast<TermHeader*>(page);
}

// Равные знакоместа между изменёнными, которые дешевле вывести, чем
// начинать новый отрезок: у знаков - два, у цвета - шесть.
constexpr uint32_t kMergeGap = 2;
constexpr uint32_t kAttrGap  = 6;

} // namespace

void term_page_init(uint8_t* page) {
    std::memset(page, 0, kTermPageBytes);
    TermHeader& h = head(page);
    h.version     = kTermVersion;
    h.full        = 1;
    std::atomic_signal_fence(std::memory_order_release);
    h.magic = kTermMagic;
}

bool term_publish(uint8_t* page, const TermGrid& g) {
    volatile TermHeader& h = head(page);
    if (h.shown != h.upd) return false;
    uint8_t* const ch     = page + kTermCharsAt;
    uint8_t* const at     = page + kTermAttrsAt;
    TermSpan* const spans = reinterpret_cast<TermSpan*>(page + sizeof(TermHeader));
    uint32_t n            = 0;
    bool all              = h.full != 0;
    // Отрезок знаков стоит ~280 тактов Z80 на знакоместо, отрезок цвета -
    // ~20: сначала знаки (с промежутками до kMergeGap), потом цвет того, что
    // знаками не покрыто.
    auto add = [&](uint32_t r, uint32_t c, uint32_t len, uint8_t kind) {
        if (n == kTermSpans) {
            all = true;
            return;
        }
        spans[n++] = TermSpan{static_cast<uint8_t>(r), static_cast<uint8_t>(c), static_cast<uint8_t>(len), kind};
    };
    for (uint32_t r = 1; r < kTermRows && !all; ++r) {
        const uint8_t* oc     = ch + r * kTermCols;
        const uint8_t* oa     = at + r * kTermCols;
        bool drawn[kTermCols] = {};
        for (uint32_t c = 0; c < kTermCols;) {
            if (oc[c] == g.ch[r][c]) {
                ++c;
                continue;
            }
            uint32_t last = c;
            for (uint32_t e = c + 1; e < kTermCols && e - last <= kMergeGap; ++e) {
                if (oc[e] != g.ch[r][e]) last = e;
            }
            for (uint32_t k = c; k <= last; ++k) {
                drawn[k] = true;
            }
            add(r, c, last + 1u - c, kTermSpanChars);
            c = last + 1u;
        }
        for (uint32_t c = 0; c < kTermCols;) {
            if (drawn[c] || oa[c] == g.at[r][c]) {
                ++c;
                continue;
            }
            uint32_t last = c;
            for (uint32_t e = c + 1; e < kTermCols && e - last <= kAttrGap && !drawn[e]; ++e) {
                if (oa[e] != g.at[r][e]) last = e;
            }
            add(r, c, last + 1u - c, kTermSpanAttr);
            c = last + 1u;
        }
    }
    if (n == 0 && !all) return true;
    std::memcpy(ch, g.ch, sizeof(g.ch));
    std::memcpy(at, g.at, sizeof(g.at));
    h.count = all ? kTermAll : static_cast<uint8_t>(n);
    h.full  = 0;
    std::atomic_signal_fence(std::memory_order_release);
    h.upd = static_cast<uint8_t>(h.upd + 1u);
    return true;
}

void term_resend(uint8_t* page) {
    static_cast<volatile TermHeader&>(head(page)).full = 1;
}

bool term_key(uint8_t* page, uint8_t& k) {
    volatile TermHeader& h = head(page);
    if (h.ack == h.req) return false;
    k     = h.keys[h.ack % kTermKeySlots];
    h.ack = static_cast<uint8_t>(h.ack + 1u);
    return true;
}

} // namespace soundsinth::config
