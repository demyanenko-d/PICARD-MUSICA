// Порт ITDecompression (ITCompression.cpp OpenMPT) и BitReader.h;
// копирайт и потоковый API - в it_decompress.h.

#include "core/formats/it_decompress.h"

namespace soundsinth::formats::it {

namespace {

// Порт BitReader без владения источником: читает из переданного
// bitstream, позиция и буфер - в DecompressState, чтобы
// decompress_step можно было возобновлять.
uint32_t read_bits(DecompressState& s, const uint8_t* data, uint32_t size, int32_t num_bits) {
    while (s.bit_num < num_bits) {
        if (s.byte_pos >= size) {
            s.ok = false;
            return 0;
        }
        s.bit_buf |= (static_cast<uint32_t>(data[s.byte_pos++]) << s.bit_num);
        s.bit_num += 8;
    }
    const uint32_t v = s.bit_buf & ((1u << num_bits) - 1u);
    s.bit_buf >>= num_bits;
    s.bit_num -= num_bits;
    return v;
}

// Порт ITDecompression::ChangeWidth.
int32_t change_width(int32_t cur_width, int32_t width) {
    ++width;
    if (width >= cur_width) ++width;
    return width;
}

} // namespace

uint32_t decompress_step(DecompressState& state, const uint8_t* bitstream, uint32_t bitstream_bytes,
                         uint32_t max_samples, int16_t* out) {
    const bool is16bit = state.is16bit;
    const bool is215 = state.is215;
    const int32_t fetch_a = is16bit ? 4 : 3;
    const int32_t lower_b = is16bit ? -8 : -4;
    const int32_t upper_b = is16bit ? 7 : 3;
    const int32_t def_width = is16bit ? 17 : 9;

    if (state.width == 0) {
        // Свежий блок, как в ITDecompression::ITDecompression: mem1 = mem2 = 0
        // и стартовая ширина перед первым отсчётом блока.
        state.mem1 = 0;
        state.mem2 = 0;
        state.width = def_width;
    }

    uint32_t written = 0;

    auto write_sample = [&](int32_t v, int32_t top_bit) {
        if (v & top_bit) v -= (top_bit << 1); // знаковое расширение N-битного значения
        state.mem1 += v;
        state.mem2 += state.mem1;
        const int32_t result = is215 ? state.mem2 : state.mem1;
        if (is16bit) {
            out[written] = static_cast<int16_t>(static_cast<uint32_t>(result) & 0xFFFFu);
        } else {
            out[written] = static_cast<int8_t>(static_cast<uint32_t>(result) & 0xFFu); // сырой диапазон, без *256
        }
        ++written;
    };

    while (written < max_samples) {
        if (state.width > def_width) break; // ширина больше предельной - поток битый
        const int32_t v = static_cast<int32_t>(read_bits(state, bitstream, bitstream_bytes, state.width));
        if (!state.ok) break;
        const int32_t top_bit = 1 << (state.width - 1);

        if (state.width <= 6) {
            // Режим A: 1-6 бит
            if (v == top_bit) {
                const int32_t wv = static_cast<int32_t>(read_bits(state, bitstream, bitstream_bytes, fetch_a));
                if (!state.ok) break;
                state.width = change_width(state.width, wv);
            } else {
                write_sample(v, top_bit);
            }
        } else if (state.width < def_width) {
            // Режим B: ширина 7-8 (8 бит) или 7-16 (16 бит)
            if (v >= top_bit + lower_b && v <= top_bit + upper_b) {
                state.width = change_width(state.width, v - (top_bit + lower_b));
            } else {
                write_sample(v, top_bit);
            }
        } else {
            // Режим C: 9 / 17 бит
            if (v & top_bit) {
                state.width = (v & ~top_bit) + 1;
            } else {
                write_sample(v & ~top_bit, 0);
            }
        }
    }

    return written;
}

} // namespace soundsinth::formats::it
