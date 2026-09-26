#include "testing.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "song_compare.h"
#include "core/formats/it.h"
#include "core/formats/it_decompress.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "third_party/libxmp_itsex.h"

// Golden-тест: сверка нашего порта IT-декомпрессии
// (formats/it_decompress.h, портирован из OpenMPT
// soundlib/ITCompression.cpp) с независимой второй реализацией того же
// формата - адаптированным libxmp src/loaders/itsex.c (public domain, см.
// third_party/libxmp_itsex.h). Совпадение результата двух разных
// кодовых баз на реальных сжатых данных из файла - намного более сильное
// доказательство корректности арифметики порта, чем сверка с самим собой.

namespace {

uint16_t read_u16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t read_u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

struct FoundBlock {
    std::vector<uint8_t> file;
    uint32_t block_data_offset = 0; // после 2-байтного префикса длины
    uint16_t compressed_size = 0;
    uint32_t sample_count = 0; // сколько сэмплов в этом (первом) блоке
    bool is16bit = false;
    bool is215 = false;
};

// Минимальный самостоятельный разбор IT-заголовка, только чтобы найти
// первый сжатый сэмпл и его первый блок. Точные смещения полей - в
// formats/it.cpp; здесь они повторно не сверяются, их проверяют тесты
// загрузчика IT.
bool find_first_compressed_sample(const char* path, FoundBlock& out, bool want_16bit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 192 || std::memcmp(file.data(), "IMPM", 4) != 0) return false;

    const uint16_t order_count = read_u16le(&file[32]);
    const uint16_t instrument_count = read_u16le(&file[34]);
    const uint16_t sample_count = read_u16le(&file[36]);
    const uint16_t cmwt = read_u16le(&file[42]);
    if (cmwt < 0x200) return false;

    const uint32_t sample_ptrs_offset = 192u + order_count + static_cast<uint32_t>(instrument_count) * 4u;
    if (sample_ptrs_offset + static_cast<uint32_t>(sample_count) * 4u > file.size()) return false;

    for (uint32_t i = 0; i < sample_count; ++i) {
        const uint32_t ptr = read_u32le(&file[sample_ptrs_offset + i * 4]);
        if (ptr == 0 || ptr + 80 > file.size() || std::memcmp(&file[ptr], "IMPS", 4) != 0) continue;

        const uint8_t flags = file[ptr + 18];
        const bool is_compressed = (flags & 0x08) != 0;
        if (!is_compressed) continue;

        const bool is16bit = (flags & 0x02) != 0;
        if (is16bit != want_16bit) continue;
        const uint32_t length_samples = read_u32le(&file[ptr + 48]);
        const uint32_t sample_pointer = read_u32le(&file[ptr + 72]);
        if (length_samples == 0 || sample_pointer + 2 > file.size()) continue;

        const uint16_t compressed_size = read_u16le(&file[sample_pointer]);
        const uint32_t block_data_offset = sample_pointer + 2;
        if (compressed_size == 0 || block_data_offset + compressed_size > file.size()) continue;

        const uint32_t block_capacity = is16bit ? 16384u : 32768u;
        out.is215 = (file[ptr + 46] & 0x04) != 0; // бит 2 cvt сэмпла, как у загрузчика; до move
        out.file = std::move(file);
        out.block_data_offset = block_data_offset;
        out.compressed_size = compressed_size;
        out.sample_count = (length_samples < block_capacity) ? length_samples : block_capacity;
        out.is16bit = is16bit;
        return true;
    }
    return false;
}

static const char* kCandidates[] = {
    "SD/test_music/it/bombls16.it", "SD/test_music/it/bz_ult9.it", "SD/test_music/it/00009.it",       "SD/test_music/it/0700.it",
    "SD/test_music/it/00013.it",    "SD/test_music/it/038djzjack_littlerock.it", "SD/test_music/it/00012 ladda upp denna.it",
};

