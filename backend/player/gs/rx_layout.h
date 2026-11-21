// SPDX-License-Identifier: MIT
#pragma once

// Раскладка приёма GS. Четырёхканальный MOD на 31 сэмпл кладётся так, что
// каждый сэмпл начинается с начала страницы PSRAM: голос играет его прямо
// из приёма, без копии (ByteSource::pages_at). Заголовок и паттерны - как в
// файле, подряд: их отдаёт резиденту устройство. Решается по первым
// kHeaderBytes байтам; до решения и без раскладки смещение в приёме равно
// смещению в файле. Число паттернов - максимум таблицы порядка плюс один;
// где загрузчик решит иначе, сэмпл не совпадёт с отрезком и будет
// скопирован, звук от этого не меняется.

#include <cstdint>

#include "core/memory/psram_alloc.h"

#include "devices/gs/gs_device.h"

namespace player::gs {

class RxLayout {
public:
    static constexpr uint32_t kSamples     = 31;
    static constexpr uint32_t kHeaderBytes = devices::gs::kModPatternsAt;
    // Добивка до страниц: по странице на сэмпл и на заголовок. Приём - файл
    // и добивка: потолок файла раскладка не уменьшает.
    static constexpr uint32_t kPadBytes = (kSamples + 1u) * soundsinth::memory::kPsramPageBytes;

    // Новый поток: раскладка ещё не выбрана.
    void reset() {
        decided_ = false;
        on_      = false;
    }
    bool decided() const { return decided_; }
    // Первые kHeaderBytes байт уже в приёме: выбрать раскладку. mod - поток
    // может быть модулем (у файла .mid раскладки нет).
    void decide(const uint8_t* p, bool mod);
    // Смещение в приёме для байта файла at; room - сколько байт подряд за ним
    // лежит в том же отрезке. sample - номер сэмпла с этим байтом, иначе
    // kSamples.
    uint32_t place(uint32_t at, uint32_t* room, uint32_t* sample = nullptr) const;
    // Длина сэмпла из заголовка, байт.
    uint32_t sample_bytes(uint32_t sample) const { return static_cast<uint32_t>(words_[sample]) * 2u; }
    // Сколько от начала приёма лежит подряд как в файле: заголовок и
    // паттерны при раскладке, иначе всё принятое.
    uint32_t image_len(uint32_t received) const { return on_ && head_ < received ? head_ : received; }

private:
    bool decided_             = false;
    bool on_                  = false;
    uint32_t head_            = 0;  // заголовок и паттерны, байт
    uint16_t words_[kSamples] = {}; // длины сэмплов из заголовка, слов
};

} // namespace player::gs
