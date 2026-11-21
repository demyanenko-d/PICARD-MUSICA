/*
 * ui.h - экран приложения, плеер 0.5.
 *
 * Сверху дека (знакоместа 0..8): статус, формат, часы, CPU и CH, VU,
 * прогресс, строка чипов, спектр, строка инфо, строка типа, кнопки. Под
 * ней нижняя панель (строки 9..22) - файловый браузер шрифтом 6x8:
 * строка 9 - путь, 10..22 - список.
 *
 * Статичная часть деки - пререндер (res/deck.scr), динамика дорисовывается
 * поверх.
 */

#ifndef UI_H
#define UI_H

#include "fw.h"

#define UI_LIST_ROWS   13
#define UI_PATH_CHARS  40   /* символов 6x8 в строке браузера */

void ui_init(void);

/** Перерисовать то, что приехало с платы; не чаще раза в кадр. */
void ui_update(void);

/** Строка с текущим каталогом, len - длина path; длинный - хвостом. */
void ui_draw_path(const char *path, u8 len);

/* Цвет строки списка, mark: биты курсора и играющего. */
#define UI_MARK_CURSOR  1
#define UI_MARK_PLAYING 2

/** Одна строка списка: i - номер строки на экране, 0..UI_LIST_ROWS-1. */
void ui_draw_entry(u8 i, const char *name, u8 is_dir, u8 mark);

/** Только цвет строки i, текст не трогается. */
void ui_entry_attr(u8 i, u8 mark);

/** Сдвинуть список на строку: down - строки 1..12 на место 0..11, иначе
    0..11 на 1..12. Освободившуюся строку рисует вызывающий. */
void ui_scroll_list(bool_t down);

/** Запущен файл: формат по расширению, имя и размер - в строку инфо.
    name должен жить, пока файл играет. */
void ui_set_file(const char *name, u32 size);

/** Режим повтора 0..3: всё, один, шафл, один раз - значок на деке. */
void ui_set_repeat(u8 mode);

/** Кнопка горит 0.14 с: NEXT - >> зелёным (смена трека), FORWARD - >>
    жёлтым (F), REPEAT - режим зелёным (M). */
enum { UI_FLASH_NEXT, UI_FLASH_FORWARD, UI_FLASH_REPEAT };
void ui_flash(u8 what);

/** Сообщение о беде: ERROR в строке типа, текст в строке инфо; до
    следующего файла. text должен жить до тех пор. */
void ui_alarm(const char *text);

#endif /* UI_H */
