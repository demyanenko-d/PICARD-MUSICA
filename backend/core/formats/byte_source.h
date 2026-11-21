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
    // Байты [offset, offset + n) уже лежат в PSRAM страницами подряд с начала
    // страницы: номер первой. 0xffff (memory::kPsramNoPage) - нет или
    // источник так не умеет. Умеет только приём модуля GS.
    uint16_t (*pages_at)(void* self, uint32_t offset, uint32_t n) = nullptr;
};

} // namespace soundsinth::formats
