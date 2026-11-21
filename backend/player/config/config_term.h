// SPDX-License-Identifier: MIT
#pragma once

// Экран конфигуратора в странице: ПЗУ - терминал.
//
// Плата держит в странице сетку 32x24 - код знака и атрибут на знакоместо
// - и список изменённых отрезков кадра. ПЗУ выводит отрезки своим шрифтом:
// у отрезка "только цвет" пишет одни атрибуты, у прочих - знаки и
// атрибуты. Нажатия ПЗУ кладёт в кольцо страницы.
//
// Кадр: плата пишет сетку и отрезки, только когда ПЗУ показало прошлый
// (shown == upd), и затем поднимает upd. ПЗУ запоминает upd, выводит,
// пишет его в shown. Сетку посреди вывода никто не трогает.
//
// Строка 0 - название, она за ПЗУ: отрезков в ней не бывает.

#include <cstdint>

namespace soundsinth::config {

inline constexpr uint32_t kTermMagic     = 0x4d525450u; // 'PTRM'
inline constexpr uint16_t kTermVersion   = 1;
inline constexpr uint32_t kTermCols      = 32;
inline constexpr uint32_t kTermRows      = 24;
inline constexpr uint32_t kTermKeySlots  = 8;
inline constexpr uint32_t kTermCharsAt   = 0x100;
inline constexpr uint32_t kTermAttrsAt   = 0x400;
inline constexpr uint32_t kTermPageBytes = kTermAttrsAt + kTermCols * kTermRows;
// Счётчик стартов ПЗУ, сразу за сеткой; писатель - только ПЗУ. Сменился -
// машину сбросили, экран пуст: следующий кадр плата шлёт целиком.
inline constexpr uint32_t kTermStartsAt = kTermPageBytes;
inline constexpr uint8_t kTermAll       = 0xff; // число отрезков: весь экран
inline constexpr uint8_t kTermSpanAttr  = 0;    // только атрибуты
inline constexpr uint8_t kTermSpanChars = 1;    // знаки и атрибуты

struct TermSpan {
    uint8_t row, col, len, kind;
};

struct TermHeader {
    uint32_t magic;
    uint16_t version;
    uint8_t upd;   // плата: номер выложенного кадра
    uint8_t shown; // ПЗУ: номер показанного
    uint8_t req;   // ПЗУ: положено нажатий, по модулю 256
    uint8_t ack;   // плата: взято
    uint8_t count; // отрезков кадра, kTermAll - весь экран
    uint8_t full;  // плата: следующий кадр целиком
    uint8_t keys[kTermKeySlots];
};
static_assert(sizeof(TermHeader) == 20, "the terminal header layout is frozen");
inline constexpr uint32_t kTermSpans = (kTermCharsAt - sizeof(TermHeader)) / sizeof(TermSpan);

struct TermGrid {
    uint8_t ch[kTermRows][kTermCols];
    uint8_t at[kTermRows][kTermCols];
};

// Пустая страница терминала: заголовок, сетка нулями. Первый кадр уйдёт
// целиком.
void term_page_init(uint8_t* page);

// Выложить кадр g, если ПЗУ показало прошлый. false - ещё показывает,
// кадр подождёт. Неизменившийся кадр не выкладывается.
bool term_publish(uint8_t* page, const TermGrid& g);

// Следующий кадр - целиком: ПЗУ стартовало заново и экран пуст.
void term_resend(uint8_t* page);

// Следующее нажатие из кольца. false - нажатий нет.
bool term_key(uint8_t* page, uint8_t& k);

} // namespace soundsinth::config
