// SPDX-License-Identifier: MIT
#pragma once

// ByteSource поверх буфера в памяти, файл уже прочитан целиком. Для
// юнит-тестов (без диска и хоста) и для случая "хост отдал файл
// целиком".

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "core/formats/byte_source.h"

namespace soundsinth::formats {

class MemoryByteSource {
public:
    MemoryByteSource(const void* data, uint32_t size) : data_(static_cast<const uint8_t*>(data)), size_(size) {}

    ByteSource as_byte_source() { return ByteSource{this, &read_fn, &seek_fn, &size_fn}; }

private:
    static uint32_t read_fn(void* self, void* dst, uint32_t n) {
        auto* src              = static_cast<MemoryByteSource*>(self);
        const uint32_t avail   = src->size_ - src->pos_;
        const uint32_t to_copy = std::min(n, avail);
        std::memcpy(dst, src->data_ + src->pos_, to_copy);
        src->pos_ += to_copy;
        return to_copy;
    }
    static bool seek_fn(void* self, uint32_t offset) {
        auto* src = static_cast<MemoryByteSource*>(self);
        if (offset > src->size_) {
            return false;
        }
        src->pos_ = offset;
        return true;
    }
    static uint32_t size_fn(void* self) { return static_cast<MemoryByteSource*>(self)->size_; }

    const uint8_t* data_;
    uint32_t size_;
    uint32_t pos_ = 0;
};

} // namespace soundsinth::formats
