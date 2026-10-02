// SPDX-License-Identifier: MIT
#pragma once

// Источник байт файла. Структура с указателями на функции, а не виртуальный
// класс: без vtable и RTTI на горячем пути и в сборке МК без исключений.
// Загрузчики форматов читают файл только через него и не знают, что за ним.

#include <cstdint>

namespace soundsinth::formats {

struct ByteSource {
    void* self;
    uint32_t (*read)(void* self, void* dst, uint32_t n); // сколько прочитано
    bool (*seek)(void* self, uint32_t offset);           // false - не удалось или за пределами
    uint32_t (*size)(void* self);                        // общий размер, если известен
};

} // namespace soundsinth::formats
