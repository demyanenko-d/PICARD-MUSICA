#pragma once

// Тонкая обёртка над formats::ByteSource: число и порядок вызовов
// read()/seek() не зависят от подставленного ByteSource. Little-endian
// (все трекерные форматы little-endian).
//
// Своей буферизации нет: она нужна источникам с дорогим мелким вызовом
// (BusByteSource) и живёт внутри них.

#include <cstdint>
#include <cstring>

#include "core/formats/byte_source.h"

namespace soundsinth::formats {

class BinaryReader {
public:
    explicit BinaryReader(ByteSource src) : src_(src) {}

    uint8_t u8() {
        uint8_t v = 0;
        read_raw(&v, 1);
        return v;
    }
    int8_t i8() { return static_cast<int8_t>(u8()); }

    uint16_t u16() {
        uint8_t b[2];
        read_raw(b, 2);
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    }
    int16_t i16() { return static_cast<int16_t>(u16()); }

    uint32_t u32() {
        uint8_t b[4];
        read_raw(b, 4);
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    }

    // Читает n байт как есть (имена, сырые блоки под распаковку).
    void bytes(void* dst, uint32_t n) { read_raw(dst, n); }

    void skip(uint32_t n) {
        // Через seek(tell()+n), а не n холостых read(): на FatFs и шине
        // это дешевле (f_lseek против побайтного f_read).
        seek(pos_ + n);
    }

    bool seek(uint32_t pos) {
        if (!src_.seek(src_.self, pos)) {
            ok_ = false;
            return false;
        }
        pos_ = pos;
        return true;
    }

    uint32_t tell() const { return pos_; }
    uint32_t size() const { return src_.size(src_.self); }

    // Липкий флаг: становится false при первом недочтении или неудачном
    // seek и больше не сбрасывается. Загрузчику достаточно одной проверки
    // ok() после блока чтений.
    bool ok() const { return ok_; }

private:
    void read_raw(void* dst, uint32_t n) {
        const uint32_t got = src_.read(src_.self, dst, n);
        if (got != n) {
            std::memset(static_cast<uint8_t*>(dst) + got, 0, n - got);
            ok_ = false;
        }
        pos_ += got;
    }

    ByteSource src_;
    uint32_t pos_ = 0;
    bool ok_ = true;
};

} // namespace soundsinth::formats
