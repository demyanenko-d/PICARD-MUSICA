// SPDX-License-Identifier: MIT
#pragma once

// Экраны TOOLS по готовым числам: без носителя и файловой системы.

#include <cstdint>

#include "core/config/config.h"

#include "devices/storage/fat_check.h"

#include "player/config/config_tools.h"

namespace player::config {

enum class CheckPhase : uint8_t { Scan, Done, Failed };

struct CheckView {
    CheckPhase phase;
    bool usb;     // флешка, а не карта
    bool present; // носитель есть
    devices::storage::FatVolume vol;
    devices::storage::FatCheck c;
    uint32_t took_ms;
    bool fixed;
    bool fix_failed;
};

void tool_render_check(soundsinth::config::ToolScreen& scr, const CheckView& k);

// Тест частоты карты: по строке на частоту из kSdSpeedHz.
enum class SpeedState : uint8_t { Wait, Run, Done, NoHighSpeed };

struct SpeedRow {
    SpeedState state;
    bool differs;    // данные не те, что на младшей частоте
    uint32_t errors; // сбоев обмена и неудачных чтений
    uint32_t kbps;   // чтение пачками, КБ/с
};

struct SpeedView {
    bool present; // карта есть
    bool running;
    bool stopped;    // прерван ESC
    uint8_t setting; // частота в настройках
    uint32_t done;   // чтений сделано и всего
    uint32_t total;
    SpeedRow rows[soundsinth::config::kSdSpeedCount];
};

void tool_render_speed(soundsinth::config::ToolScreen& scr, const SpeedView& v);

// Проверка USB HID: по две строки на интерфейс.
struct HidRow {
    uint8_t addr;
    uint8_t idx;
    uint8_t role; // platform::kUsbRole*, 0 - не взят
    bool boot;
    uint16_t vid;
    uint16_t pid;
    uint16_t top_page;
    uint16_t top_usage;
    uint16_t last_len; // 0 - отчётов ещё не было
    bool decoded;
    int32_t x; // мышь - приращение, пульт - значение оси
    int32_t y;
    uint16_t buttons; // разряд на кнопку карты
    uint8_t keys[2];  // клавиатура: модификаторы и первый код
};

inline constexpr uint32_t kHidRows = 6;

enum class HidSave : uint8_t { None, Saved, Failed };

struct HidView {
    uint8_t count;
    HidRow rows[kHidRows];
    HidSave save;
};

void tool_render_hid(soundsinth::config::ToolScreen& scr, const HidView& v);

} // namespace player::config
