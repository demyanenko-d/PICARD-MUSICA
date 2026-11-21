// SPDX-License-Identifier: MIT
#pragma once

// Терминал модуля Wi-Fi в TOOLS. Здесь только строки экрана и правка
// ввода; разговор с модулем ведёт порт.

#include <cstdint>

#include "player/config/config_tools.h"

namespace soundsinth::config {

// Терминал: ответы модуля сверху, строка ввода внизу, готовые команды
// стрелками. Буквы вне кавычек становятся заглавными: AT их требует.
class AtTerminal {
public:
    static constexpr uint32_t kOutLines  = 12;
    static constexpr uint32_t kInputMax  = 64;
    static constexpr uint32_t kInputLine = 13;

    void reset();
    // Нажатие. true - строка готова к отправке, она в command().
    bool key(uint8_t k);
    const char* command() const { return sent_; }
    // Байты модуля: строки по переводу строки, длинные переносятся.
    void feed(const uint8_t* p, uint32_t n);
    // Строка от платы, своим цветом.
    void note(const char* text, uint8_t attr);
    void status(const char* text, uint8_t attr);
    void render(ToolScreen& scr) const;
    bool dirty() const { return dirty_; }
    void clean() { dirty_ = false; }

private:
    void push(const char* s, uint32_t n, uint8_t attr);
    void flush_part();

    char out_[kOutLines][kToolTextCols + 1] = {};
    uint8_t out_attr_[kOutLines]            = {};
    char part_[kToolTextCols + 1]           = {};
    uint32_t part_n_                        = 0;
    char in_[kInputMax + 1]                 = {};
    uint32_t in_n_                          = 0;
    char sent_[kInputMax + 1]               = {};
    int preset_                             = -1;
    char status_[kToolTextCols + 1]         = {};
    uint8_t status_attr_                    = kToolInk;
    bool dirty_                             = false;
};

} // namespace soundsinth::config
