#pragma once

#include <cstdint>

namespace soundsinth::model {

// Точка огибающей: XM (громкость, панорама), IT (громкость, панорама,
// питч или фильтр), .mid (громкость). У MOD/S3M огибающих нет,
// Envelope::enabled остаётся false.
struct EnvelopePoint {
    uint16_t tick = 0;  // X, в тиках
    int16_t value = 0;  // Y: у громкости 0..64 (у .mid - децибелы, kQuirkEnvelopeDecibel), у панорамы и питча - по формату, приводит загрузчик
};

// IT - до 25 точек, XM - до 12. Фиксированный массив, а не выделение
// в арене: огибающих на инструмент не больше трёх, в отличие от
// контрольных точек сэмпла, которых бывают десятки.
inline constexpr uint8_t kMaxEnvelopePoints = 25;

struct Envelope {
    // Инициализаторы по умолчанию у битовых полей только с C++20, проект на
    // C++17 - поэтому через конструктор.
    bool enabled : 1;
    bool sustain_enabled : 1;
    bool loop_enabled : 1;
    bool carry : 1;  // IT: позиция огибающей канала не сбрасывается новой нотой
    uint8_t point_count = 0;
    // XM знает одну sustain-точку, IT - диапазон (sustain loop).
    // sustain_point/sustain_end - обе границы; у XM
    // sustain_end == sustain_point.
    uint8_t sustain_point = 0;
    uint8_t sustain_end = 0;
    uint8_t loop_start = 0;
    uint8_t loop_end = 0;
    EnvelopePoint points[kMaxEnvelopePoints] = {};

    Envelope() : enabled(false), sustain_enabled(false), loop_enabled(false), carry(false) {}
};

// Огибающая трекера (XM, IT) после разбора точек - к виду OpenMPT:
// потерянный старший байт тика (так сохранял XI MPT 1.07) берётся у
// предыдущей точки, первая точка - на тике 0, тики не убывают, значения
// 0..64, номера петли и удержания - не дальше последней точки.
void sanitize_tracker_envelope(Envelope& env);

} // namespace soundsinth::model
