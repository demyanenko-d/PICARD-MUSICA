#pragma once

// Загрузчик S3M: упакованные паттерны, сэмплы PCM 8/16 бит.

#include "core/model/song.h"
#include "core/formats/byte_source.h"
#include "core/memory/track_memory.h"

namespace soundsinth::formats::s3m {

// При false out непригоден, mem сбросить перед следующей попыткой; error_out
// - причина.
// metadata_only = true: разобрать заголовки, паттерны и дескрипторы, но
// не читать PCM сэмплов - их вытянет load_sample_pcm() по одному, в
// порядке воспроизведения. По умолчанию false: полная загрузка.
bool load(formats::ByteSource src, memory::TrackMemory& mem, soundsinth::model::Song& out, const char** error_out = nullptr,
           bool metadata_only = false);

// Распаковать PCM одного сэмпла в PSRAM и опубликовать в каталоге. Повторный
// вызов займёт вторую цепочку страниц; false - сэмпл остался нерезидентным,
// трек играет дальше.
bool load_sample_pcm(formats::ByteSource src, memory::TrackMemory& mem, const soundsinth::model::Song& song,
                      uint16_t sample_index, const char** reason_out = nullptr);
} // namespace soundsinth::formats::s3m
