#pragma once

#include <cstdint>

namespace soundsinth::mixbus {

// Q24.8: 256 единиц - одна единица int16. В целых каждый голос отбрасывает
// дробь вниз: шум и смещение нуля растут с полифонией. У Q16.16 нет запаса
// над шкалой; у Q24.8 48 дБ: 64 голоса .mid по 2.5 шкалы в фазе - 1.34 млрд
// из 2.1.
inline constexpr uint32_t kMixFracBits = 8;

// Контракт источника звука: структура с указателями на функции вместо
// виртуального класса, без vtable и RTTI на МК.
struct SoundSource {
    void* self;
    // Добавляет (суммирует, не перезаписывает) свой вклад в буфер блока.
    // mix_l/mix_r в Q24.8 (kMixFracBits): полная громкость около
    // +-32767*256. Источник не ограничивает сам, это делает MixBus::render()
    // один раз в конце, после округления до целых.
    void (*render_add)(void* self, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames);
};

} // namespace soundsinth::mixbus
