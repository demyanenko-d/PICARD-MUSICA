#pragma once

// Загрузчик IT: поля инструмента NNA/DCT/DCA, биты "повтори предыдущее
// значение" на канал в упаковке паттерна, блочное битовое сжатие сэмплов
// (it_decompress.h, порт ITDecompression из OpenMPT).
//
// Старый формат инструмента (cmwt < 0x200, ITOldInstrument) тоже
// поддерживается: другая раскладка полей, одна огибающая громкости вместо
// трёх.

#include "core/model/song.h"
#include "core/formats/byte_source.h"
#include "core/memory/track_memory.h"

namespace soundsinth::formats::it {

// false - out непригоден, mem сбросить перед следующей попыткой; error_out -
// причина. metadata_only - без PCM: сэмплы тянет load_sample_pcm() по одному.
bool load(formats::ByteSource src, memory::TrackMemory& mem, soundsinth::model::Song& out, const char** error_out = nullptr,
           bool metadata_only = false);

// Распаковать PCM одного сэмпла в PSRAM и опубликовать; song и src - от
// этого же загрузчика. Повторный вызов займёт вторую цепочку страниц. false
// - сэмпл нерезидентен (ModPlug-ADPCM, файл обрезан, битый сжатый блок, нет
// места), трек играет дальше; reason_out - причина.
bool load_sample_pcm(formats::ByteSource src, memory::TrackMemory& mem, const soundsinth::model::Song& song,
                      uint16_t sample_index, const char** reason_out = nullptr);

} // namespace soundsinth::formats::it
