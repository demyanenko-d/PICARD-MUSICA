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

// Параметр общего эффекта из SysEx: номер FxParam в param, значение 0..127
// в value. Одно сообщение - не больше одного параметра.
//
// GS (41 dev 42 12 40 01 aa vv): 30 - макрос реверберации, переводится во
// время хвоста; 33 - уровень реверберации; 34 - время; 3A - уровень
// хоруса.
//
// XG (43 1n 4C 02 01 aa ...): 00 - тип реверберации (MSB), переводится во
// время хвоста, тип 0 - эффект выключен, уровень 0; 0C - возврат
// реверберации; 20 - тип хоруса, тип 0 - уровень хоруса 0; 2C - возврат
// хоруса.
//
// Время по макросу и типу - приближение по названию зала: у GS и XG своя
// модель ревербератора, здесь меняется только длина хвоста.
inline bool sysex_effect_param(const uint8_t* s, uint32_t len, uint8_t& param, uint8_t& value) {
    constexpr uint8_t kLevel = 0, kTime = 1, kChorus = 2; // как model::FxParam
    if (len >= 8 && s[0] == 0x41 && s[2] == 0x42 && s[3] == 0x12 && s[4] == 0x40 && s[5] == 0x01) {
        const uint8_t v = static_cast<uint8_t>(s[7] & 0x7fu);
        switch (s[6]) {
            case 0x30: {
                // Room 1..3, Hall 1..2, Plate, Delay, Panning Delay.
                constexpr uint8_t kMacroTime[8] = {32, 40, 48, 64, 72, 56, 48, 48};
                param                           = kTime;
                value                           = kMacroTime[v & 7u];
                return true;
            }
            case 0x33:
                param = kLevel;
                value = v;
                return true;
            case 0x34:
                param = kTime;
                value = v;
                return true;
            case 0x3A:
                param = kChorus;
                value = v;
                return true;
            default:
                return false;
        }
    }
    if (len >= 7 && s[0] == 0x43 && (s[1] & 0xf0u) == 0x10 && s[2] == 0x4c && s[3] == 0x02 && s[4] == 0x01) {
        const uint8_t v = static_cast<uint8_t>(s[6] & 0x7fu);
        switch (s[5]) {
            case 0x00:
                if (v == 0) {
                    param = kLevel;
                    value = 0;
                    return true;
                }
                param = kTime;
                // Hall, Room, Stage, Plate; 10..13 - White Room, Tunnel, Canyon, Basement.
                switch (v) {
                    case 0x02:
                        value = 40;
                        break;
                    case 0x03:
                        value = 56;
                        break;
                    case 0x04:
                        value = 56;
                        break;
                    case 0x10:
                        value = 36;
                        break;
                    case 0x11:
                        value = 96;
                        break;
                    case 0x12:
                        value = 110;
                        break;
                    case 0x13:
                        value = 44;
                        break;
                    default:
                        value = 64;
                        break;
                }
                return true;
            case 0x0C:
                param = kLevel;
                value = v;
                return true;
            case 0x20:
                if (v != 0) return false;
                param = kChorus;
                value = 0;
                return true;
            case 0x2C:
                param = kChorus;
                value = v;
                return true;
            default:
                return false;
        }
    }
    return false;
}

// Сколько байт тела читают разборы выше: столько живому потоку и нужно
// держать, остальное сообщение пропускается.
inline constexpr uint32_t kSysexPrefix = 8;

} // namespace soundsinth::formats::midi
