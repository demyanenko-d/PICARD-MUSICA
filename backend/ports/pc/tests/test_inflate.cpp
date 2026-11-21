// SPDX-License-Identifier: MIT
// Распаковка gzip: блоки без сжатия, с постоянными и с динамическими кодами
// (потоки сделаны zlib), отказы на обрезанном входе и на тесном выходе,
// обслуживание на длинном потоке.

#include "testing.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/util/inflate.h"

namespace {

using soundsinth::util::gzip_parse;
using soundsinth::util::inflate;

soundsinth::util::InflateWork g_work;

// clang-format off
const uint8_t kStored[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x0a, 0x01, 0x1c, 0x00, 0xe3, 0xff, 0x56, 0x47, 0x4d, 0x20, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x20,
    0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x3a, 0x20, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0xe6, 0x25, 0xd8, 0x14, 0x1c, 0x00, 0x00, 0x00};
const uint8_t kFixed[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0a, 0x4b, 0x4c, 0x4a, 0x4e, 0x44, 0x45, 0x0a, 0x19, 0xa9, 0x39, 0x39, 0xf9, 0xc8, 0x24, 0x00, 0xae,
    0xd8, 0x6c, 0x3b, 0x24, 0x00, 0x00, 0x00};
const uint8_t kDynamic[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0a, 0x85, 0x92, 0x3b, 0x0a, 0x80, 0x40, 0x0c, 0x44, 0xaf, 0xb2, 0x47, 0xc8, 0xc4, 0xf8, 0xc3, 0xd3,
    0x58, 0x58, 0xd8, 0x8a, 0xb0, 0xd7, 0x17, 0x21, 0xd5, 0x13, 0x49, 0xf9, 0xd8, 0x61, 0xe6, 0x11, 0xb6, 0xef, 0xe7, 0xdd, 0xac, 0xf5, 0xeb, 0xbc, 0x8f, 0x66,
    0x5b, 0xeb, 0x2f, 0x47, 0xb2, 0x92, 0x97, 0xe4, 0x48, 0x56, 0xb2, 0x27, 0x8f, 0xe0, 0x15, 0x79, 0x47, 0xdf, 0x84, 0x3d, 0x19, 0x02, 0x03, 0x0a, 0x66, 0x0c,
    0x18, 0x38, 0x90, 0x5f, 0xd0, 0x27, 0x0c, 0x8e, 0x78, 0xff, 0x13, 0x76, 0x08, 0x3b, 0x85, 0x03, 0xc2, 0x82, 0xb0, 0x41, 0x58, 0x85, 0xb0, 0x17, 0x17, 0x0e,
    0x08, 0x0b, 0xc2, 0x06, 0x61, 0x55, 0xc2, 0x5e, 0x5c, 0x38, 0x8a, 0x2f, 0x61, 0x10, 0x56, 0x21, 0xec, 0xdf, 0x0b, 0x3f, 0x2e, 0x27, 0x38, 0xae, 0x83, 0x02,
    0x00, 0x00};
// clang-format on

std::string unpack(const uint8_t* gz, size_t len) {
    size_t at    = 0;
    uint32_t out = 0;
    CHECK(gzip_parse(gz, len, at, out));
    std::string s(out, '\0');
    CHECK_EQ(inflate(gz + at, len - at - 8, reinterpret_cast<uint8_t*>(&s[0]), out, g_work), static_cast<size_t>(out));
    return s;
}

void test_inflate() {
    std::printf("test_inflate\n");
    CHECK(unpack(kStored, sizeof(kStored)) == "VGM stored block: 0123456789");
    CHECK(unpack(kFixed, sizeof(kFixed)) == "abcabcabcabcabcabc hello hello hello");
    std::string text;
    for (int i = 0; i < 40; i++) {
        text += "wait " + std::to_string(i * 37 % 11) + " write " + std::to_string(i * i % 7) + "; ";
    }
    CHECK(unpack(kDynamic, sizeof(kDynamic)) == text);

    // Тесный выход и обрезанный вход - отказ, а не запись за край.
    size_t at    = 0;
    uint32_t out = 0;
    CHECK(gzip_parse(kDynamic, sizeof(kDynamic), at, out));
    std::vector<uint8_t> buf(out + 16, 0xEE);
    CHECK_EQ(inflate(kDynamic + at, sizeof(kDynamic) - at - 8, buf.data(), out - 1, g_work), size_t{0});
    CHECK(buf[out - 1] == 0xEE && buf[out] == 0xEE);
    CHECK_EQ(inflate(kDynamic + at, 40, buf.data(), out, g_work), size_t{0});
    CHECK_EQ(inflate(kStored + 10, 10, buf.data(), out, g_work), size_t{0});
    // Блок с постоянными кодами из 10000 литералов 'A' (код 8 разрядов,
    // старшим вперёд) и конца блока: обслуживание зовётся раз в 4096 символов.
    std::vector<uint8_t> packed;
    uint32_t acc = 0, nacc = 0;
    auto put = [&](uint32_t code, int bits, bool msb_first) {
        for (int i = 0; i < bits; i++) {
            acc |= ((code >> (msb_first ? bits - 1 - i : i)) & 1u) << nacc;
            if (++nacc == 8) {
                packed.push_back(static_cast<uint8_t>(acc));
                acc = nacc = 0;
            }
        }
    };
    put(3, 3, false); // последний блок, постоянные коды
    for (int i = 0; i < 10000; i++) {
        put(0x30 + 'A', 8, true);
    }
    put(0, 7, true);
    put(0, 7, false); // дополнить последний байт
    std::vector<uint8_t> plain(10000);
    int served = 0;
    CHECK_EQ(
        inflate(packed.data(), packed.size(), plain.data(), plain.size(), g_work, +[](void* user) { ++*static_cast<int*>(user); }, &served), size_t{10000});
    CHECK(plain.front() == 'A' && plain.back() == 'A');
    CHECK_EQ(served, 2);
    // Не gzip.
    CHECK(!gzip_parse(reinterpret_cast<const uint8_t*>("Vgm               "), 18, at, out));
}

} // namespace

void run_inflate_tests() {
    test_inflate();
}
