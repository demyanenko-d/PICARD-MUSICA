/*
 * txtlib.h - буферы и печать строк в окна WC. Реализация: asm/txtlib.s.
 *
 * Формат буфера, без завершающего нуля:
 *   buf[0] = ёмкость в символах, ставится один раз в buf_init
 *   buf[1] = текущая длина
 *   buf[2..] = символы
 *
 * Объявляется на два байта длиннее ёмкости: char work_buf[66] под 64.
 */

#ifndef TXTLIB_H
#define TXTLIB_H

#include "wc_api.h"

#define FS_SCREEN_COLS 96U  /* макс. ширина экрана (90x36, запас) */

/** Полезная ширина содержимого = ширина окна - 4; при 0 - FS_SCREEN_COLS. */
static inline uint8_t get_content_width(const wc_window_t *win) {
    uint8_t w = win->width;
    return (w ? w : FS_SCREEN_COLS) - 4U;
}

/** Инициализировать буфер: записать ёмкость, len=0. Один раз на буфер. */
static inline void buf_init(char *buf, uint8_t capacity) {
    buf[0] = capacity;
    buf[1] = 0;
}

/** Очистить буфер: len=0, ёмкость не меняется. */
static inline void buf_clear(char *buf) {
    buf[1] = 0;
}

/** Текущая длина буфера в символах. */
static inline uint8_t buf_len(const char *buf) {
    return buf[1];
}

/**
 * Дописать строку с нулём в конце; не влезшее отбрасывается. Возвращает,
 * сколько символов добавлено; так же buf_append_char и _hex.
 */
uint8_t buf_append_str(char *buf, const char *src);

/** Дописать один символ. */
uint8_t buf_append_char(char *buf, char c);

/** Дописать байт двумя знаками hex, "00".."FF". */
uint8_t buf_append_u8_hex(char *buf, uint8_t val);

/** Дописать uint16 десятичным, без ведущих нулей. */
uint8_t buf_append_u16_dec(char *buf, uint16_t val);

/** Дописать uint32 десятичным. */
uint8_t buf_append_u32_dec(char *buf, uint32_t val);

/** Дописать "MM:SS", пять знаков, без проверки ёмкости. sec - 0..5999. */
void buf_append_mmss(char *buf, uint16_t sec);

/**
 * Вывести строку буфера в окно, добив пробелами до ширины содержимого.
 * y - строка внутри окна, от 1; attr - WC_COLOR(bg, fg).
 */
void print_line(wc_window_t *win, uint8_t y, const char *buf, uint8_t attr);

#endif /* TXTLIB_H */
