/*
 * ui.h - экран приложения.
 *
 * Раскладка на 32x24 знакоместа:
 *
 *   0   ugly player              <плата>
 *   1   file: <имя>
 *   2   State: <статус>          time: <мм:сс / мм:сс>
 *   3   samp :N    patt :N    inst :N
 *   4   load :N    queue:N    total:N
 *   5   voice:N    cpu  :N%
 *   6
 *   7   <текущий каталог>
 *   8   каталог, шестнадцать строк
 *   ..
 *   23
 *
 * Строки 3-5 - три колонки одной ширины, числа стоят друг под другом.
 * Подписи дополнены пробелами перед двоеточием до шести знаков, по самой
 * длинной (queue/total/voice): одной ширины колонок мало, числа идут
 * сразу за подписью.
 *
 * Цвета те же, что в плагине: жёлтый - загрузка, зелёный - играет,
 * голубой - кончилось, красный - беда.
 */

#ifndef UI_H
#define UI_H

#include "fw.h"

#define UI_ROW_TITLE   0
#define UI_ROW_FILE    1
#define UI_ROW_STATE   2
#define UI_ROW_COUNTS  3
#define UI_ROW_LOAD    4
#define UI_ROW_VOICES  5
#define UI_ROW_ERROR   6   /* последний отказ открытия; телеметрия её не трогает */
#define UI_ROW_PATH    7
#define UI_ROW_LIST    8
#define UI_LIST_ROWS   16

/** Колонки строк 3-5. Ширина 10 плюс пробел - три штуки ровно на 32. */
#define UI_COL1        0
#define UI_COL2        11
#define UI_COL3        22

/* Цвета - чернила по чёрной бумаге. */
#define UI_ATTR_TITLE  (FW_INK(FW_WHITE) | FW_BRIGHT)
#define UI_ATTR_PLAIN  FW_INK(FW_WHITE)
#define UI_ATTR_LOAD   FW_INK(FW_YELLOW)
#define UI_ATTR_PLAY   (FW_INK(FW_GREEN) | FW_BRIGHT)
#define UI_ATTR_DONE   (FW_INK(FW_CYAN) | FW_BRIGHT)
#define UI_ATTR_ALARM  (FW_INK(FW_RED) | FW_BRIGHT)

/* Каталог: чёрным по серому, выбранная строка наоборот. */
#define UI_ATTR_LIST   (FW_INK(FW_BLACK) | FW_PAPER(FW_WHITE))
#define UI_ATTR_CURSOR (FW_INK(FW_WHITE) | FW_PAPER(FW_BLACK) | FW_BRIGHT)
/* Играющая запись: жёлтая строка. Под курсором бумага чёрная, и жёлтыми
   становятся буквы - иначе курсор прятал бы признак. */
#define UI_ATTR_LIST_PLAY   (FW_INK(FW_BLACK) | FW_PAPER(FW_YELLOW))
#define UI_ATTR_CURSOR_PLAY (FW_INK(FW_YELLOW) | FW_PAPER(FW_BLACK) | FW_BRIGHT)

void ui_init(void);

/** Написать строку с колонок, добив пробелами до конца поля. */
void ui_field(u8 x, u8 y, u8 width, const char *text, u8 attr);

/** То же, но значением служит число. */
void ui_field_num(u8 x, u8 y, u8 width, const char *label, u16 value, u8 attr);
/* То же плюс до marks восклицательных знаков после числа: столько
   уровней сброса голосов из-за перегрузки. */
void ui_field_num_marks(u8 x, u8 y, u8 width, const char *label, u16 value, u8 marks, u8 attr);

/** Перерисовать всё, что приехало с платы. */
void ui_update(void);

/** Строка с текущим каталогом. */
void ui_draw_path(const char *path);

/** Одна строка списка: i - номер строки на экране, 0..UI_LIST_ROWS-1. */
void ui_draw_entry(u8 i, const char *name, u8 is_dir, u8 selected, u8 playing);

/** Сообщение о беде вместо времени. */
void ui_alarm(const char *text);

#endif /* UI_H */
