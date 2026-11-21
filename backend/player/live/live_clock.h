// SPDX-License-Identifier: MIT
#pragma once

// Часы живых чипов: штамп записи из отвода (live_tap.h) - в такт чипа.
// Рендер разбирает записи с постоянной задержкой, и каждая ложится на свой
// такт.

#include <cstdint>

#include "player/live/live_tap.h"

namespace player::live {

inline constexpr uint64_t kLiveBusHz = 2000000; // штамп шины - в полумикросекундах
// Задержка звука от записи: больше, чем рендер забегает вперёд вывода (пять
// буферов по 256 отсчётов - 29 мс), иначе запись приходит позже своего
// отсчёта.
inline constexpr uint64_t kLiveDelayHalves = kLiveBusHz / 20u; // 50 мс
// Рендер стоял дольше - отсчёт времени заново: записи паузы ложатся разом.
inline constexpr uint64_t kLiveGapHalves = kLiveBusHz / 10u;
// Задержка с запасом меньше полуоборота штампа: разность штампов со знаком.
static_assert(kLiveDelayHalves < (kTapStampMask >> 1), "the delay fits the signed stamp range");

// Часы шины сейчас, в полумикросекундах - тот же таймер платы, из которого
// обработчик шины делает штамп.
uint64_t live_clock_now();
// Свои часы вместо таймера платы - для тестов; nullptr - вернуть таймер.
void live_clock_set(uint64_t (*clock)());

// Фактическая частота вывода (мГц) против номинальной (Гц): делитель PIO
// вывода не делит частоту ядра ровно, и рендер, привязанный к выводу, уходит
// от часов шины - на десятки миллионных. Без поправки запас задержки 21-50 мс
// кончается через 11-25 минут непрерывного звука. Звать после старта вывода.
void live_clock_set_out_rate(uint64_t actual_mhz, uint32_t nominal_hz);
// Поправка: отношение частот минус единица, Q32.
extern int32_t g_live_out_skew_q32;

// Привязка тактов чипа к часам шины.
struct LiveClock {
    bool anchored        = false;
    uint64_t last        = 0; // часы прошлого рендера
    uint64_t origin_bus  = 0; // время шины такта origin_tick
    uint64_t origin_tick = 0;

    // В начале рендера: now - часы шины, chip_now - текущий такт чипа.
    // true - перепривязка после простоя рендера (меню NMI): записи, лежащие
    // в кольце, старше простоя, и штамп их не различает - старше полуоборота
    // (0.262 с) они выглядят записями из будущего и держали бы разбор до
    // 0.3 с. Источник кладёт их разом на origin_tick.
    bool sync(uint64_t now, uint64_t chip_now) {
        const bool gap = !anchored || now - last > kLiveGapHalves;
        // Запись в кольце старше простоя на задержку и один рендер; с
        // простоя kStaleHalves её возраст переходит полуоборот штампа.
        constexpr uint64_t kStaleHalves = ((kTapStampMask + 1u) >> 1) - kLiveDelayHalves - 16000u;
        const bool stale                = anchored && static_cast<int64_t>(now - last) > static_cast<int64_t>(kStaleHalves);
        if (gap) {
            origin_tick = chip_now;
            origin_bus  = now - kLiveDelayHalves;
            anchored    = true;
        }
        last = now;
        return stale;
    }

    // Такт чипа для записи со штампом stamp: тактов чипа num на den
    // полумикросекунд. Разность со знаком: запись, положенная после чтения
    // часов, моложе now, и без знака она стала бы записью полусекундной
    // давности - легла бы на 50 мс раньше своего такта. Время шины от привязки
    // - в шкале вывода (g_live_out_skew_q32).
    uint64_t tick_of(uint32_t stamp, uint64_t now, uint64_t num, uint64_t den) const {
        int32_t back = static_cast<int32_t>((static_cast<uint32_t>(now) - stamp) & kTapStampMask);
        if (back > static_cast<int32_t>(kTapStampMask >> 1)) back -= static_cast<int32_t>(kTapStampMask + 1u);
        const uint64_t at = now - back;
        if (at <= origin_bus) return origin_tick;
        const int64_t span = static_cast<int64_t>(at - origin_bus);
        const uint64_t out = static_cast<uint64_t>(span + ((span * g_live_out_skew_q32) >> 32));
        return origin_tick + out * num / den;
    }
};

} // namespace player::live