void run_one_bit_depth(bool want_16bit) {
    FoundBlock block;
    bool found = false;
    for (const char* path : kCandidates) {
        if (find_first_compressed_sample(path, block, want_16bit)) {
            std::printf("  используется %s (%s, %u сэмплов в первом блоке, is215=%d)\n", path,
                        block.is16bit ? "16-бит" : "8-бит", block.sample_count, block.is215 ? 1 : 0);
            found = true;
            break;
        }
    }
    if (!found) {
        std::printf("  ни в одном файле корпуса не нашлось %s сжатого сэмпла — ПРОПУСК этой разрядности\n",
                    want_16bit ? "16-битного" : "8-битного");
        return;
    }

    const uint8_t* bitstream = block.file.data() + block.block_data_offset;

    if (block.is16bit) {
        std::vector<int16_t> ours(block.sample_count);
        soundsinth::formats::it::DecompressState state{};
        state.is16bit = true;
        state.is215 = block.is215;
        const uint32_t got = soundsinth::formats::it::decompress_step(state, bitstream, block.compressed_size,
                                                                        block.sample_count, ours.data());
        CHECK_EQ(got, block.sample_count);

        std::vector<int16_t> theirs(block.sample_count);
        const int rc = libxmp_itsex::decompress16(bitstream, block.compressed_size, theirs.data(),
                                                    static_cast<int>(block.sample_count), block.is215 ? 1 : 0);
        CHECK_EQ(rc, 0);

        for (uint32_t i = 0; i < block.sample_count; ++i) {
            CHECK_EQ(ours[i], theirs[i]);
        }
    } else {
        std::vector<int16_t> ours(block.sample_count);
        soundsinth::formats::it::DecompressState state{};
        state.is215 = block.is215;
        const uint32_t got = soundsinth::formats::it::decompress_step(state, bitstream, block.compressed_size,
                                                                        block.sample_count, ours.data());
        CHECK_EQ(got, block.sample_count);

        std::vector<int8_t> theirs(block.sample_count);
        const int rc = libxmp_itsex::decompress8(bitstream, block.compressed_size, theirs.data(),
                                                   static_cast<int>(block.sample_count), block.is215 ? 1 : 0);
        CHECK_EQ(rc, 0);

        for (uint32_t i = 0; i < block.sample_count; ++i) {
            // Наш decompress_step для 8 бит тоже отдаёт сырой диапазон (-128..127)
            // в int16_t-контейнере, см. formats/it_decompress.h.
            CHECK_EQ(ours[i], static_cast<int16_t>(theirs[i]));
        }
    }
}

// Все блоки всех сжатых сэмплов файла: наш декодер целиком и кусками по 1, 7
// и kStepSamples отсчётов с одним состоянием (так зовёт загрузчик) против
// libxmp. found[16 бит][2.15] - какие сочетания встретились; worst_step -
// наибольший расход потока за шаг kStepSamples.
void check_all_blocks(const char* path, bool found[2][2], uint32_t& blocks, uint32_t& bad, uint32_t& worst_step) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("  ПРОПУСК (нет файла): %s\n", path);
        return;
    }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 192 || std::memcmp(file.data(), "IMPM", 4) != 0) return;
    const uint16_t order_count = read_u16le(&file[32]);
    const uint16_t instrument_count = read_u16le(&file[34]);
    const uint16_t sample_count = read_u16le(&file[36]);
    const uint32_t sample_ptrs = 192u + order_count + static_cast<uint32_t>(instrument_count) * 4u;
    for (uint32_t i = 0; i < sample_count; ++i) {
        if (sample_ptrs + i * 4 + 4 > file.size()) break;
        const uint32_t ptr = read_u32le(&file[sample_ptrs + i * 4]);
        if (ptr == 0 || ptr + 80 > file.size() || std::memcmp(&file[ptr], "IMPS", 4) != 0) continue;
        const uint8_t flags = file[ptr + 18];
        if ((flags & 0x08) == 0) continue; // не сжат
        const bool is16 = (flags & 0x02) != 0;
        const bool is215 = (file[ptr + 46] & 0x04) != 0;
        uint32_t left = read_u32le(&file[ptr + 48]);
        uint32_t pos = read_u32le(&file[ptr + 72]);
        const uint32_t capacity = is16 ? 16384u : 32768u;
        while (left > 0 && pos + 2 <= file.size()) {
            const uint16_t size = read_u16le(&file[pos]);
            pos += 2;
            if (size == 0 || pos + size > file.size()) break;
            const uint8_t* bits = file.data() + pos;
            const uint32_t n = left < capacity ? left : capacity;
            std::vector<int16_t> theirs(n);
            int rc;
            if (is16) {
                rc = libxmp_itsex::decompress16(bits, size, theirs.data(), static_cast<int>(n), is215 ? 1 : 0);
            } else {
                std::vector<int8_t> t8(n);
                rc = libxmp_itsex::decompress8(bits, size, t8.data(), static_cast<int>(n), is215 ? 1 : 0);
                for (uint32_t k = 0; k < n; ++k) theirs[k] = t8[k];
            }
            if (rc == 0) {
                found[is16][is215] = true;
                ++blocks;
                for (uint32_t chunk : {n, 1u, 7u, soundsinth::formats::it::kStepSamples}) {
                    std::vector<int16_t> ours(n);
                    soundsinth::formats::it::DecompressState state{};
                    state.is16bit = is16;
                    state.is215 = is215;
                    uint32_t done = 0;
                    while (done < n) {
                        const uint32_t want = n - done < chunk ? n - done : chunk;
                        const uint32_t pos_before = state.byte_pos;
                        const uint32_t got = soundsinth::formats::it::decompress_step(state, bits, size, want, ours.data() + done);
                        if (chunk == soundsinth::formats::it::kStepSamples && state.byte_pos - pos_before > worst_step) {
                            worst_step = state.byte_pos - pos_before;
                        }
                        if (got == 0) break;
                        done += got;
                    }
                    if (done != n || ours != theirs) {
                        if (++bad <= 5) {
                            std::printf("  %s сэмпл %u: куском %u - %u из %u, %s\n", path, i, chunk, done, n,
                                        ours == theirs ? "значения те же" : "значения разошлись");
                        }
                    }
                }
            }
            pos += size;
            left -= n;
        }
    }
}

