// SPDX-License-Identifier: MIT
// Потоковый разбор JSON.

#include "net/zxart/json_stream.h"

#include <algorithm>
#include <cstring>

namespace net::zxart {
namespace {

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

} // namespace

uint32_t utf8_encode(uint32_t code, char* out) {
    if (code < 0x80u) {
        out[0] = static_cast<char>(code);
        return 1;
    }
    if (code < 0x800u) {
        out[0] = static_cast<char>(0xc0u | (code >> 6));
        out[1] = static_cast<char>(0x80u | (code & 0x3fu));
        return 2;
    }
    out[0] = static_cast<char>(0xe0u | ((code >> 12) & 0x0fu));
    out[1] = static_cast<char>(0x80u | ((code >> 6) & 0x3fu));
    out[2] = static_cast<char>(0x80u | (code & 0x3fu));
    return 3;
}

void JsonStream::reset(const JsonHandler* handler) {
    handler_ = handler;
    state_   = State::Value;
    depth_   = 0;
    len_     = 0;
    code_n_  = 0;
    skip_    = 0;
}

void JsonStream::feed(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n && state_ != State::Failed; ++i) {
        // Знак, закрывший число или прервавший знак UTF-8, разбирается ещё
        // раз в новом состоянии.
        do {
            again_ = false;
            step(static_cast<char>(p[i]));
        } while (again_);
    }
}

void JsonStream::put(char c) {
    if (len_ < kTextMax) text_[len_++] = c;
}

void JsonStream::put_code(uint32_t code) {
    if (code >= 0x20u && code < 0x7fu) {
        put(static_cast<char>(code));
        return;
    }
    // Управляющие знаки и вне BMP - '_'; прочее - UTF-8, целым знаком или
    // никак, если не влезает.
    if (code < 0x80u || code > 0xffffu || (code >= 0xd800u && code <= 0xdfffu)) {
        put('_');
        return;
    }
    char b[3];
    const uint32_t k = utf8_encode(code, b);
    if (len_ + k > kTextMax) return;
    std::memcpy(text_ + len_, b, k);
    len_ += k;
}

void JsonStream::open(bool array) {
    if (depth_ >= kMaxDepth) {
        skip_        = 1;
        skip_string_ = false;
        skip_escape_ = false;
        state_       = State::Skip;
        return;
    }
    array_[depth_]  = array;
    key_[depth_][0] = '\0';
    ++depth_;
    handler_->open(handler_->ctx, depth_, array);
    state_ = array ? State::ValueOrEnd : State::KeyOrEnd;
}

void JsonStream::close(bool array) {
    if (depth_ == 0u || array_[depth_ - 1u] != array) {
        state_ = State::Failed;
        return;
    }
    handler_->close(handler_->ctx, depth_, array);
    --depth_;
    state_ = depth_ == 0u ? State::Done : State::After;
}

// Ключ значения; у элемента массива - ключ самого массива.
const char* JsonStream::value_key() const {
    if (depth_ == 0u) return "";
    if (!array_[depth_ - 1u]) return key_[depth_ - 1u];
    return depth_ >= 2u ? key_[depth_ - 2u] : "";
}

// Значение готово: обработчику, дальше запятая или конец контейнера.
void JsonStream::finish_value(bool quoted) {
    text_[len_] = '\0';
    handler_->value(handler_->ctx, depth_, value_key(), text_, quoted);
    state_ = depth_ == 0u ? State::Done : State::After;
}

void JsonStream::finish_string() {
    if (!is_key_) {
        finish_value(true);
        return;
    }
    const uint32_t n = std::min(len_, kKeyMax);
    std::memcpy(key_[depth_ - 1u], text_, n);
    key_[depth_ - 1u][n] = '\0';
    state_               = State::Colon;
}

