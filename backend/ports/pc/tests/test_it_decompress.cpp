#include "testing.h"

#include <cstdio>

#include "core/formats/it_decompress.h"

using namespace soundsinth::formats::it;

namespace {

// Ручной, проверенный трассировкой алгоритма тест: три 8-битных сэмпла
// [5, -3, 10] закодированы напрямую в "режиме C" (ширина 9 бит, без
// смены ширины). Дельты (5, -8 как unsigned
// mod 256 = 248, 13) упакованы младшим битом вперёд в 4 байта:
// 0x05, 0xF0, 0x35, 0x00.
void test_mode_c_three_samples() {
    std::printf("test_it_decompress_mode_c_three_samples\n");

    const uint8_t bitstream[4] = {0x05, 0xF0, 0x35, 0x00};
    int16_t out[3] = {};
    DecompressState state{};
    const uint32_t written = decompress_step(state, bitstream, sizeof(bitstream), 3, out);

    CHECK_EQ(written, 3u);
    CHECK_EQ(out[0], static_cast<int16_t>(5));
    CHECK_EQ(out[1], static_cast<int16_t>(-3));
    CHECK_EQ(out[2], static_cast<int16_t>(10));
}

void test_truncated_stream_stops_early() {
    std::printf("test_it_decompress_truncated_stream_stops_early\n");

    const uint8_t bitstream[1] = {0x05}; // недостаточно бит на 3 сэмпла по 9 бит
    int16_t out[3] = {-1, -1, -1};
    DecompressState state{};
    const uint32_t written = decompress_step(state, bitstream, sizeof(bitstream), 3, out);

    // Байта не хватает и на первый отсчёт: поток оборван, отсчётов 0 (контракт
    // decompress_step - 0 значит "дальше звать не нужно").
    CHECK_EQ(written, 0u);
}

} // namespace

void run_it_decompress_tests() {
    test_mode_c_three_samples();
    test_truncated_stream_stops_early();
}
