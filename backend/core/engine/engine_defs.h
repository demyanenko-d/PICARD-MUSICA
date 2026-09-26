#pragma once

// Общие величины движка, которые нужны и тем, кому секвенсор не нужен
// (голос, фильтр, загрузчик сессии, прошивка). Не настройки для -D: под
// эту частоту подобраны длины линий ревербератора (reverb.h).

#include <cstdint>

#include "core/model/instrument.h"
#include "core/model/quirks.h"

namespace soundsinth::engine {

// Частота выхода, отсчётов в секунду.
inline constexpr uint32_t kSampleRateHz = 44100;

// Дробная точка Q16.16: позиция и шаг голоса (Voice::frac_pos, step),
// уровень затухания канала (ChannelState::fadeout_level).
inline constexpr uint32_t kQ16Bits = 16;
inline constexpr uint32_t kQ16One = 1u << kQ16Bits;

// Q8 (256 = 1.0): шаг огибающих по темпу и загрузка рендера.
inline constexpr uint32_t kQ8Bits = 8;
inline constexpr uint32_t kQ8One = 1u << kQ8Bits;

// Проценты в Q8: 100% - kQ8One.
inline constexpr uint32_t pct_to_q8(uint32_t pct) {
    return (pct * kQ8One) / 100u;
}

// Новый бюджет голосов при перегрузке рендера: один голос на step_pct
// загрузки сверх порога, не ниже min_voices и не больше max_step за раз.
// Один голос с фильтром стоит около 1.8% загрузки, поэтому шаг в голос на
// каждые 2% возвращает рендер под порог за один-два тика, а не за двадцать.
inline constexpr uint8_t voice_budget_after_overload(uint8_t budget, uint32_t load_q8, uint32_t high_q8,
                                                     uint8_t min_voices, uint32_t step_q8, uint32_t max_step) {
    if (load_q8 <= high_q8 || budget <= min_voices) return budget;
    uint32_t step = 1u + (load_q8 - high_q8) / (step_q8 != 0 ? step_q8 : 1u);
    if (step > max_step) step = max_step;
    const uint32_t room = static_cast<uint32_t>(budget - min_voices);
    if (step > room) step = room;
    return static_cast<uint8_t>(budget - step);
}

// Опорные ноты Linear, общие для голоса и диспетчера эффектов: иначе на
// Note-Trigger и на первом тике портаменто одна нота даёт разные частоты.
inline constexpr uint8_t kReferenceNote = 48; // XM: relative_note 0, finetune 0 -> частота == c5_speed на ноте 48 (C-4)
inline constexpr uint8_t kItLinearReferenceNote = 60; // IT: частота == c5_speed на ноте 60 (C-5)

inline constexpr uint8_t linear_reference_note(soundsinth::model::QuirkFlags quirks) {
    return (quirks & soundsinth::model::kQuirkItLinearC5Reference) ? kItLinearReferenceNote : kReferenceNote;
}

// Единица позиции питча Linear - 1/64 полутона (768 на октаву): шкала IT
// для линейных слайдов и вибрато.
inline constexpr int32_t kLinearAmountUnitsPerSemitone = 64;

// Шкалы величин форматов - общие с загрузчиками, живут в formats/common.
using soundsinth::model::kGlobalVolumeMax;
using soundsinth::model::kPanCenter;
using soundsinth::model::kPanMax;
using soundsinth::model::kVolumeMax;

// Срез фильтра - 0..127, 127 - фильтр открыт.
inline constexpr uint8_t kFilterCutoffOpen = 127;
// Точки огибающих - 0..64. Без сдвига: у огибающей громкости 64 (множитель
// 1), у панорамы и питча 32.
inline constexpr uint8_t kEnvelopeNeutral = 64;
inline constexpr uint8_t kEnvelopeCenter = 32;
// Модификатор среза от огибающей фильтра - -256..+256; +256 - открытый
// срез, как без огибающей.
inline constexpr int16_t kFilterEnvNeutral = 256;

} // namespace soundsinth::engine
