#pragma once

// Компандированная 8-битная дельта - резидентный формат 16-битных сэмплов.
// Линейная дельта +-127 на резких перепадах упирается в потолок (лесенка);
// здесь шаг растёт до полной шкалы, малые дельты точны до 1.
//
// Байт: биты 7:5 - scale (индекс kScaleTable), 4:0 - value со знаком
// (-16..15); delta = value * kScaleTable[scale]. Состояние - один predictor,
// он же контрольная точка; кодировщик прогоняет декодер после каждого байта.

#include <cstdint>

#include "platform/hot_path.h"
#include "core/audio/saturate.h"

namespace soundsinth::dpcm8 {

struct Dpcm8State {
    int16_t predictor = 0;
};

struct Dpcm8Checkpoint {
    int16_t predictor;
};

inline constexpr uint32_t kCheckpointIntervalSamples = 256;

// Прогрессия примерно x3, верхний масштаб не 2187 (3^7): value доходит до
// -16, а -16*2187 = -34992 переполняет int16. Нужно -16*S >= -32768
// (S <= 2048) и 15*S <= 32767 (S <= 2184), отсюда 2048: -16*2048 = -32768
// ровно, 15*2048 = 30720.
inline constexpr int32_t kScaleTable[8] = {1, 3, 9, 27, 81, 243, 729, 2048};

// Поля байта-кода (раскладка в шапке файла). Кодировщик и декодер обязаны
// совпадать, поэтому числа только здесь.
inline constexpr uint32_t kCodeScaleLsb = 5;
inline constexpr uint32_t kCodeValueBits = 0x1fu;
inline constexpr uint32_t kCodeValueSignBit = 0x10u;
inline constexpr int32_t kCodeValueMin = -static_cast<int32_t>(kCodeValueSignBit);    // -16
inline constexpr int32_t kCodeValueMax = static_cast<int32_t>(kCodeValueSignBit) - 1; // 15
static_assert(kCodeValueMin * kScaleTable[7] >= -32768 && kCodeValueMax * kScaleTable[7] <= 32767,
              "дельта кода не обрезается int16");

namespace detail {

constexpr int16_t compute_delta(int code) {
    const uint8_t scale = static_cast<uint8_t>(code) >> kCodeScaleLsb;
    int32_t v = static_cast<int32_t>(code & kCodeValueBits);
    if (v & kCodeValueSignBit) v -= static_cast<int32_t>(kCodeValueBits) + 1; // расширение знака 5 бит (-16..15)
    return static_cast<int16_t>(v * kScaleTable[scale]);
}

// Таблица на все 256 кодов -> дельта, считается при компиляции
// (constexpr-конструктор). Атрибут держит её в SRAM: иначе каждое чтение
// value[code] шло бы по общей с PSRAM шине QMI, даже когда код декодера в
// SRAM.
struct DeltaLut {
    int16_t value[256]{};
    constexpr DeltaLut() {
        for (int code = 0; code < 256; ++code) {
            value[code] = compute_delta(code);
        }
    }
};
// Атрибут на переменной (месте в памяти), а не на типе: section() на
// определении структуры экземпляры не переносит.
SOUNDSINTH_HOT_PATH_ATTR("kDeltaLut") inline constexpr DeltaLut kDeltaLut{};

} // namespace detail

// Один шаг декодера по значению: предиктор после байта-кода. В цикле
// предиктор живёт в регистре, в память - после цикла; через Dpcm8State&
// компилятор писал бы его на каждом байте (запись int16_t и чтение кода
// uint8_t для него могут перекрываться). В заголовке: зовётся на каждый
// отсчёт голоса из другой единицы трансляции, LTO выключен.
inline int32_t decode_step(int32_t predictor, uint8_t code) {
    return sat_s16(predictor + detail::kDeltaLut.value[code]);
}

// Декодирует один байт-код; им же пользуется кодировщик.
inline int16_t decode_delta(uint8_t code, Dpcm8State& state) {
    state.predictor = static_cast<int16_t>(decode_step(state.predictor, code));
    return state.predictor;
}

// Байт-код под sample относительно state.predictor: перебор 8
// масштабов, минимальная ошибка.
uint8_t quantize_sample(int16_t sample, const Dpcm8State& state);

// Кодирует sample_count моно-отсчётов int16 в sample_count байт и
// контрольные точки через каждые kCheckpointIntervalSamples (состояние
// перед отсчётом с абсолютным индексом, кратным 256). Возвращает число
// записанных точек. checkpoints_capacity >= ceil(sample_count /
// kCheckpointIntervalSamples), иначе часть точек не запишется (байты всё
// равно все).
//
// Потоковый режим (перепаковка большого сэмпла кусками): state - вход и
// выход, для первого куска Dpcm8State{} (predictor = 0).
// global_sample_offset - абсолютная позиция первого отсчёта вызова от
// начала сэмпла.
uint32_t encode_block(const int16_t* samples, uint32_t sample_count, uint32_t global_sample_offset, Dpcm8State& state,
                      int8_t* dpcm_out, Dpcm8Checkpoint* checkpoints_out, uint32_t checkpoints_capacity);

// Декодирует ровно sample_count отсчётов от состояния state (обычно из
// контрольной точки) в samples_out.
void decode_block(const int8_t* dpcm_in, uint32_t sample_count, Dpcm8State state, int16_t* samples_out);

} // namespace soundsinth::dpcm8