void test_all_blocks_whole_and_chunked() {
    std::printf("test_it_golden_all_blocks_whole_and_chunked\n");
    static const char* kFiles[] = {
        "SD/test_music/it/bombls16.it", "SD/test_music/it/00009.it", "SD/test_music/it/0700.it",
        "SD/test_music/it/00013.it", "SD/test_music/it/00012 ladda upp denna.it",
        "music/src/it/bmtest/lady__v61.it", "music/src/it/smoke/poly_ge-chin.it",
    };
    bool found[2][2] = {};
    uint32_t blocks = 0, bad = 0, worst_step = 0;
    for (const char* path : kFiles) check_all_blocks(path, found, blocks, bad, worst_step);
    std::printf("  блоков %u, расхождений %u; 8 бит %d/%d, 16 бит %d/%d (обычный/2.15); шаг до %u байт\n", blocks, bad,
                found[0][0], found[0][1], found[1][0], found[1][1], worst_step);
    CHECK(blocks > 0);
    CHECK_EQ(bad, 0u);
    CHECK(worst_step <= soundsinth::formats::it::kStepWorstBytes);
    CHECK(found[0][0] && found[1][0]);
    // 2.15 есть только в music/src - без него сочетание пропускается.
    std::ifstream probe("music/src/it/smoke/poly_ge-chin.it");
    if (probe) {
        CHECK(found[0][1] && found[1][1]);
    } else {
        std::printf("  ПРОПУСК: нет файлов с вариантом 2.15\n");
    }
}

