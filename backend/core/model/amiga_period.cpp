// SPDX-License-Identifier: MIT
#include "core/model/amiga_period.h"

#include <iterator>

#include "core/model/instrument.h"
#include "core/model/song.h"

namespace soundsinth::model {

namespace {

// Таблица периодов ProTracker: семь октав, индекс 0 - наша
// нота 12 (856 - индекс 24, нота 36).
constexpr uint16_t kProTrackerPeriods[7 * 12] = {
    3424, 3232, 3048, 2880, 2712, 2560, 2416, 2280, 2152, 2032, 1920, 1812, 1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076,
    1016, 960,  907,  856,  808,  762,  720,  678,  640,  604,  570,  538,  508,  480,  453,  428,  404,  381,  360,  339,  320,
    302,  285,  269,  254,  240,  226,  214,  202,  190,  180,  170,  160,  151,  143,  135,  127,  120,  113,  107,  101,  95,
    90,   85,   80,   75,   71,   67,   63,   60,   56,   53,   50,   47,   45,   42,   40,   37,   35,   33,   31,   30,   28,
};
constexpr uint8_t kProTrackerFirstNote = 12;

constexpr bool amiga_table_inside_protracker() {
    for (uint32_t i = 0; i < std::size(kAmigaPeriodTable); ++i) {
        if (kProTrackerPeriods[kAmigaFirstNote - kProTrackerFirstNote + i] != kAmigaPeriodTable[i]) return false;
    }
    return true;
}
static_assert(amiga_table_inside_protracker(), "the three Amiga octaves are part of the ProTracker table");

// Период ноты по таблице трёх октав, за её пределами - удвоением по октавам
// (период вдвое меньше на октаву выше, вдвое больше ниже), с ограничением
// 16-битным периодом вместо переполнения.
constexpr uint16_t period_by_octaves(int32_t note) {
    int32_t table_note   = note;
    int32_t octave_shift = 0; // >0 - октав выше таблицы (период делится), <0 - ниже (умножается)
    while (table_note < kAmigaFirstNote) {
        table_note += 12;
        --octave_shift;
    }
    while (table_note > kAmigaLastNote) {
        table_note -= 12;
        ++octave_shift;
    }
    uint32_t period = kAmigaPeriodTable[table_note - kAmigaFirstNote];
    for (int32_t i = 0; i < octave_shift && period > 1; ++i) {
        period >>= 1;
    }
    for (int32_t i = 0; i < -octave_shift; ++i) {
        if (period > 0xffffu / 2u) {
            period = 0xffffu;
            break;
        }
        period <<= 1;
    }
    return static_cast<uint16_t>(period);
}

// Периоды нот 0..120: 120 - граница поиска amiga_snap_period.
struct NotePeriods {
    uint16_t period[kNoteMapSize + 1];
};

constexpr NotePeriods make_note_periods() {
    NotePeriods t{};
    for (uint32_t n = 0; n <= kNoteMapSize; ++n) {
        t.period[n] = period_by_octaves(static_cast<int32_t>(n));
    }
    return t;
}

constexpr NotePeriods kNotePeriods = make_note_periods();

} // namespace

uint8_t amiga_period_to_note(uint16_t period) {
    if (period == 0 || period == 0xfff) {
        return kNoteNone;
    }
    constexpr uint32_t count = std::size(kProTrackerPeriods);
    for (uint32_t i = 0; i < count; ++i) {
        if (period >= kProTrackerPeriods[i]) {
            // Между соседями - ближайший; строгое <: на середине - более высокая нота.
            if (period != kProTrackerPeriods[i] && i != 0 && kProTrackerPeriods[i - 1] - period < period - kProTrackerPeriods[i]) {
                return static_cast<uint8_t>(kProTrackerFirstNote + i - 1);
            }
            return static_cast<uint8_t>(kProTrackerFirstNote + i);
        }
    }
    return static_cast<uint8_t>(kProTrackerFirstNote + count - 1); // короче таблицы - самая высокая нота
}

uint16_t amiga_note_to_period(uint8_t note) {
    return kNotePeriods.period[note > kNoteMapSize ? kNoteMapSize : note];
}

uint16_t amiga_snap_period(uint16_t period, bool nearest) {
    if (period == 0) return 0;
    // Нижняя граница по нотам: период с ростом ноты убывает. Для nearest
    // граница между n и n+1 - среднее геометрическое их периодов (середина
    // полутона по высоте), сравнение в квадратах, без корня.
    const uint64_t p2       = static_cast<uint64_t>(period) * period;
    const uint16_t* periods = kNotePeriods.period;
    uint32_t lo             = 0;
    uint32_t count          = kNoteMapSize;
    while (count > 0) {
        const uint32_t step = count / 2;
        const uint32_t mid  = lo + step;
        const uint64_t pm   = periods[mid];
        const bool below    = nearest ? pm * periods[mid + 1] <= p2 : pm <= period;
        if (!below) {
            lo     = mid + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return periods[lo > kNoteMapSize - 1u ? kNoteMapSize - 1u : lo];
}

} // namespace soundsinth::model
