// SPDX-License-Identifier: MIT
#pragma once

// Разбор SysEx: что из него берут и загрузчик .mid, и живой поток. Тело
// сообщения - без F0 и завершающего F7.

#include <cstdint>

namespace soundsinth::formats::midi {

// Объявление канала ударным - три сообщения по спецификациям Roland GS и
// Yamaha XG.
//
// GS "Use For Rhythm Part": F0 41 dev 42 12 40 1n 15 vv sum F7. Блок n - не
// номер канала: партия 1 (n=0) на десятом канале, партии 2..10 (n=1..9) на
// первом..девятом, 11..16 (n=A..F) на своих. vv: 0 - мелодический, 1 и 2 -
// наборы ударных.
//
// XG "Part Mode": F0 43 1n 4C 08 part 07 vv F7, part - индекс канала, vv 0 -
// обычный, остальное - ударные.
//
// GM On (F0 7E dev 09 01), GM2 On (09 03), GS Reset (40 00 7F 00) и XG System
// On (F0 43 1n 4C 00 00 7E 00 F7) возвращают умолчание.
inline void sysex_drum_channels(const uint8_t* s, uint32_t len, bool* drum_channel) {
    auto reset_default = [&]() {
        for (uint32_t i = 0; i < 16; ++i) {
            drum_channel[i] = (i == 9);
        }
    };
    if (len >= 4 && s[0] == 0x7e && s[2] == 0x09 && (s[3] == 0x01 || s[3] == 0x03)) {
        reset_default();
        return;
    }
    if (len >= 8 && s[0] == 0x41 && s[2] == 0x42 && s[3] == 0x12) {
        if (s[4] == 0x40 && s[5] == 0x00 && s[6] == 0x7f) {
            reset_default();
            return;
        }
        if ((s[5] & 0xf0u) == 0x10 && s[6] == 0x15) {
            const uint8_t n  = static_cast<uint8_t>(s[5] & 0x0fu);
            const uint8_t ch = n == 0 ? 9 : (n <= 9 ? static_cast<uint8_t>(n - 1) : n);
            if (ch < 16) drum_channel[ch] = s[7] != 0;
        }
        return;
    }
    if (len >= 7 && s[0] == 0x43 && (s[1] & 0xf0u) == 0x10 && s[2] == 0x4c) {
        if (s[3] == 0x00 && s[4] == 0x00 && s[5] == 0x7e) {
            reset_default();
            return;
        }
        if (s[3] == 0x08 && s[5] == 0x07 && s[4] < 16) drum_channel[s[4]] = s[6] != 0;
    }
}

// Общая громкость из SysEx: Roland GS (41 dev 42 12 40 00 04 vv) и
// универсальная (7F dev 04 01 lsb msb). Нужна как событие со временем - в
// трёх файлах библиотеки ею сделаны затухания всего трека.
inline bool sysex_master_volume(const uint8_t* s, uint32_t len, uint8_t& vol) {
    if (len >= 8 && s[0] == 0x41 && s[2] == 0x42 && s[3] == 0x12 && s[4] == 0x40 && s[5] == 0x00 && s[6] == 0x04) {
        vol = static_cast<uint8_t>(s[7] & 0x7fu);
        return true;
    }
    if (len >= 6 && s[0] == 0x7f && s[2] == 0x04 && s[3] == 0x01) {
        vol = static_cast<uint8_t>(s[5] & 0x7fu);
        return true;
    }
    return false;
}

// Сколько байт тела читают разборы выше: столько живому потоку и нужно
// держать, остальное сообщение пропускается.
inline constexpr uint32_t kSysexPrefix = 8;

} // namespace soundsinth::formats::midi
