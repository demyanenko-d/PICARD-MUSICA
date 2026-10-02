// SPDX-License-Identifier: MIT
#pragma once

// ByteSource поверх FatFs, образ диска через platform_pc::mount_disk_image.
// Только ПК (тест test_fatfs_mod, mass_load_scan): прошивка читает трек по
// шине (BusByteSource), свою карту через FatFs не открывает. ff.h
// здесь не подключается: FIL спрятан за непрозрачным хранилищем, чтобы
// не тащить FatFs в каждый файл, объявляющий FatFsByteSource&.

#include <cstdint>

#include "core/formats/byte_source.h"

namespace soundsinth::formats {

class FatFsByteSource {
public:
    FatFsByteSource();
    ~FatFsByteSource();

    FatFsByteSource(const FatFsByteSource&)            = delete;
    FatFsByteSource& operator=(const FatFsByteSource&) = delete;

    // Открывает файл на уже смонтированном томе (монтирование -
    // mount_disk_image и f_mount - снаружи). true - успех.
    bool open(const char* path);
    void close();
    bool is_open() const;

    ByteSource as_byte_source();

private:
    static uint32_t read_fn(void* self, void* dst, uint32_t n);
    static bool seek_fn(void* self, uint32_t offset);
    static uint32_t size_fn(void* self);

    // FIL - 557+ байт в ff.h (зависит от опций сборки); в публичном
    // заголовке без ff.h хранится указатель на кучу, реальный sizeof(FIL)
    // знает только fatfs_byte_source.cpp. Открывается раз на файл, не на
    // горячем пути, new/delete здесь допустимы.
    void* file_;
};

} // namespace soundsinth::formats
