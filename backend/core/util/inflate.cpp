// SPDX-License-Identifier: MIT
#include "core/util/inflate.h"

#include <initializer_list>

namespace soundsinth::util {

namespace {

constexpr int kMaxBits             = 15;
constexpr int kLitCodes            = 288;
constexpr int kDistCodes           = 30;
constexpr uint32_t kServiceSymbols = 4096;

struct Stream {
    const uint8_t* src;
    size_t len, at;
    uint8_t* dst;
    size_t cap, out;
    uint32_t bits;
    int nbits;
    bool bad;
    InflateWork* work;
    void (*service)(void*);
    void* user;
    uint32_t until; // символов до обслуживания

    // n младших разрядов потока, n до 16; за концом входа - ошибка.
    uint32_t take(int n) {
        while (nbits < n) {
            if (at == len) {
                bad = true;
                return 0;
            }
            bits  |= static_cast<uint32_t>(src[at++]) << nbits;
            nbits += 8;
        }
        const uint32_t v   = bits & ((1u << n) - 1u);
        bits             >>= n;
        nbits             -= n;
        return v;
    }
};

// Канонический код: сколько символов каждой длины и символы по порядку кодов.
struct Code {
    uint16_t count[kMaxBits + 1];
    uint16_t* symbol;
};

// false - длины не образуют полного кода (перебор или недобор).
bool build(Code& c, const uint8_t* lengths, int n) {
    for (uint16_t& k : c.count) {
        k = 0;
    }
    for (int i = 0; i < n; i++) {
        c.count[lengths[i]]++;
    }
    int left = 1;
    for (int len = 1; len <= kMaxBits; len++) {
        left = left * 2 - c.count[len];
        if (left < 0) return false;
    }
    uint16_t offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; len++) {
        offs[len + 1] = static_cast<uint16_t>(offs[len] + c.count[len]);
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i] != 0) c.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    // Недобор допустим только у кода из одного символа (расстояния).
    return left == 0 || n - c.count[0] == 1;
}

// Символ по коду; -1 - кода такой длины нет или вход кончился.
int decode(Stream& s, const Code& c) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; len++) {
        code |= static_cast<int>(s.take(1));
        if (s.bad) return -1;
        const int count = c.count[len];
        if (code - count < first) return c.symbol[index + (code - first)];
        index  += count;
        first   = (first + count) * 2;
        code  <<= 1;
    }
    return -1;
}

// clang-format off
constexpr uint16_t kLenBase[29]   = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29]   = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30]  = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30]  = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
// clang-format on

// Блок с кодами: литералы и пары длина-расстояние до символа конца.
bool block(Stream& s, const Code& lit, const Code& dist) {
    for (;;) {
        if (s.service != nullptr && --s.until == 0) {
            s.until = kServiceSymbols;
            s.service(s.user);
        }
        const int sym = decode(s, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (s.out == s.cap) return false;
            s.dst[s.out++] = static_cast<uint8_t>(sym);
            continue;
        }
        if (sym == 256) return true;
        if (sym >= 257 + 29) return false;
        const size_t len = kLenBase[sym - 257] + s.take(kLenExtra[sym - 257]);
        const int d      = decode(s, dist);
        if (d < 0 || d >= kDistCodes) return false;
        const size_t back = kDistBase[d] + s.take(kDistExtra[d]);
        if (s.bad || back > s.out || len > s.cap - s.out) return false;
        for (size_t i = 0; i < len; i++, s.out++) {
            s.dst[s.out] = s.dst[s.out - back];
        }
    }
}

bool stored(Stream& s) {
    s.bits  = 0;
    s.nbits = 0;
    if (s.len - s.at < 4) return false;
    const size_t n = static_cast<size_t>(s.src[s.at] | (s.src[s.at + 1] << 8));
    if (n != static_cast<size_t>(~(s.src[s.at + 2] | (s.src[s.at + 3] << 8)) & 0xffff)) return false;
    s.at += 4;
    if (n > s.len - s.at || n > s.cap - s.out) return false;
    for (size_t i = 0; i < n; i++) {
        s.dst[s.out++] = s.src[s.at++];
    }
    return true;
}

