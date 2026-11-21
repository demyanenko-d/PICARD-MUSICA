/*
 * fw.h - z80_fw для Си: экран, клавиатура, прерывания.
 *
 * Здесь то, что зовётся из SDCC напрямую: аргументы эти функции берут там,
 * где их кладёт компилятор. Карта и FAT носят ошибку во флаге переноса и
 * числа в DEHL, им нужны переходники - они у приложения.
 */

#ifndef Z80_FW_H
#define Z80_FW_H

#include "../common/types.h"

/* -- Экран -- */
/*
 * Цвет - обычный атрибут Спектрума: чернила в битах 0-2, бумага в 3-5,
 * яркость в 6, мигание в 7.
 */
#define FW_INK(c)    (c)
#define FW_PAPER(c)  ((c) << 3)
#define FW_BRIGHT    0x40

#define FW_BLACK   0
#define FW_BLUE    1
#define FW_RED     2
#define FW_MAGENTA 3
#define FW_GREEN   4
#define FW_CYAN    5
#define FW_YELLOW  6
#define FW_WHITE   7

/* Вывод 8x8 (scr.s): 32x24 знакоместа. */
extern u8 fw_scr_x;              /* куда пойдёт следующий символ */
extern u8 fw_scr_attr;           /* каким цветом печатаем */

void fw_scr_at(u8 x, u8 y);
void fw_scr_putc(u8 ch);
/** Строка, ноль в конце. */
void fw_scr_puts(const char *s);
/** n пробелов текущим цветом, в пределах строки. */
void fw_scr_fill(u8 n);

/* -- Клавиатура -- */
/*
 * Коды те же, что у ПЗУ: стрелок на этой клавиатуре нет, их читают как
 * CAPS SHIFT с цифрами 5-8.
 */
#define FW_KEY_LEFT   8
#define FW_KEY_RIGHT  9
#define FW_KEY_DOWN   10
#define FW_KEY_UP     11
#define FW_KEY_DEL    12
#define FW_KEY_ENTER  13
#define FW_KEY_ESC    27
#define FW_KEY_SPACE  32

/** Полуряды как есть с порта: 0 в бите - нажата. */
extern u8 fw_kbd_matrix[8];

/** Взять код из кольца; 0 - нажатий не было. */
u8 fw_kbd_get(void);

/** Ждать нажатия. Ждёт через HALT, а не пустым циклом. */
u8 fw_kbd_wait(void);

/** Забыть накопившееся: после долгой операции нажатия обычно не к месту. */
void fw_kbd_flush(void);

/* Без прерываний (fw_irq_init не зван): опросить клавиатуру самому и
   взять код из кольца. */
void fw_kbd_scan(void);
u8 fw_kbd_take(void);

/* -- Прерывания -- */

/** Поставить свой обработчик (IM 2) и разрешить прерывания. */
void fw_irq_init(void);

/** Вернуть обработчик из ПЗУ. Обязательно перед выходом в TR-DOS. */
void fw_irq_stop(void);

/** Вызывается из прерывания раз в кадр, после опроса клавиатуры; 0 - нет.
    Работает при запрещённых прерываниях и обязан уложиться в кадр. */
extern void (*fw_irq_hook)(void);

/** Сколько кадров прошло с запуска - то есть пятидесятых долей секунды. */
extern u16 fw_ticks;

/** Подождать n кадров. */
void fw_wait_frames(u8 n);

#endif /* Z80_FW_H */
