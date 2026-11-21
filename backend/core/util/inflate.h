// SPDX-License-Identifier: MIT
#pragma once

// Распаковка deflate (RFC 1951) из памяти в память и разбор обёртки gzip
// (RFC 1952). Окном служит сам выход: отдельных 32 КБ не нужно. Коды
// Хаффмана читаются по разряду - таблиц быстрого поиска нет, рабочая память
// - InflateWork, её даёт вызывающий.

#include <cstddef>
#include <cstdint>

namespace soundsinth::util {

// Символы двух кодов блока по порядку кодов и длины кодов при их чтении.
struct InflateWork {
    uint16_t lit_symbol[288];
    uint16_t dist_symbol[30];
    uint8_t lengths[288 + 30];
};

// Число байт выхода; 0 - поток испорчен, обрезан или не помещается в dst.
// service (может быть nullptr) зовётся раз в 4096 символов: распаковка
// мегабайт идёт секунды.
size_t inflate(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, InflateWork& work, void (*service)(void*) = nullptr, void* user = nullptr);

// Файл gzip: где начинается поток deflate и длина распакованного (из хвоста
// файла, по модулю 2^32). false - не gzip.
bool gzip_parse(const uint8_t* src, size_t len, size_t& data_at, uint32_t& out_len);

} // namespace soundsinth::util