// Путь загрузчика: it::load кладёт сжатые сэмплы в PSRAM через окно файла,
// подливку, переход к следующему блоку и расширение знака. Байты PSRAM
// каждого сжатого сэмпла Raw8/Raw16 без прореживания и разворота против
// libxmp по всем блокам подряд; Dpcm8 пропускается.
void test_loader_matches_libxmp() {
    std::printf("test_it_golden_loader_matches_libxmp\n");
    static const char* kFiles[] = {
        "SD/test_music/it/bombls16.it", "SD/test_music/it/00009.it", "SD/test_music/it/0700.it",
        "music/src/it/bmtest/lady__v61.it", "music/src/it/smoke/poly_ge-chin.it",
    };
    uint32_t compared = 0, skipped = 0, bad = 0;
    for (const char* path : kFiles) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::printf("  ПРОПУСК (нет файла): %s\n", path);
            continue;
        }
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        soundsinth::formats::MemoryByteSource src(file.data(), static_cast<uint32_t>(file.size()));
        soundsinth::memory::TrackMemory mem;
        soundsinth::memory::track_memory_create(mem);
        soundsinth::model::Song song;
        CHECK(soundsinth::formats::it::load(src.as_byte_source(), mem, song, nullptr));
        const uint16_t order_count = read_u16le(&file[32]);
        const uint16_t instrument_count = read_u16le(&file[34]);
        const uint16_t sample_count = read_u16le(&file[36]);
        const uint32_t sample_ptrs = 192u + order_count + static_cast<uint32_t>(instrument_count) * 4u;
        for (uint32_t i = 0; i < sample_count && i < song.sample_count; ++i) {
            const uint32_t ptr = read_u32le(&file[sample_ptrs + i * 4]);
            if (ptr == 0 || ptr + 80 > file.size() || std::memcmp(&file[ptr], "IMPS", 4) != 0) continue;
            const uint8_t flags = file[ptr + 18];
            if ((flags & 0x08) == 0 || (flags & 0x04) != 0) continue; // не сжат или стерео
            const bool is16 = (flags & 0x02) != 0;
            const bool is215 = (file[ptr + 46] & 0x04) != 0;
            const uint32_t length = read_u32le(&file[ptr + 48]);
            const soundsinth::model::SampleDescriptor& sd = song.samples[i];
            if (sd.resident_encoding == soundsinth::model::ResidentEncoding::Dpcm8 || sd.length_samples != length) {
                ++skipped;
                continue;
            }
            std::vector<int16_t> theirs;
            uint32_t left = length;
            uint32_t pos = read_u32le(&file[ptr + 72]);
            const uint32_t capacity = is16 ? 16384u : 32768u;
            while (left > 0 && pos + 2 <= file.size()) {
                const uint16_t size = read_u16le(&file[pos]);
                pos += 2;
                if (size == 0 || pos + size > file.size()) break;
                const uint32_t n = left < capacity ? left : capacity;
                std::vector<int16_t> block(n);
                if (is16) {
                    libxmp_itsex::decompress16(file.data() + pos, size, block.data(), static_cast<int>(n), is215 ? 1 : 0);
                } else {
                    std::vector<int8_t> b8(n);
                    libxmp_itsex::decompress8(file.data() + pos, size, b8.data(), static_cast<int>(n), is215 ? 1 : 0);
                    for (uint32_t k = 0; k < n; ++k) block[k] = b8[k];
                }
                theirs.insert(theirs.end(), block.begin(), block.end());
                pos += size;
                left -= n;
            }
            if (theirs.size() != length) {
                ++skipped;
                continue;
            }
            const std::vector<uint8_t> ours = song_compare::collect_sample_bytes(mem.psram, mem.sample_cache, static_cast<uint16_t>(i), sd);
            const bool raw16 = sd.resident_encoding == soundsinth::model::ResidentEncoding::Raw16;
            bool same = ours.size() == length * (raw16 ? 2u : 1u);
            for (uint32_t k = 0; same && k < length; ++k) {
                const int16_t v = raw16 ? static_cast<int16_t>(ours[2 * k] | (ours[2 * k + 1] << 8))
                                        : static_cast<int16_t>(static_cast<int8_t>(ours[k]));
                same = v == theirs[k];
            }
            ++compared;
            if (!same && ++bad <= 5) std::printf("  %s сэмпл %u: PSRAM расходится с libxmp\n", path, i);
        }
        soundsinth::memory::track_memory_destroy(mem);
    }
    std::printf("  сжатых сэмплов сверено %u, пропущено %u (Dpcm8, прореженные, развёрнутые), расхождений %u\n",
                compared, skipped, bad);
    CHECK(compared > 0);
    CHECK_EQ(bad, 0u);
}

} // namespace

void run_it_golden_tests() {
    std::printf("test_it_golden_matches_independent_libxmp_decoder\n");
    run_one_bit_depth(true);
    run_one_bit_depth(false);
    test_all_blocks_whole_and_chunked();
    test_loader_matches_libxmp();
}
