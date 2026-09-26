#pragma once

// Что загрузчик трекерного файла (MOD, S3M, XM, IT) потерял при разборе и
// полной загрузке. Ядро само не печатает: pc_player выводит строку "load:",
// плата - через debug_log оркестратора. Писатель один - загрузчик (на
// плате Core1), сбрасывает в начале load().

#include <cstdint>

#include "core/model/song.h"

namespace soundsinth::model {

struct TrackerLoadStats {
    uint16_t samples_nonresident = 0;   // сэмпл с данными, который не распаковывается (ADPCM)
    uint16_t samples_dropped = 0;       // данные сэмпла не помещаются в файл - выброшен при разборе
    uint16_t samples_failed = 0;        // полная загрузка: не лёг в PSRAM или каталог, файл оборвался
    uint16_t first_failed_sample = 0;
    const char* first_failure = nullptr;
    uint16_t ignored_sustain_loops = 0; // IT: петля удержания сэмпла не воспроизводится
    uint16_t ignored_autovibrato = 0;   // автовибрато сэмпла IT и инструмента XM не воспроизводится
    uint32_t effect_cells_dropped = 0;  // ячейки с командой, которая не разбирается (Effect::None)
    // Где по заголовкам кончается файл: наибольшее смещение сэмпла плюс его
    // байты. У MOD это точная длина файла, у S3M, XM и IT - нижняя граница
    // (сжатые сэмплы своей длины в заголовке не несут). Принятого меньше -
    // файл недобран, и разбирать его нечего.
    uint32_t source_end = 0;
};

inline TrackerLoadStats g_tracker_load_stats;

// Сэмплы с данными, которые загрузчик не распаковывает; зовётся после
// разбора всех заголовков.
inline void count_nonresident_samples(const Song& song) {
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        const SampleDescriptor& sd = song.samples[i];
        if (sd.length_samples > 0 && !sample_is_resident(sd)) ++g_tracker_load_stats.samples_nonresident;
    }
}

// Конец файла по заголовкам сэмплов; зовётся после разбора всех заголовков.
inline void note_source_end(const Song& song) {
    uint32_t end = 0;
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        const SampleDescriptor& sd = song.samples[i];
        const uint32_t bytes = sample_source_bytes(sd);
        if (bytes == 0) continue;
        const uint32_t e = sd.file_offset + bytes;
        if (e > end) end = e;
    }
    g_tracker_load_stats.source_end = end;
}

inline void note_sample_failed(uint16_t sample_index, const char* reason) {
    TrackerLoadStats& s = g_tracker_load_stats;
    if (s.samples_failed++ == 0) {
        s.first_failed_sample = sample_index;
        s.first_failure = reason;
    }
}

} // namespace soundsinth::model
