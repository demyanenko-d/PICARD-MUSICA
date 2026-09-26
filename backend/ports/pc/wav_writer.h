#pragma once

#include <cstdint>
#include <cstdio>

namespace platform_pc {

// Потоковая запись WAV: PCM 16 бит стерео; размеры чанков RIFF и data
// дописываются в close().
class WavWriter {
public:
    bool open(const char* path, uint32_t sample_rate_hz);
    void write_frames(const int16_t* interleaved, uint32_t frame_count);
    void close();

private:
    std::FILE* file_ = nullptr;
    uint32_t data_bytes_ = 0;
};

} // namespace platform_pc