bool fixed(Stream& s) {
    uint8_t* const lengths = s.work->lengths;
    Code lit{{}, s.work->lit_symbol}, dist{{}, s.work->dist_symbol};
    for (int i = 0; i < kLitCodes; i++) {
        lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    }
    build(lit, lengths, kLitCodes);
    for (int i = 0; i < kDistCodes; i++) {
        lengths[i] = 5;
    }
    build(dist, lengths, kDistCodes);
    return block(s, lit, dist);
}

bool dynamic(Stream& s) {
    uint8_t* const lengths = s.work->lengths;
    Code lit{{}, s.work->lit_symbol}, dist{{}, s.work->dist_symbol};
    const int nlen = static_cast<int>(s.take(5)) + 257, ndist = static_cast<int>(s.take(5)) + 1, ncode = static_cast<int>(s.take(4)) + 4;
    if (s.bad || nlen > 286 || ndist > kDistCodes) return false;
    for (int i = 0; i < 19; i++) {
        lengths[kLengthOrder[i]] = i < ncode ? static_cast<uint8_t>(s.take(3)) : uint8_t{0};
    }
    // Код длин кодов - в таблице литералов, пока она не нужна.
    if (!build(lit, lengths, 19)) return false;
    for (int i = 0; i < nlen + ndist;) {
        const int sym = decode(s, lit);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = static_cast<uint8_t>(sym);
            continue;
        }
        if (sym == 16 && i == 0) return false;
        const uint8_t value = sym == 16 ? lengths[i - 1] : uint8_t{0};
        int repeat          = sym == 16 ? 3 + static_cast<int>(s.take(2)) : sym == 17 ? 3 + static_cast<int>(s.take(3)) : 11 + static_cast<int>(s.take(7));
        if (s.bad || i + repeat > nlen + ndist) return false;
        for (; repeat != 0; repeat--) {
            lengths[i++] = value;
        }
    }
    if (lengths[256] == 0 || !build(lit, lengths, nlen) || !build(dist, lengths + nlen, ndist)) return false;
    return block(s, lit, dist);
}

} // namespace

size_t inflate(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, InflateWork& work, void (*service)(void*), void* user) {
    Stream s{src, src_len, 0, dst, dst_cap, 0, 0, 0, false, &work, service, user, kServiceSymbols};
    for (;;) {
        const bool last     = s.take(1) != 0;
        const uint32_t type = s.take(2);
        if (s.bad || type == 3) return 0;
        if (!(type == 0 ? stored(s) : type == 1 ? fixed(s) : dynamic(s))) return 0;
        if (last) return s.out;
    }
}

bool gzip_parse(const uint8_t* src, size_t len, size_t& data_at, uint32_t& out_len) {
    if (len < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) return false;
    const uint8_t flags = src[3];
    size_t at           = 10;
    if ((flags & 0x04u) != 0) { // дополнительное поле
        if (at + 2 > len) return false;
        at += 2u + static_cast<size_t>(src[at] | (src[at + 1] << 8));
    }
    for (const uint8_t text : {uint8_t{0x08}, uint8_t{0x10}}) { // имя, комментарий
        if ((flags & text) == 0) continue;
        while (at < len && src[at] != 0) {
            at++;
        }
        at++;
    }
    if ((flags & 0x02u) != 0) at += 2; // сумма заголовка
    if (at + 8 > len) return false;
    data_at = at;
    out_len = static_cast<uint32_t>(src[len - 4]) | (static_cast<uint32_t>(src[len - 3]) << 8) | (static_cast<uint32_t>(src[len - 2]) << 16) |
              (static_cast<uint32_t>(src[len - 1]) << 24);
    return true;
}

} // namespace soundsinth::util
