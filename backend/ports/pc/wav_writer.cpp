// SPDX-License-Identifier: MIT
#include "pc/wav_writer.h"

namespace platform_pc {
namespace {

void write_u32le(std::FILE* f, uint32_t v) {
    std::fwrite(&v, 4, 1, f);
}

void write_u16le(std::FILE* f, uint16_t v) {
    std::fwrite(&v, 2, 1, f);
}

} // namespace

bool WavWriter::open(const char* path, uint32_t sample_rate_hz) {
    file_ = std::fopen(path, "wb");
    if (!file_) {
        return false;
    }

    const uint16_t channels        = 2;
    const uint16_t bits_per_sample = 16;
    const uint32_t byte_rate       = sample_rate_hz * channels * (bits_per_sample / 8);
    const uint16_t block_align     = static_cast<uint16_t>(channels * (bits_per_sample / 8));

    std::fwrite("RIFF", 1, 4, file_);
    write_u32le(file_, 0); // патчим в close()
    std::fwrite("WAVE", 1, 4, file_);

    std::fwrite("fmt ", 1, 4, file_);
    write_u32le(file_, 16);
    write_u16le(file_, 1); // PCM
    write_u16le(file_, channels);
    write_u32le(file_, sample_rate_hz);
    write_u32le(file_, byte_rate);
    write_u16le(file_, block_align);
    write_u16le(file_, bits_per_sample);

    std::fwrite("data", 1, 4, file_);
    write_u32le(file_, 0); // патчим в close()

    data_bytes_ = 0;
    return true;
}

void WavWriter::write_frames(const int16_t* interleaved, uint32_t frame_count) {
    const uint32_t bytes = frame_count * 2 * static_cast<uint32_t>(sizeof(int16_t));
    std::fwrite(interleaved, 1, bytes, file_);
    data_bytes_ += bytes;
}

void WavWriter::close() {
    if (!file_) {
        return;
    }
    const uint32_t riff_size = 36 + data_bytes_;
    std::fseek(file_, 4, SEEK_SET);
    write_u32le(file_, riff_size);
    std::fseek(file_, 40, SEEK_SET);
    write_u32le(file_, data_bytes_);
    std::fclose(file_);
    file_ = nullptr;
}

} // namespace platform_pc
