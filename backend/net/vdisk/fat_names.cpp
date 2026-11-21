// SPDX-License-Identifier: MIT
// Имена записей FAT: UTF-8 поставщика -> длинное UTF-16 и короткое cp866.

#include "net/vdisk/fat_names.h"

#include <cstring>

namespace net::vdisk {
namespace {

// U+00C0..U+017F: основа ASCII латинской буквы, '_' - не буква.
constexpr char kLatin[] = "AAAAAAACEEEEIIIIDNOOOOO_OUUUUYTsaaaaaaaceeeeiiii"
                          "dnooooo_ouuuuytyAaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGg"
                          "GgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOo"
                          "OoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
static_assert(sizeof(kLatin) - 1u == 0x180u - 0xc0u, "latin table covers U+00C0..U+017F");

constexpr uint32_t kDecodeMax = 2u * kNameMax + 8u;

// Знак длинного имени: то, что показывают Proteus и Wild Commander.
uint16_t long_unit(uint32_t c) {
    if (c < 0x20u || c == 0x7fu) return '_';
    if (c < 0x80u) return std::strchr("\\/:*?\"<>|", static_cast<int>(c)) != nullptr ? '_' : static_cast<uint16_t>(c);
    if (c == 0xa0u) return ' ';
    if (c >= 0xc0u && c < 0x180u) return static_cast<uint16_t>(kLatin[c - 0xc0u]);
    if (c == 0x401u || c == 0x451u || (c >= 0x410u && c <= 0x44fu)) return static_cast<uint16_t>(c);
    switch (c) {
        case 0x400u:
        case 0x404u:
            return 0x415u; // Ѐ Є -> Е
        case 0x450u:
        case 0x454u:
            return 0x435u;
        case 0x406u:
        case 0x407u:
            return 'I';
        case 0x456u:
        case 0x457u:
            return 'i';
        case 0x40du:
            return 0x418u;
        case 0x45du:
            return 0x438u;
        case 0x40eu:
            return 0x423u; // Ў -> У
        case 0x45eu:
            return 0x443u;
        case 0x490u:
            return 0x413u; // Ґ -> Г
        case 0x491u:
            return 0x433u;
        case 0x2013u:
        case 0x2014u:
            return '-';
        case 0x2018u:
        case 0x2019u:
        case 0x201cu:
        case 0x201du:
            return '\'';
        case 0x2026u:
            return '.';
        default:
            return '_';
    }
}

// UTF-8 по знакам; неполная последовательность в конце отбрасывается,
// неверная - '_'.
uint32_t decode(const char* s, uint16_t* out, uint32_t cap) {
    const auto* p = reinterpret_cast<const uint8_t*>(s);
    uint32_t n    = 0;
    while (*p != 0u && n < cap) {
        uint32_t c     = *p++;
        uint32_t extra = 0;
        if (c >= 0xf0u) {
            extra = 3;
            c     = 0x110000u; // вне BMP - '_'
        } else if (c >= 0xe0u) {
            extra  = 2;
            c     &= 0x0fu;
        } else if (c >= 0xc0u) {
            extra  = 1;
            c     &= 0x1fu;
        } else if (c >= 0x80u) {
            out[n++] = '_';
            continue;
        }
        bool cut = false;
        for (uint32_t i = 0; i < extra; ++i) {
            if ((*p & 0xc0u) != 0x80u) {
                cut = *p == 0u;
                c   = 0x110000u;
                break;
            }
            c = (c << 6) | (*p++ & 0x3fu);
        }
        if (cut) break;
        out[n++] = long_unit(c);
    }
    return n;
}

uint16_t upper(uint16_t c) {
    if (c >= 'a' && c <= 'z') return static_cast<uint16_t>(c - 'a' + 'A');
    if (c >= 0x430u && c <= 0x44fu) return static_cast<uint16_t>(c - 0x20u);
    if (c == 0x451u) return 0x401u;
    return c;
}

// Заглавный знак в cp866; 0 - в cp866 его нет.
uint8_t oem(uint16_t c) {
    if (c < 0x80u) return static_cast<uint8_t>(c);
    if (c >= 0x410u && c <= 0x42fu) return static_cast<uint8_t>(0x80u + (c - 0x410u));
    if (c == 0x401u) return 0xf0u;
    return 0;
}

bool sfn_byte(uint8_t c) {
    if (c >= 0x80u) return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return c != 0u && std::strchr("$%'-_@`!(){}^#&~", c) != nullptr;
}

} // namespace

uint32_t fat_ext_len(const uint16_t* name, uint32_t len) {
    for (uint32_t k = 1; k <= 5u && k < len; ++k) {
        if (name[len - k] == '.') return k;
    }
    return 0;
}

uint32_t fat_long_name(const char* utf8, uint16_t* out) {
    uint16_t tmp[kDecodeMax];
    uint32_t n     = decode(utf8, tmp, kDecodeMax);
    uint32_t start = 0;
    while (start < n && tmp[start] == ' ') {
        ++start;
    }
    while (n > start && (tmp[n - 1u] == ' ' || tmp[n - 1u] == '.')) {
        --n;
    }
    const uint16_t* s  = tmp + start;
    n                 -= start;
    if (n == 0u) {
        out[0] = '_';
        return 1;
    }
    const uint32_t ext  = fat_ext_len(s, n);
    const uint32_t keep = n > kNameMax ? kNameMax - ext : n - ext;
    std::memcpy(out, s, keep * sizeof(uint16_t));
    std::memcpy(out + keep, s + n - ext, ext * sizeof(uint16_t));
    return keep + ext;
}

bool fat_exact_sfn(const uint16_t* name, uint32_t len, uint8_t sfn[11], uint8_t& case_bits) {
    uint32_t dot = len;
    for (uint32_t i = 0; i < len; ++i) {
        if (name[i] == '.') dot = i;
    }
    const uint32_t base = dot;
    const uint32_t ext  = dot < len ? len - dot - 1u : 0u;
    if (base == 0u || base > 8u || ext > 3u || (dot < len && ext == 0u)) return false;
    std::memset(sfn, ' ', 11);
    bool lower_part[2] = {false, false};
    bool upper_part[2] = {false, false};
    for (uint32_t i = 0; i < len; ++i) {
        if (i == dot) continue;
        const uint16_t c = name[i];
        if (c >= 0x80u || c == '~') return false;
        const uint32_t part = i < base ? 0u : 1u;
        lower_part[part]    = lower_part[part] || (c >= 'a' && c <= 'z');
        upper_part[part]    = upper_part[part] || (c >= 'A' && c <= 'Z');
        const auto u        = static_cast<uint8_t>(upper(c));
        if (!sfn_byte(u)) return false;
        sfn[part == 0u ? i : 8u + i - base - 1u] = u;
    }
    if ((lower_part[0] && upper_part[0]) || (lower_part[1] && upper_part[1])) return false;
    case_bits = static_cast<uint8_t>((lower_part[0] ? 0x08u : 0u) | (lower_part[1] ? 0x10u : 0u));
    return true;
}

SfnBasis fat_sfn_basis(const uint16_t* name, uint32_t len) {
    SfnBasis b{};
    uint32_t dot = len;
    for (uint32_t i = 0; i < len; ++i) {
        if (name[i] == '.') dot = i;
    }
    // Точка в начале не отделяет расширение.
    uint32_t lead = 0;
    while (lead < len && (name[lead] == '.' || name[lead] == ' ')) {
        ++lead;
    }
    if (dot < lead) dot = len;
    if (lead != 0u) b.lossy = true;
    auto put = [&](uint16_t c, uint8_t* dst, uint8_t& n, uint32_t cap) {
        if (c == ' ' || c == '.') {
            b.lossy = true;
            return;
        }
        uint8_t o = oem(upper(c));
        if (o == 0u || !sfn_byte(o)) {
            o       = '_';
            b.lossy = true;
        }
        if (n < cap) {
            dst[n++] = o;
        } else {
            b.lossy = true;
        }
    };
    for (uint32_t i = lead; i < dot; ++i) {
        put(name[i], b.base, b.base_len, 8);
    }
    for (uint32_t i = dot + 1u; i < len; ++i) {
        put(name[i], b.ext, b.ext_len, 3);
    }
    if (b.base_len == 0u) {
        b.base[b.base_len++] = '_';
        b.lossy              = true;
    }
    return b;
}

void fat_sfn_candidate(const SfnBasis& b, uint32_t n, uint16_t hash, uint8_t sfn[11]) {
    std::memset(sfn, ' ', 11);
    std::memcpy(sfn + 8, b.ext, b.ext_len);
    if (n == 0u) {
        std::memcpy(sfn, b.base, b.base_len);
    } else {
        char tail[12];
        uint32_t t      = 0;
        uint32_t number = n <= 4u ? n : n - 4u;
        char digits[10];
        uint32_t d = 0;
        do {
            digits[d++]  = static_cast<char>('0' + number % 10u);
            number      /= 10u;
        } while (number != 0u && d < 7u);
        tail[t++] = '~';
        while (d > 0u) {
            tail[t++] = digits[--d];
        }
        uint8_t head[8];
        uint32_t h = 0;
        if (n <= 4u) {
            for (; h < b.base_len && h < 6u; ++h) {
                head[h] = b.base[h];
            }
        } else {
            for (; h < b.base_len && h < 2u; ++h) {
                head[h] = b.base[h];
            }
            static constexpr char kHex[] = "0123456789ABCDEF";
            for (int s = 12; s >= 0; s -= 4) {
                head[h++] = static_cast<uint8_t>(kHex[(hash >> s) & 0xfu]);
            }
        }
        if (h + t > 8u) h = 8u - t;
        std::memcpy(sfn, head, h);
        std::memcpy(sfn + h, tail, t);
    }
    // 0xE5 в первом байте - метка удалённой записи.
    if (sfn[0] == 0xe5u) sfn[0] = 0x05u;
}

uint16_t fat_fold(uint16_t c) {
    if (c >= 'A' && c <= 'Z') return static_cast<uint16_t>(c - 'A' + 'a');
    if (c >= 0x410u && c <= 0x42fu) return static_cast<uint16_t>(c + 0x20u);
    if (c == 0x401u) return 0x451u;
    return c;
}

} // namespace net::vdisk
