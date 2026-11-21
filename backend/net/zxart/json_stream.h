// SPDX-License-Identifier: MIT
#pragma once

// Потоковый разбор JSON: байты приходят кусками, документ целиком не
// хранится. Обработчик получает открытие и закрытие контейнеров и
// значения вместе с ключом.
//
// Глубина - число открытых контейнеров: значение в корневом объекте лежит
// на глубине 1. Контейнеры глубже kMaxDepth пропускаются целиком, без
// вызовов обработчика. Строки длиннее kTextMax обрезаются по границе знака;
// знаки не ASCII - UTF-8, управляющие, суррогаты и вне BMP - '_'.

#include <cstdint>

namespace net::zxart {

// Знак BMP в UTF-8: в out 1-3 байта, возвращает их число.
uint32_t utf8_encode(uint32_t code, char* out);

struct JsonHandler {
    void* ctx;
    void (*open)(void* ctx, uint32_t depth, bool array);
    void (*close)(void* ctx, uint32_t depth, bool array);
    // key у элемента массива - ключ самого массива.
    void (*value)(void* ctx, uint32_t depth, const char* key, const char* text, bool quoted);
};

class JsonStream {
public:
    static constexpr uint32_t kMaxDepth = 14;
    static constexpr uint32_t kKeyMax   = 23;
    static constexpr uint32_t kTextMax  = 95;

    void reset(const JsonHandler* handler);
    void feed(const uint8_t* p, uint32_t n);
    bool failed() const { return state_ == State::Failed; }
    // Корневой контейнер закрыт.
    bool done() const { return state_ == State::Done; }

private:
    enum class State : uint8_t {
        Value,
        ValueOrEnd,
        KeyOrEnd,
        Key,
        Colon,
        After,
        String,
        Escape,
        Hex,
        Literal,
        Skip,
        Done,
        Failed,
    };

    void put(char c);
    void put_code(uint32_t code);
    void step(char c);
    void step_string(char c);
    void step_skip(char c);
    void open(bool array);
    void close(bool array);
    void finish_value(bool quoted);
    void finish_string();
    const char* value_key() const;

    const JsonHandler* handler_ = nullptr;
    State state_                = State::Value;
    uint32_t depth_             = 0;
    bool array_[kMaxDepth]{};
    char key_[kMaxDepth][kKeyMax + 1]{};
    char text_[kTextMax + 1]{};
    uint32_t len_     = 0;
    bool is_key_      = false;
    bool again_       = false;
    uint32_t code_    = 0; // знак UTF-8 или \u в сборке
    uint32_t code_n_  = 0; // сколько байтов или цифр осталось
    uint32_t skip_    = 0; // открытых контейнеров в пропуске
    bool skip_string_ = false;
    bool skip_escape_ = false;
};

} // namespace net::zxart
