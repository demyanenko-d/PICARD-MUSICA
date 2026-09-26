#pragma once

// Загрузка и разбор трека поверх любого soundsinth::formats::ByteSource.
//
// Формат определяется не по расширению (в протоколе нет имени файла) и не
// отдельной таблицей сигнатур: каждый загрузчик сам проверяет сигнатуру и
// чисто отказывает (false + error_out). run_session_load() пробует их по
// очереди, от самых специфичных сигнатур (MThd, IMPM, XM, SCRM) к MOD с
// самой широкой эвристикой на смещении 1080, и берёт первый успешный.

#include <cstdint>

#include "core/engine/song_duration.h"
#include "core/model/song.h"
#include "core/formats/byte_source.h"
#include "core/memory/track_memory.h"

namespace soundsinth::bank {
struct Bank;
}

namespace player::load {

// Каким загрузчиком разобран трек. При прогрессивной загрузке PCM
// сэмплов тянется позже метаданных, и надо знать, чей load_sample_pcm()
// вызывать (load_track_sample).
enum class TrackFormat : uint8_t { None, Midi, It, Xm, S3m, Mod };

// Банк инструментов для .mid. Без него MThd не узнаётся, и файл
// отвергается как "не наш", а не падает посреди загрузки. На плате банк
// берётся с карты, без него - из региона флеша
// (SOUNDSINTH_BANK_FLASH_OFFSET); ставится раз при старте.
void session_loader_set_bank(const soundsinth::bank::Bank* bank);

inline constexpr uint8_t kLoaderCount = 5;

// Имя i-го загрузчика в порядке попыток ("MIDI", "IT", ...) - подпись к
// SessionLoadResult::attempt_errors[i].
const char* session_loader_name(uint8_t i);

// Итог run_session_load; заполняется всегда.
struct SessionLoadResult {
    TrackFormat format = TrackFormat::None;
    // Один проход песни в выходных отсчётах, до повтора (order_pos, row):
    // секвенсор песню сам не заканчивает. По ней вызывающий видит конец трека
    // и время для хоста.
    uint32_t total_frames = 0;
    soundsinth::engine::DurationStats duration; // как кончился проход длительности
    // Отказ: причина последнего загрузчика.
    const char* error = nullptr;
    // Почему отказал каждый загрузчик, в порядке session_loader_name(). Одной
    // последней причины мало: на файле с сигнатурой IMPM она "неизвестная
    // или неподдерживаемая сигнатура MOD", настоящая не видна.
    const char* attempt_errors[kLoaderCount] = {};
};

// Сбрасывает mem и Song, пробует загрузчики по очереди, при успехе считает
// длительность. Хосту ничего не сообщает. metadata_only: без PCM сэмплов,
// их тянет load_track_sample(); длительность верна, она по паттернам.
bool run_session_load(soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem, soundsinth::model::Song& out_song,
                       SessionLoadResult& result, bool metadata_only = false);

// PCM одного сэмпла загрузчиком, которым разобран трек. Только после
// run_session_load(metadata_only = true).
bool load_track_sample(TrackFormat format, soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem,
                        const soundsinth::model::Song& song, uint16_t sample_index,
                        const char** reason_out = nullptr);

} // namespace player::load