// Пропуск контейнера глубже предела: только скобки вне строк.
void JsonStream::step_skip(char c) {
    if (skip_string_) {
        if (skip_escape_) {
            skip_escape_ = false;
        } else if (c == '\\') {
            skip_escape_ = true;
        } else if (c == '"') {
            skip_string_ = false;
        }
        return;
    }
    if (c == '"') {
        skip_string_ = true;
    } else if (c == '{' || c == '[') {
        ++skip_;
    } else if ((c == '}' || c == ']') && --skip_ == 0u) {
        state_ = State::After;
    }
}

// Знак внутри строки: сборка UTF-8, конец строки, начало escape.
void JsonStream::step_string(char c) {
    const auto u = static_cast<uint8_t>(c);
    if (code_n_ != 0u) {
        // Продолжение знака UTF-8; оборванный знак - '_'.
        if ((u & 0xc0u) != 0x80u) {
            put('_');
            code_n_ = 0;
            again_  = true;
            return;
        }
        code_ = (code_ << 6) | (u & 0x3fu);
        if (--code_n_ == 0u) put_code(code_);
        return;
    }
    if (c == '\\') {
        state_ = State::Escape;
    } else if (c == '"') {
        finish_string();
    } else if (u < 0x80u) {
        put(c);
    } else if ((u & 0xe0u) == 0xc0u) {
        code_   = u & 0x1fu;
        code_n_ = 1;
    } else if ((u & 0xf0u) == 0xe0u) {
        code_   = u & 0x0fu;
        code_n_ = 2;
    } else if ((u & 0xf8u) == 0xf0u) {
        code_   = u & 0x07u;
        code_n_ = 3;
    } else {
        put('_');
    }
}

void JsonStream::step(char c) {
    switch (state_) {
        case State::ValueOrEnd:
            if (space(c)) return;
            if (c == ']') {
                close(true);
                return;
            }
            state_ = State::Value;
            again_ = true;
            return;
        case State::Value:
            if (space(c)) return;
            if (c == '{' || c == '[') {
                open(c == '[');
            } else if (c == '"') {
                len_    = 0;
                code_n_ = 0;
                is_key_ = false;
                state_  = State::String;
            } else if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
                len_ = 0;
                put(c);
                state_ = State::Literal;
            } else {
                state_ = State::Failed;
            }
            return;
        case State::KeyOrEnd:
        case State::Key:
            if (space(c)) return;
            if (c == '}' && state_ == State::KeyOrEnd) {
                close(false);
            } else if (c == '"') {
                len_    = 0;
                code_n_ = 0;
                is_key_ = true;
                state_  = State::String;
            } else {
                state_ = State::Failed;
            }
            return;
        case State::Colon:
            if (space(c)) return;
            state_ = c == ':' ? State::Value : State::Failed;
            return;
        case State::After:
            if (space(c)) return;
            if (c == ',') {
                state_ = array_[depth_ - 1u] ? State::Value : State::Key;
            } else if (c == '}' || c == ']') {
                close(c == ']');
            } else {
                state_ = State::Failed;
            }
            return;
        case State::String:
            step_string(c);
            return;
        case State::Escape:
            state_ = State::String;
            switch (c) {
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    put(' ');
                    return;
                case 'u':
                    code_   = 0;
                    code_n_ = 4;
                    state_  = State::Hex;
                    return;
                default:
                    put(c);
                    return;
            }
        case State::Hex: {
            uint32_t v = 0;
            if (c >= '0' && c <= '9') {
                v = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v = static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                state_ = State::Failed;
                return;
            }
            code_ = code_ * 16u + v;
            if (--code_n_ != 0u) return;
            // Вторая половина суррогатной пары знака не добавляет.
            if (code_ < 0xdc00u || code_ > 0xdfffu) put_code(code_);
            state_ = State::String;
            return;
        }
        case State::Literal:
            if (c == ',' || c == '}' || c == ']' || space(c)) {
                finish_value(false);
                again_ = state_ == State::After;
                return;
            }
            put(c);
            return;
        case State::Skip:
            step_skip(c);
            return;
        case State::Done:
            if (!space(c)) state_ = State::Failed;
            return;
        case State::Failed:
            return;
    }
}

} // namespace net::zxart
