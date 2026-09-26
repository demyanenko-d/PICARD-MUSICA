#pragma once

// Выбор резидентного кодека сэмпла, общий для S3M/IT/XM. MOD не нужен:
// сэмпл всегда 8 бит, длина - u16 слов (до 128 КБ), до порога
// прореживания не дотягивает.

#include <cstdint>

#include "core/codec/dpcm8.h"
#include "core/model/instrument.h"

namespace soundsinth::model {

inline constexpr uint32_t kForceDownsampleSizeThresholdBytes = 1u * 1024u * 1024u; // 1 МБ, по размеру после кодирования
inline constexpr uint32_t kForceDownsampleRateThresholdHz = 20000; // не трогать и так низкочастотные Amiga-сэмплы

struct ResidentEncodingDecision {
    ResidentEncoding mode;
    bool decimate; // сэмпл прореживается 2:1 при упаковке
};

// is16bit задаёт базовый кодек (Raw8/Dpcm8); length_samples/c5_speed -
// исходные значения из заголовка файла. decimate решается по размеру
// базового кодека до прореживания: "если закодировать как есть, выйдет
// больше 1 МБ".
//
// allow_raw16 - решение на весь трек (choose_resident_encoding), поэтому не
// зависит от порядка сэмплов: иначе один и тот же сэмпл кодировался бы
// по-разному в зависимости от того, что грузилось перед ним.
inline ResidentEncodingDecision decide_resident_encoding(bool is16bit, uint32_t length_samples, uint32_t c5_speed,
                                                          bool allow_raw16 = false) {
    // 16 бит и место есть: хранить как есть, без потерь и без распаковки на
    // каждый выходной отсчёт. Прореживание не нужно: оно ради размера, а
    // размер проверен.
    if (is16bit && allow_raw16) return ResidentEncodingDecision{ResidentEncoding::Raw16, false};

    const ResidentEncoding mode = is16bit ? ResidentEncoding::Dpcm8 : ResidentEncoding::Raw8;

    uint32_t base_bytes;
    if (mode == ResidentEncoding::Raw8) {
        base_bytes = length_samples;
    } else {
        const uint32_t checkpoints = (length_samples + dpcm8::kCheckpointIntervalSamples - 1) / dpcm8::kCheckpointIntervalSamples;
        base_bytes = length_samples + checkpoints * static_cast<uint32_t>(sizeof(dpcm8::Dpcm8Checkpoint));
    }

    const bool decimate = base_bytes > kForceDownsampleSizeThresholdBytes && c5_speed > kForceDownsampleRateThresholdHz;
    return ResidentEncodingDecision{mode, decimate};
}

} // namespace soundsinth::model
