// SPDX-License-Identifier: MIT
#include "mfm.h"

#include "slot_drive.h"

namespace devices::bdi {

namespace {

// Разряд k байта - в разряд 2k слова.
constexpr uint32_t spread8(uint32_t x) {
    x = (x | (x << 4)) & 0x0f0fu;
    x = (x | (x << 2)) & 0x3333u;
    return (x | (x << 1)) & 0x5555u;
}

// Биты байта в порядке записи: разряд k - бит k от старшего.
constexpr uint32_t rev8(uint32_t d) {
    d = ((d & 0xf0u) >> 4) | ((d & 0x0fu) << 4);
    d = ((d & 0xccu) >> 2) | ((d & 0x33u) << 2);
    return ((d & 0xaau) >> 1) | ((d & 0x55u) << 1);
}

} // namespace

void build_mfm_track(uint8_t* bits, const SectorSpec* s, int count, int gap3) {
    static Track t; // 7 КБ - не на стек; код только ПК, один поток
    track_build(t, s, count, gap3);
    track_to_cells(t, bits);
}

namespace {

int dcell(const uint8_t* t, uint32_t slots, uint32_t i) {
    i %= slots;
    return (t[i >> 3] >> (i & 7)) & 1;
}

uint16_t draw16(const uint8_t* t, uint32_t slots, uint32_t p) {
    uint16_t v = 0;
    for (uint32_t k = 0; k < 16; k++) {
        v = static_cast<uint16_t>((v << 1) | dcell(t, slots, p + k));
    }
    return v;
}

// p - ячейка синхроимпульса первого бита.
uint8_t dbyte(const uint8_t* t, uint32_t slots, uint32_t p) {
    uint8_t v = 0;
    for (uint32_t k = 0; k < 8; k++) {
        v = static_cast<uint8_t>((v << 1) | dcell(t, slots, p + 2 * k + 1));
    }
    return v;
}

} // namespace

int decode_mfm_track(const uint8_t* t, uint32_t slots, FoundSector* out, int cap, uint8_t* data, size_t data_cap) {
    int n = 0, cur = -1;
    uint32_t cur_pos = 0;
    size_t used      = 0;
    uint16_t w       = 0;
    if (t == nullptr) return 0;
    // Лишние 2000 ячеек: поле, начатое у конца оборота, дочитывается.
    for (uint32_t i = 0; i < slots + 2000; i++) {
        w = static_cast<uint16_t>((w << 1) | dcell(t, slots, i));
        if (w != 0x4489) continue;
        uint32_t p = i + 1;
        // Не больше четырёх A1 подряд: дорожка, записанная сплошь байтом
        // #F5, - одни синхрослова, и цикл без предела не кончается.
        int a1 = 1;
        while (a1 < 4 && draw16(t, slots, p) == 0x4489) {
            a1++;
            p += 16;
        }
        const uint8_t mark  = dbyte(t, slots, p);
        p                  += 16;
        uint16_t crc        = 0xffff;
        for (int k = 0; k < a1; k++) {
            crc = crc16(crc, kA1x3, 1);
        }
        crc = crc16(crc, &mark, 1);
        if (mark == 0xfe && i < slots && n < cap) {
            uint8_t f[6];
            for (uint32_t k = 0; k < 6; k++) {
                f[k] = dbyte(t, slots, p + 16 * k);
            }
            FoundSector& r  = out[n];
            r               = FoundSector{};
            r.c             = f[0];
            r.h             = f[1];
            r.r             = f[2];
            r.n             = f[3];
            r.id_crc_ok     = crc16(crc, f, 6) == 0;
            cur             = n++;
            cur_pos         = i;
            p              += 16 * 6;
        } else if (mark >= 0xf8 && mark <= 0xfb && cur >= 0 && !out[cur].has_data && i - cur_pos < 16 * 64) {
            FoundSector& r    = out[cur];
            const size_t size = static_cast<size_t>(128) << (r.n & 3);
            const bool fits   = used + size <= data_cap;
            for (size_t k = 0; k < size + 2; k++) {
                const uint8_t b = dbyte(t, slots, p + 16 * static_cast<uint32_t>(k));
                if (k < size && fits) data[used + k] = b;
                crc = crc16(crc, &b, 1);
            }
            if (fits) {
                r.has_data     = true;
                r.deleted      = mark < 0xfa;
                r.data_crc_ok  = crc == 0;
                r.data_off     = static_cast<uint32_t>(used);
                used          += size;
            }
            p += 16 * static_cast<uint32_t>(size + 2);
        }
        i = p - 1;
        w = 0;
    }
    return n;
}

void track_from_cells(Track& t, const uint8_t* bits) {
    uint32_t prev = 0;
    for (uint32_t k = 0; k < kTrackLen; k++) {
        const uint32_t w = bits[2 * k] | (bits[2 * k + 1] << 8);
        uint32_t d = 0, c = 0;
        for (uint32_t j = 0; j < 8; j++) {
            c |= ((w >> (2 * j)) & 1u) << j;
            d |= ((w >> (2 * j + 1)) & 1u) << j;
        }
        const uint32_t expect = ~(d | (d << 1) | prev) & 0xffu;
        t.put(k, static_cast<uint8_t>(rev8(d)), c != expect);
        prev = (d >> 7) & 1u;
    }
    track_index(t);
}

void track_to_cells(const Track& t, uint8_t* bits) {
    uint32_t prev = 0;
    for (uint32_t k = 0; k < kTrackLen; k++) {
        const uint32_t d = rev8(t.data[k]);
        uint32_t c       = ~(d | (d << 1) | prev) & 0xffu;
        if (t.mark(k)) c &= ~(1u << (t.data[k] == 0xc2 ? 4 : 5));
        const uint32_t w = spread8(c) | (spread8(d) << 1);
        bits[2 * k]      = static_cast<uint8_t>(w);
        bits[2 * k + 1]  = static_cast<uint8_t>(w >> 8);
        prev             = t.data[k] & 1u;
    }
    static_assert(2 * kTrackLen == kTrackBytes, "a track byte is 16 slots");
}

} // namespace devices::bdi
