// Адаптация libxmp src/loaders/itsex.c ("Public domain IT sample
// decompressor by Olivier Lapicque", доработан Alice Rowan 2023-2024).
// Арифметика (read_bits, unpacking-автомат в decompress8/16) - как в
// оригинале, дословно; заменён только слой ввода (HIO_HANDLE -> прямой
// буфер в памяти, см. .h).

#include "libxmp_itsex.h"

#include <cstring>
#include <vector>

namespace libxmp_itsex {

namespace {

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using int16 = int16_t;

#define READ_BITS_MASK(n) ((1u << (unsigned)(n)) - 1u)
#define MIN(x, y) ((x) < (y) ? (x) : (y))

struct it_stream {
    uint8* pos;
    size_t left;
    uint32 bits;
    int num_bits;
    int err;
};

inline uint32 read_bits(it_stream* in, int n) {
    uint32 retval = 0;

    if (n <= 0 || n >= 32) {
        in->err = -2;
        return 0;
    }

    retval = in->bits & READ_BITS_MASK(n);

    if (in->num_bits < n) {
        uint32 offset = in->num_bits;
        uint32 used;

        if (in->left == 0) {
            in->err = -1; // EOF
            return 0;
        }
        // Буфер должен быть дополнен нулями до кратности 4 байт.
        in->bits = in->pos[0] | (in->pos[1] << 8) | (in->pos[2] << 16) | (static_cast<uint32>(in->pos[3]) << 24);

        used = static_cast<uint32>(MIN(in->left, 4));

        in->num_bits = static_cast<int>(used * 8);
        in->pos += 4;
        in->left -= used;

        n -= static_cast<int>(offset);
        retval |= (in->bits & READ_BITS_MASK(n)) << offset;
    }

    in->bits >>= n;
    in->num_bits -= n;

    return retval;
}

// Отличие от оригинального init_block: длина блока (in->left) не
// читается отсюда (у нас её уже прочитал вызывающий код - bitstream_bytes
// это она и есть), только копирование во внутренний нулями дополненный
// буфер для read_bits (см. .h).
bool init_block(it_stream* in, std::vector<uint8_t>& tmp, const uint8_t* bitstream, uint32_t bitstream_bytes) {
    const uint32_t padded = (bitstream_bytes + 4 + 3) & ~3u; // +4 - запас, чтобы read_bits никогда не читал вне tmp
    tmp.assign(padded, 0);
    std::memcpy(tmp.data(), bitstream, bitstream_bytes);

    in->pos = tmp.data();
    in->left = bitstream_bytes;
    in->bits = 0;
    in->num_bits = 0;
    in->err = 0;
    return true;
}

} // namespace

int decompress8(const uint8_t* bitstream, uint32_t bitstream_bytes, int8_t* dst_signed, int len, int it215) {
    auto* dst = reinterpret_cast<uint8*>(dst_signed);
    it_stream in;
    uint32 block_count = 0;
    uint8 left = 0, temp = 0, temp2 = 0;
    uint32 d, pos;
    std::vector<uint8_t> tmp;

    std::memset(&in, 0, sizeof(in));

    while (len) {
        if (!block_count) {
            block_count = 0x8000;
            left = 9;
            temp = temp2 = 0;
            if (!init_block(&in, tmp, bitstream, bitstream_bytes)) return -1;
        }

        d = block_count;
        if (d > static_cast<uint32>(len)) d = static_cast<uint32>(len);

        pos = 0;
        do {
            uint16 bits = static_cast<uint16>(read_bits(&in, left));
            if (in.err) return -1;

            if (left < 7) {
                uint32 i = 1u << (left - 1);
                uint32 j = bits & 0xffffu;
                if (i != j) goto unpack_byte;
                bits = static_cast<uint16>((read_bits(&in, 3) + 1) & 0xff);
                if (in.err) return -1;
                left = (static_cast<uint8>(bits) < left) ? static_cast<uint8>(bits) : static_cast<uint8>((bits + 1) & 0xff);
                goto next;
            }

            if (left < 9) {
                uint16 i = static_cast<uint16>((0xff >> (9 - left)) + 4);
                uint16 j = static_cast<uint16>(i - 8);
                if ((bits <= j) || (bits > i)) goto unpack_byte;
                bits = static_cast<uint16>(bits - j);
                left = (static_cast<uint8>(bits & 0xff) < left) ? static_cast<uint8>(bits & 0xff)
                                                                  : static_cast<uint8>((bits + 1) & 0xff);
                goto next;
            }

            if (left >= 10) goto skip_byte;

            if (bits >= 256) {
                left = static_cast<uint8>((bits + 1) & 0xff);
                goto next;
            }

        unpack_byte:
            if (left < 8) {
                uint8 shift = static_cast<uint8>(8 - left);
                signed char c = static_cast<signed char>(bits << shift);
                c = static_cast<signed char>(c >> shift);
                bits = static_cast<uint16>(c);
            }
            bits = static_cast<uint16>(bits + temp);
            temp = static_cast<uint8>(bits);
            temp2 = static_cast<uint8>(temp2 + temp);
            dst[pos] = it215 ? temp2 : temp;

        skip_byte:
            pos++;

        next:;
        } while (pos < d);

        block_count -= d;
        len -= static_cast<int>(d);
        dst += d;
    }

    return 0;
}

int decompress16(const uint8_t* bitstream, uint32_t bitstream_bytes, int16_t* dst, int len, int it215) {
    it_stream in;
    uint32 block_count = 0;
    uint8 left = 0;
    int16 temp = 0, temp2 = 0;
    uint32 d, pos;
    std::vector<uint8_t> tmp;

    std::memset(&in, 0, sizeof(in));

    while (len) {
        if (!block_count) {
            block_count = 0x4000;
            left = 17;
            temp = temp2 = 0;
            if (!init_block(&in, tmp, bitstream, bitstream_bytes)) return -1;
        }

        d = block_count;
        if (d > static_cast<uint32>(len)) d = static_cast<uint32>(len);

        pos = 0;
        do {
            uint32 bits = read_bits(&in, left);
            if (in.err) return -1;

            if (left < 7) {
                uint32 i = 1u << (left - 1);
                uint32 j = bits;
                if (i != j) goto unpack_byte;
                bits = read_bits(&in, 4) + 1;
                if (in.err) return -1;
                left = (static_cast<uint8>(bits & 0xff) < left) ? static_cast<uint8>(bits & 0xff)
                                                                  : static_cast<uint8>((bits + 1) & 0xff);
                goto next;
            }

            if (left < 17) {
                uint32 i = (0xffffu >> (17 - left)) + 8;
                uint32 j = (i - 16) & 0xffffu;
                if ((bits <= j) || (bits > (i & 0xffffu))) goto unpack_byte;
                bits -= j;
                left = (static_cast<uint8>(bits & 0xff) < left) ? static_cast<uint8>(bits & 0xff)
                                                                  : static_cast<uint8>((bits + 1) & 0xff);
                goto next;
            }

            if (left >= 18) goto skip_byte;

            if (bits >= 0x10000u) {
                left = static_cast<uint8>((bits + 1) & 0xff);
                goto next;
            }

        unpack_byte:
            if (left < 16) {
                uint8 shift = static_cast<uint8>(16 - left);
                int16 c = static_cast<int16>(bits << shift);
                c = static_cast<int16>(c >> shift);
                bits = static_cast<uint32>(c);
            }
            bits = static_cast<uint32>(bits + temp);
            temp = static_cast<int16>(bits);
            temp2 = static_cast<int16>(temp2 + temp);
            dst[pos] = it215 ? temp2 : temp;

        skip_byte:
            pos++;

        next:;
        } while (pos < d);

        block_count -= d;
        len -= static_cast<int>(d);
        dst += d;
        if (len <= 0) break;
    }

    return 0;
}

#undef READ_BITS_MASK
#undef MIN

} // namespace libxmp_itsex
