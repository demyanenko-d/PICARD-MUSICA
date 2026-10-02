// SPDX-License-Identifier: MIT
#pragma once

// Загрузчик XM: огибающие, keymap, relative_note, дельта-кодированные
// сэмплы (не сжатие, но декодирование последовательное, с накоплением).
//
// Только раскладка версии >= 0x0104 (почти все реальные файлы). Старые
// 1.02/1.03 (8-байтный заголовок паттерна, инструменты до паттернов)
// отклоняются с ошибкой.

#include "core/model/song.h"
#include "core/formats/byte_source.h"
#include "core/memory/track_memory.h"

namespace soundsinth::formats::xm {

// false - out непригоден, mem сбросить перед следующей попыткой; error_out -
// причина. metadata_only - без PCM: сэмплы тянет load_sample_pcm() по одному.
bool load(formats::ByteSource src, memory::TrackMemory& mem, soundsinth::model::Song& out, const char** error_out = nullptr, bool metadata_only = false);

// Распаковать PCM одного сэмпла в PSRAM и опубликовать в каталоге. Повторный
// вызов займёт вторую цепочку страниц. false - сэмпл нерезидентен
// (ModPlug-ADPCM, файл обрезан, нет места), трек играет дальше; reason_out -
// причина.
bool load_sample_pcm(formats::ByteSource src, memory::TrackMemory& mem, const soundsinth::model::Song& song, uint16_t sample_index,
                     const char** reason_out = nullptr);

} // namespace soundsinth::formats::xm
