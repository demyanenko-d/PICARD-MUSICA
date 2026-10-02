/*
 * fw.h - то, что конфигуратор берёт у z80_fw.
 *
 * Только экран и клавиатура: карта, том и каталог ему не нужны - всё, что
 * он показывает, уже лежит в странице настроек.
 */

#ifndef CONFIG_ROM_FW_H
#define CONFIG_ROM_FW_H

#include "../common/types.h"

/* -- Цвета -- */

#define FW_BLACK   0
#define FW_BLUE    1
#define FW_RED     2
#define FW_MAGENTA 3
#define FW_GREEN   4
#define FW_CYAN    5
#define FW_YELLOW  6
#define FW_WHITE   7

/* Атрибут: чернила, бумага, яркость. */
#define FW_ATTR(ink, paper) ((u8)((ink) | ((paper) << 3)))
#define FW_BRIGHT 0x40

/* -- Экран -- */

extern u8 fw_scr_x;
extern u8 fw_scr_y;
extern u8 fw_scr_attr; /* каким цветом печатаем */

void fw_scr_cls(u8 attr);
void fw_scr_fill_attr(u8 attr);
void fw_scr_at(u8 x, u8 y);
void fw_scr_putc(u8 ch);
void fw_scr_puts(const char *s);

/* -- Клавиатура -- */
/*
 * Коды те же, что у ПЗУ машины. Через этот же путь приходят клавиши USB:
 * плата выводит свою страницу на порт 0xFE, только пока клавиша нажата,
 * поэтому обе клавиатуры работают сразу и мешать друг другу не могут.
 */
#define FW_KEY_LEFT  8
#define FW_KEY_RIGHT 9
#define FW_KEY_DOWN  10
#define FW_KEY_UP    11
#define FW_KEY_DEL   12
#define FW_KEY_ENTER 13
#define FW_KEY_ESC   27
#define FW_KEY_SPACE 32

u8 fw_kbd_get(void);
u8 fw_kbd_wait(void);
void fw_kbd_flush(void);

/* -- Время -- */

/* Кадров с запуска, то есть пятидесятых долей секунды. Считает обработчик
   в crt0, поэтому volatile: без него цикл ожидания читает его один раз. */
extern volatile u16 fw_ticks;

#endif
