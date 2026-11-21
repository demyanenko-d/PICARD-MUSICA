// SPDX-License-Identifier: MIT
#pragma once

// Трек VGM в памяти трека: журнал - в данных трека PSRAM (у .vgz после
// распаковки, сжатый файл на это время - во временном загрузчика), плеер и
// память чипов - в арене метаданных. Живёт до смены трека.

#include <cstdint>

#include "core/formats/byte_source.h"
#include "core/memory/track_memory.h"
#include "player/config.h"
#include "player/vgm/vgm.h"

namespace player::vgm {

// Журнал с петлёй: петля играется столько раз, затем музыка идёт дальше и
// гаснет за время затухания. Без петли гаснут последние секунды журнала.
inline constexpr uint32_t kTrackLoops = 2;
inline constexpr uint32_t kFadeFrames = SOUNDSINTH_END_FADE_SECONDS * kRate;

// Первые байты файла: журнал VGM или обёртка gzip.
bool sniff(const uint8_t* head, uint32_t bytes);

// Образ ПЗУ ритма YM2608 для трека: kRhythmRomBytes в памяти трека или
// nullptr - образа нет, ритм молчит.
using RhythmRomFn = const uint8_t* (*)(soundsinth::memory::TrackMemory& mem);

// Сбрасывает mem и кладёт в неё трек. nullptr - отказ, причина в error.
// service (может быть nullptr) зовётся во время распаковки. rhythm_rom
// зовётся только у файла с YM2608, до открытия чипов; nullptr - образ,
// поставленный set_rhythm_rom, не меняется.
Player* load_track(soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem, const char** error, void (*service)(void*) = nullptr,
                   void* user = nullptr, RhythmRomFn rhythm_rom = nullptr);

// Петля, которую стоит повторять: не короче секунды. Короче - отметка конца
// (в корпусе есть петля в 25 мс тишины перед концом журнала).
inline bool looped(const Info& i) {
    return i.loop_start != 0 && i.loop_samples >= kRate;
}

// Сколько раз играть петлю: kTrackLoops с поправками заголовка, как считает
// VGMPlay, - множитель в 1/16 и вычитаемая база; не меньше одного.
inline uint32_t track_loops(const Info& i) {
    const int32_t n = static_cast<int32_t>((kTrackLoops * i.loop_modifier + 8u) / 16u) - i.loop_base;
    return n < 1 ? 1u : static_cast<uint32_t>(n);
}

// Длина трека в отсчётах: журнал, у журнала с петлёй - ещё петли и время
// затухания; 0 - в заголовке длины нет.
inline uint32_t track_frames(const Player& p) {
    const Info& i = p.info();
    if (i.total_samples == 0) return 0;
    return looped(i) ? i.total_samples + i.loop_samples * (track_loops(i) - 1u) + kFadeFrames : i.total_samples;
}

// Кадров затухания перед концом трека.
inline uint32_t track_fade_frames(const Player& p) {
    const uint32_t quarter = track_frames(p) / 4u;
    return quarter < kFadeFrames ? quarter : kFadeFrames;
}

} // namespace player::vgm
