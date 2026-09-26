/*
 * fw.h - z80_fw глазами Си.
 *
 * Библиотека на ассемблере: аргументы в регистрах, DEHL под 32-битные
 * числа, IX под приёмник. Соглашение SDCC другое, между ними лежит слой
 * переходников.
 *
 * Всё, что и так передаётся через память, объявлено переменными.
 */

#ifndef FW_H
#define FW_H

/* Типы те же, что у клиента шины. */
#include "../common/types.h"

/* -- Карта -- */

/** Поднять карту. 0 - получилось. */
u8 fw_sd_init(void);

/* -- Том -- */

/**
 * Разобрать том: сам найдёт раздел, если карта размечена как диск.
 * 0 - получилось.
 */
u8 fw_mount(void);

extern u8  fat_spc;              /* секторов в кластере */

/* -- Страницы -- */
/*
 * Библиотека своей памяти не занимает: страницы под отрезки файлов и под
 * таблицу каталога называет приложение. Заполнить до fw_mount.
 *
 * Берём 0, 4 и 6: банки 2 и 5 уже видны в других окнах, свободных ровно
 * три.
 */
extern u8 fat_page_work;         /* отрезки играющего файла */
extern u8 fat_page_pre;          /* отрезки следующего */
extern u8 fat_page_dir;          /* таблица каталога */
extern u8 fat_port_shadow;       /* теневая копия 0x7FFD             */

/** Проверить, что названные страницы годятся. 0 - годятся. */
u8 fw_page_check(void);

/* -- Каталог -- */

/** Запись каталога - то, что вернул обход или таблица. */
extern u8  fat_ent_attr;         /* 0x10 - каталог */
extern u32 fat_ent_clus;
extern u32 fat_ent_size;
extern char fat_ent_name[];      /* CP866, ноль в конце */

/* Итог последнего построения отрезков (fw_open): сколько вышло и не
   переполнилась ли страница под них. */
extern u16 fat_ext_count;
extern u8  fat_ext_overflow;

#define FW_ATTR_DIR     0x10
#define FW_ATTR_VOLUME  0x08
#define FW_ATTR_HIDDEN  0x02
#define FW_ATTR_SYSTEM  0x04

/** Открыть корневой каталог. 0 - получилось. */
u8 fw_dir_open_root(void);

/** Открыть каталог, чей кластер лежит в fat_ent_clus. 0 - получилось. */
u8 fw_dir_open_ent(void);

/**
 * Следующая запись каталога прямо с диска.
 * 1 - есть, 0 - кончился, 0xFF - карта не ответила.
 */
u8 fw_dir_next(void);

/**
 * Прочитать открытый каталог целиком в страницу.
 * 0 - получилось; fat_dir_full = 1 - влезло не всё.
 */
u8 fw_dir_load(void);
extern u16 fat_dir_count;
extern u8  fat_dir_full;

/** Достать запись номер i из таблицы в fat_ent_*. 0 - получилось. */
u8 fw_dir_get(u16 i);
/* Включить страницу в окно 0xC000. Нужна только сортировке списка: она
   переставляет таблицу указателей каталога, а та живёт в странице.
   Перед выходом обязательно вернуть ту, что была (fat_port_shadow). */
void fw_dir_page(u8 page);

/* -- Файл -- */

/**
 * Найти файл по имени в открытом каталоге. Сравнение без учёта регистра.
 * 1 - нашли (всё в fat_ent_*), 0 - нет, 0xFF - карта не ответила.
 */
u8 fw_find(const char *name);

/**
 * Открыть файл: 0 - рабочий, 1 - префетч. Кластер и размер берутся из
 * fat_ent_*, то есть из последней найденной записи.
 * 0 - получилось.
 */
u8 fw_open(u8 slot);

/** Сделать префетченный файл рабочим. Диска не касается вовсе. */
void fw_adopt(void);

/** С каким файлом работают чтение и перемотка: 0 рабочий, 1 префетч. */
void fw_use(u8 slot);

extern u32 fat_file_size;
extern u32 fat_file_pos;
extern u16 fat_file_ext_count;   /* на сколько отрезков лёг файл */

/** Встать на позицию в байтах. */
void fw_seek(u32 pos);

/** Куда идут данные быстрого чтения: 0 - в память, 1 - прямо в порт платы. */
extern u8 fat_read_dest;
#define FW_DEST_MEM  0
#define FW_DEST_BUS  1

/**
 * Быстрое чтение, целыми секторами. Позиция обязана стоять на границе
 * сектора: карта отдаёт сектор целиком, и начать с середины нельзя.
 * Подряд идущие секторы уходят одной командой карте.
 * 0 - получилось.
 */
u8 fw_read_sectors(void *dst, u16 sectors);

/** Точное чтение: сколько попросили, с любой позиции. 0 - получилось. */
u8 fw_read_bytes(void *dst, u16 count);

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

extern u8 fw_scr_x;
extern u8 fw_scr_y;
extern u8 fw_scr_attr;          /* каким цветом печатаем */

void fw_scr_cls(u8 attr);
void fw_scr_fill_attr(u8 attr);
void fw_scr_at(u8 x, u8 y);
void fw_scr_putc(u8 ch);
void fw_scr_puts(const char *s);

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

/** Взять код из кольца; 0 - нажатий не было. */
u8 fw_kbd_get(void);

/** Ждать нажатия. Ждёт через HALT, а не пустым циклом. */
u8 fw_kbd_wait(void);

/** Забыть накопившееся: после долгой операции нажатия обычно не к месту. */
void fw_kbd_flush(void);

/* -- Прерывания -- */

/** Поставить свой обработчик (IM 2) и разрешить прерывания. */
void fw_irq_init(void);

/** Вернуть обработчик из ПЗУ. Обязательно перед выходом в TR-DOS. */
void fw_irq_stop(void);

/** Сколько кадров прошло с запуска - то есть пятидесятых долей секунды. */
extern u16 fw_ticks;

/** Подождать n кадров. */
void fw_wait_frames(u8 n);

#endif /* FW_H */
