#pragma once

// Несжатый PCM сэмпла из файла в резидентные страницы PSRAM - общий путь
// MOD, S3M, XM и несжатых сэмплов IT.

#include <cstdint>

#include "core/model/instrument.h"

namespace soundsinth::formats {
class BinaryReader;
}
namespace soundsinth::memory {
struct TrackMemory;
}

namespace soundsinth::model {

// Буфер под причину с числами: reason_out - const char*, а текст
// собирается на лету. Писатель один - загрузчик (на плате Core1, на ПК
// однопоточно).
inline constexpr uint32_t kLoadReasonBytes = 192;
char* load_reason_buffer();

// Чтение с sd.file_offset кусками по 4096 отсчётов, перевод по sd.encoding
// (Pcm8, Pcm16 - по sd.signed_pcm; XmDelta8, XmDelta16 - сумма дельт с
// нуля), упаковка по дескриптору, публикация в каталоге. Из файла читается
// sd.source_length_samples. false - сэмпл нерезидентен, причина в
// reason_out (может быть nullptr).
bool pack_file_pcm(memory::TrackMemory& mem, formats::BinaryReader& r, const SampleDescriptor& sd, uint16_t sample_index,
                   const char** reason_out);

// Страниц на весь резидентный сэмпл хватает. Место проверяется до чтения:
// иначе отказ читал бы сэмпл до конца страниц, а повтор после вытеснения -
// заново (у WC это перемотка файла).
inline constexpr char kPsramFull[] = "PSRAM кончилась";
bool resident_pages_fit(const memory::TrackMemory& mem, const SampleDescriptor& sd);

} // namespace soundsinth::model
