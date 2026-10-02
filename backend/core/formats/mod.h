// SPDX-License-Identifier: MIT
#pragma once

// Загрузчик MOD: formats::ByteSource -> резидентные метаданные Song ->
// сжатые паттерны в PSRAM (PatternPacker) -> сэмплы в Raw8-страницах
// PSRAM (SamplePacker; MOD-сэмплы всегда 8 бит).

#include "core/model/song.h"
#include "core/formats/byte_source.h"
#include "core/memory/track_memory.h"

namespace soundsinth::formats::mod {

// src - файл целиком, уже открытый вызывающим (MemoryByteSource в
// тестах и pc_player, FatFsByteSource ПК, шина на плате). mem - инициализированная
// (track_memory_create) и, если загрузка не первая, сброшенная
// (track_memory_reset_for_new_track) память. out - результат; при false
// out заполнен частично и непригоден, вызывающий обязан сбросить mem
// перед следующей попыткой. error_out (если не nullptr) - статическая
// строка с причиной отказа, для диагностики.
// metadata_only = true: разобрать заголовки, паттерны и дескрипторы, но
// не читать PCM сэмплов - их вытянет load_sample_pcm() по одному, в
// порядке воспроизведения. По умолчанию false: полная загрузка.
bool load(formats::ByteSource src, memory::TrackMemory& mem, soundsinth::model::Song& out, const char** error_out = nullptr, bool metadata_only = false);

// Распаковать PCM одного сэмпла в PSRAM и опубликовать в каталоге. Повторный
// вызов займёт вторую цепочку страниц; false - сэмпл остался нерезидентным,
// трек играет дальше.
bool load_sample_pcm(formats::ByteSource src, memory::TrackMemory& mem, const soundsinth::model::Song& song, uint16_t sample_index,
                     const char** reason_out = nullptr);
} // namespace soundsinth::formats::mod
