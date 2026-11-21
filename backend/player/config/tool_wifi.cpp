// SPDX-License-Identifier: MIT
#include "player/config/tool_wifi.h"

#include <cstdio>
#include <cstring>
#include <iterator>

namespace soundsinth::config {
namespace {

constexpr const char* kPresets[] = {
    "AT", "AT+GMR", "AT+CWMODE?", "AT+CWLAP", "AT+CWJAP?", "AT+CIFSR", "AT+UART_CUR?", "AT+RST", "AT+RESTORE",
};
constexpr int kPresetCount = static_cast<int>(std::size(kPresets));

} // namespace

// Поле за полем: временный объект занял бы на стеке задачи модуля больше
// полукилобайта.
void AtTerminal::reset() {
    std::memset(out_, 0, sizeof(out_));
    std::memset(out_attr_, 0, sizeof(out_attr_));
    part_n_      = 0;
    in_n_        = 0;
    in_[0]       = 0;
    sent_[0]     = 0;
    preset_      = -1;
    status_[0]   = 0;
    status_attr_ = kToolInk;
    dirty_       = true;
}

void AtTerminal::push(const char* s, uint32_t n, uint8_t attr) {
    std::memmove(out_[0], out_[1], (kOutLines - 1u) * sizeof(out_[0]));
    std::memmove(out_attr_, out_attr_ + 1, kOutLines - 1u);
    if (n > kToolTextCols) n = kToolTextCols;
    std::memcpy(out_[kOutLines - 1u], s, n);
    out_[kOutLines - 1u][n]   = 0;
    out_attr_[kOutLines - 1u] = attr;
    dirty_                    = true;
}

void AtTerminal::flush_part() {
    if (part_n_ != 0) push(part_, part_n_, kToolInk);
    part_n_ = 0;
}

void AtTerminal::feed(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c == '\r') continue;
        if (c == '\n') {
            flush_part();
            continue;
        }
        part_[part_n_++] = ascii_printable(c) ? static_cast<char>(c) : '?';
        if (part_n_ == kToolTextCols) flush_part();
    }
    // Начатая строка видна сразу: приглашение ">" приходит без перевода.
    if (part_n_ != 0) dirty_ = true;
}

void AtTerminal::note(const char* text, uint8_t attr) {
    flush_part();
    push(text, static_cast<uint32_t>(std::strlen(text)), attr);
}

void AtTerminal::status(const char* text, uint8_t attr) {
    std::snprintf(status_, sizeof(status_), "%s", text);
    status_attr_ = attr;
    dirty_       = true;
}

bool AtTerminal::key(uint8_t k) {
    dirty_ = true;
    if (k == kToolKeyUp || k == kToolKeyDown) {
        preset_ = k == kToolKeyUp ? (preset_ <= 0 ? kPresetCount - 1 : preset_ - 1) : (preset_ + 1) % kPresetCount;
        std::snprintf(in_, sizeof(in_), "%s", kPresets[preset_]);
        in_n_ = static_cast<uint32_t>(std::strlen(in_));
        return false;
    }
    if (k == kToolKeyDel) {
        if (in_n_ != 0) in_[--in_n_] = 0;
        return false;
    }
    if (k == kToolKeyEnter) {
        if (in_n_ == 0) return false;
        std::memcpy(sent_, in_, in_n_ + 1u);
        flush_part();
        char line[kToolTextCols + 1];
        std::snprintf(line, sizeof(line), "> %.26s", sent_);
        push(line, static_cast<uint32_t>(std::strlen(line)), kToolCyan);
        in_n_  = 0;
        in_[0] = 0;
        return true;
    }
    if (!ascii_printable(k) || in_n_ >= kInputMax) return false;
    uint32_t quotes = 0;
    for (uint32_t i = 0; i < in_n_; ++i) {
        quotes += in_[i] == '"' ? 1u : 0u;
    }
    char c = static_cast<char>(k);
    if (quotes % 2u == 0 && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    in_[in_n_++] = c;
    in_[in_n_]   = 0;
    return false;
}

void AtTerminal::render(ToolScreen& scr) const {
    scr.clear();
    // Начатая строка модуля - последней, самая старая уходит.
    const uint32_t skip = part_n_ != 0 ? 1u : 0u;
    for (uint32_t i = 0; i + skip < kOutLines; ++i) {
        const uint32_t k = i + skip;
        scr.put(i, out_[k], out_attr_[k] != 0 ? out_attr_[k] : kToolInk);
    }
    if (skip != 0) {
        char tail[kToolTextCols + 1];
        std::memcpy(tail, part_, part_n_);
        tail[part_n_] = 0;
        scr.put(kOutLines - 1u, tail);
    }
    // Ввод: хвост, если длиннее строки, и клеточка курсора за ним.
    constexpr uint32_t kView = kToolTextCols - 3u;
    const uint32_t from      = in_n_ > kView ? in_n_ - kView : 0u;
    char line[kToolTextCols + 1];
    std::snprintf(line, sizeof(line), "%c %s", from != 0 ? '<' : '>', in_ + from);
    scr.put(kInputLine, line, kToolYellow);
    scr.cursor(kInputLine, 2u + in_n_ - from);
    scr.put(kToolHintLine, "UP/DOWN - READY COMMANDS");
    scr.put(kToolHintLine + 1u, "ENTER - SEND, DEL - ERASE");
    scr.put(kToolHintLine + 2u, status_, status_attr_);
}

} // namespace soundsinth::config
