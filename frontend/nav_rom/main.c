/*
 * main.c - навигатор образов Beta Disk.
 *
 * Список держит плата и раскладывает окно строк по странице обмена в N0;
 * здесь - запросы, отрисовка и клавиши. Раскладка страницы приходит из
 * заголовка платы (nav_page.h делает сборка).
 *
 * Экран 32x24:
 *
 *   0       том и путь
 *   1..20   список: каталоги в скобках, образы и файлы как есть,
 *           отмеченные файлы - пурпурным текстом
 *   21..22  дисководы A, B / C, D
 *   23      клавиши, число и размер отмеченных или сообщение
 *
 * Рисуется всегда в экран 5: если виден экран 7, бит 3 7FFD на время
 * работы снимается, при выходе 7FFD возвращается как был.
 */

#include "../z80_fw/fw.h"
#include "obj/nav_page.h"

/* nav_io.s */
u8 nav_screen_save(void);
void nav_screen_restore(void);
void nav_out_7ffd(u8 v);
void nav_wait_ms(u8 n);

#define PAGE    ((u8 *)0x2000)
#define PAGE_V  ((volatile u8 *)0x2000)

#define ROW_LIST   1
#define ROWS       20
#define ROW_DRIVES 21
#define ROW_HELP   23

#define ATTR_TITLE  (FW_INK(FW_WHITE) | FW_PAPER(FW_BLUE) | FW_BRIGHT)
#define ATTR_LIST   (FW_INK(FW_BLACK) | FW_PAPER(FW_WHITE))
#define ATTR_OFF    (FW_INK(FW_BLUE) | FW_PAPER(FW_WHITE))
#define ATTR_CURSOR (FW_INK(FW_WHITE) | FW_PAPER(FW_BLACK) | FW_BRIGHT)
#define ATTR_MARK   (FW_INK(FW_MAGENTA) | FW_PAPER(FW_WHITE))
#define ATTR_CURSOR_MARK (FW_INK(FW_MAGENTA) | FW_PAPER(FW_BLACK) | FW_BRIGHT)
#define ATTR_MARKS  (FW_INK(FW_MAGENTA) | FW_BRIGHT)
#define ATTR_PLAIN  FW_INK(FW_WHITE)
#define ATTR_HELP   FW_INK(FW_CYAN)
#define ATTR_ALARM  (FW_INK(FW_RED) | FW_BRIGHT)
#define ATTR_MENU   (FW_INK(FW_BLACK) | FW_PAPER(FW_CYAN))

/* Меню: рамка двойной линией (CP866) посреди экрана. */
#define MENU_X 4
#define MENU_W 24
#define BOX_H  0xCD
#define BOX_V  0xBA

static u16 s_count;  /* пунктов в списке */
static u16 s_first;  /* пункт первой строки окна на странице */
static u16 s_top;    /* пункт первой строки экрана */
static u16 s_cur;
static bool_t s_note; /* в строке клавиш сообщение */

static u16 get16(u8 at)
{
    return (u16)(PAGE[at] | (PAGE[at + 1] << 8));
}

/* Запрос плате. Ответ или NP_RESULT_FAIL, если она молчит три секунды. */
static u8 ask(u8 cmd, u16 arg, u8 drive)
{
    u16 t;
    PAGE[NP_CMD] = cmd;
    PAGE[NP_ARG_LO] = (u8)arg;
    PAGE[NP_ARG_HI] = (u8)(arg >> 8);
    PAGE[NP_ARG_DRIVE] = drive;
    PAGE_V[NP_REQ] = (u8)(PAGE_V[NP_REQ] + 1);
    for (t = 0; t < 3000; ++t) {
        if (PAGE_V[NP_ACK] == PAGE_V[NP_REQ]) {
            s_count = get16(NP_COUNT);
            s_first = get16(NP_FIRST);
            return PAGE[NP_RESULT];
        }
        nav_wait_ms(1);
    }
    return NP_RESULT_FAIL;
}

/* Текст с колонки x, добитый пробелами до width. */
static void field(u8 x, u8 y, u8 width, const char *s, u8 attr)
{
    fw_scr_attr = attr;
    fw_scr_at(x, y);
    while (*s && width) {
        fw_scr_putc((u8)*s++);
        --width;
    }
    fw_scr_fill(width);
}

/* Строка пункта i на странице или NULL, если он вне окна. */
static const u8 *item(u16 i)
{
    if (i >= s_count || i < s_first || i - s_first >= NP_ROW_COUNT) return NULL;
    return PAGE + NP_ROWS + (i - s_first) * NP_ROW_BYTES;
}

static void draw_row(u8 r)
{
    const u16 i = s_top + r;
    const u8 *row = item(i);
    const char *name;
    u8 n = 32;
    bool_t dir;

    if (row == NULL) {
        field(0, ROW_LIST + r, 32, "", ATTR_LIST);
        return;
    }
    dir = (row[0] & NP_ROW_KIND_MASK) < NP_KIND_IMAGE;
    if (row[0] & NP_ROW_MARKED_BIT) {
        fw_scr_attr = i == s_cur ? ATTR_CURSOR_MARK : ATTR_MARK;
    } else {
        fw_scr_attr = i == s_cur ? ATTR_CURSOR : (row[0] & NP_ROW_LOCKED_BIT) ? ATTR_OFF : ATTR_LIST;
    }
    fw_scr_at(0, ROW_LIST + r);
    if (dir) {
        fw_scr_putc('[');
        --n;
    }
    for (name = (const char *)row + 1; *name && n > 1; --n) fw_scr_putc((u8)*name++);
    if (dir) {
        fw_scr_putc(']');
        --n;
    }
    fw_scr_fill(n);
}

/* Опрос клавиатуры - и между строками: прерываний нет, а перерисовка
   длится несколько кадров, короткое нажатие в ней иначе пропадает. */
static void draw_list(void)
{
    u8 r;
    for (r = 0; r < ROWS; ++r) {
        draw_row(r);
        if ((r & 3) == 3) fw_kbd_scan();
    }
}

static void put_num(u16 v)
{
    char s[5];
    u8 n = 0;
    do {
        s[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) fw_scr_putc((u8)s[--n]);
}

/* Нижняя строка: клавиши; есть отмеченные файлы - их число и место на диске. */
static void draw_help(void)
{
    const u16 marked = get16(NP_MARKED);
    s_note = FALSE;
    if (marked == 0) {
        field(0, ROW_HELP, 32, "ENTER open  SPACE mark  ESC exit", ATTR_HELP);
        return;
    }
    fw_scr_attr = ATTR_MARKS;
    fw_scr_at(0, ROW_HELP);
    fw_scr_puts(" Marked files: ");
    put_num(marked);
    fw_scr_puts(", ");
    put_num(get16(NP_MARKED_KB));
    fw_scr_puts(" KB");
    fw_scr_fill((u8)(32 - fw_scr_x));
}

static void note(const char *s)
{
    field(0, ROW_HELP, 32, s, ATTR_ALARM);
    s_note = TRUE;
}

static void draw_all(void)
{
    u8 d;
    field(0, 0, 32, (const char *)PAGE + NP_TITLE, ATTR_TITLE);
    draw_list();
    for (d = 0; d < 4; ++d) {
        const char *name = (const char *)PAGE + NP_DRIVES + d * 16;
        const u8 x = (d & 1) ? 16 : 0, y = ROW_DRIVES + (d >> 1);
        fw_scr_attr = ATTR_PLAIN;
        fw_scr_at(x, y);
        fw_scr_putc((u8)('A' + d));
        fw_scr_putc(':');
        field(x + 2, y, 14, *name ? name : "-", ATTR_PLAIN);
    }
    draw_help();
}

/* Курсор в экране, окно страницы покрывает экран. */
static void follow(u16 old)
{
    const u16 top = s_top;
    if (s_cur < s_top) s_top = s_cur;
    if (s_cur >= s_top + ROWS) s_top = s_cur - (ROWS - 1);
    if (s_top < s_first || s_top + ROWS > s_first + NP_ROW_COUNT) ask(NP_CMD_WINDOW, s_top, 0);
    if (s_top != top) {
        draw_list();
    } else {
        draw_row((u8)(old - s_top));
        draw_row((u8)(s_cur - s_top));
    }
}

/* Линия рамки меню: углы left и right. */
static void menu_line(u8 y, u8 left, u8 right)
{
    u8 i;
    fw_scr_attr = ATTR_MENU;
    fw_scr_at(MENU_X, y);
    fw_scr_putc(left);
    for (i = MENU_W - 2; i; --i) fw_scr_putc(BOX_H);
    fw_scr_putc(right);
}

/* Строка меню в рамке. */
static void menu_row(u8 y, const char *s, u8 attr)
{
    fw_scr_attr = ATTR_MENU;
    fw_scr_at(MENU_X, y);
    fw_scr_putc(BOX_V);
    fw_scr_attr = attr;
    fw_scr_putc(' ');
    field(MENU_X + 2, y, MENU_W - 3, s, attr);
    fw_scr_attr = ATTR_MENU;
    fw_scr_putc(BOX_V);
}

/* Меню: заголовок в верхней линии рамки, под ней строка sub (NULL - нет)
   и n пунктов. Возврат - пункт; 0xFF - отказ (Esc, Backspace). */
static u8 menu(const char *title, const char *sub, const char *const *items, u8 n)
{
    u8 y = (u8)((ROW_DRIVES - n - (sub ? 3 : 2)) / 2 + 1);
    u8 sel = 0, i, k;
    menu_line(y, 0xC9, 0xBB);
    fw_scr_at(MENU_X + 2, y);
    fw_scr_putc(' ');
    fw_scr_puts(title);
    fw_scr_putc(' ');
    if (sub) menu_row(++y, sub, ATTR_MENU);
    menu_line((u8)(y + n + 1), 0xC8, 0xBC);
    for (;;) {
        for (i = 0; i < n; ++i) menu_row((u8)(y + 1 + i), items[i], i == sel ? ATTR_CURSOR : ATTR_MENU);
        do {
            nav_wait_ms(20);
            fw_kbd_scan();
            k = fw_kbd_take();
        } while (k == 0);
        if (k == FW_KEY_UP && sel > 0) --sel;
        if (k == FW_KEY_DOWN && sel + 1 < n) ++sel;
        if (k == FW_KEY_ENTER) return sel;
        if (k == FW_KEY_ESC || k == FW_KEY_DEL) return 0xFF;
    }
}

static const char *const k_image_drives[8] = {
    "A: read and write", "A: read only", "B: read and write", "B: read only",
    "C: read and write", "C: read only", "D: read and write", "D: read only",
};

/* Дисковод для образа name: номер, разряд 7 - только чтение; 0xFF - отказ. */
static u8 pick_drive(const char *name)
{
    const u8 sel = menu("Insert into drive", name, k_image_drives, 8);
    return sel == 0xFF ? 0xFF : (u8)((sel >> 1) | ((sel & 1) << 7));
}

/* Список сменился: курсор - куда сказала плата, по возможности посреди
   экрана. */
static void moved(void)
{
    s_cur = get16(NP_CURSOR);
    s_top = s_cur < ROWS ? 0 : s_cur - ROWS / 2;
    if (s_top + ROWS > s_count && s_count > ROWS) s_top = s_count - ROWS;
    if (s_top + ROWS > NP_ROW_COUNT) ask(NP_CMD_WINDOW, s_top, 0);
    draw_all();
}

/* Отказ платы словами. */
static void refused(u8 r)
{
    note(r == NP_RESULT_TOO_LONG    ? " Too long: over 255 sectors"
         : r == NP_RESULT_TOO_MANY  ? " 128 files is the limit"
         : r == NP_RESULT_NO_ROOM   ? " No room: the disk holds 1 MB"
         : r == NP_RESULT_NO_FILE   ? " This file does not open"
         : r == NP_RESULT_FRAGMENTS ? " This file is in too many pieces"
                                    : " Drive busy: disk not made");
}

/* Пробел: отметка файла под курсором, курсор - на строку ниже. Чего
   отметить нельзя, пропускается: файл - с сообщением. FALSE - курсор уже
   на последней строке. */
static bool_t mark(void)
{
    const u8 *row = item(s_cur);
    const u16 old = s_cur;
    u8 r;
    if (row != NULL && (row[0] & NP_ROW_KIND_MASK) == NP_KIND_FILE) {
        r = ask(NP_CMD_MARK, s_cur, 0);
        if (r == NP_RESULT_OK) {
            draw_help();
        } else {
            refused(r);
        }
    }
    if (s_cur + 1 < s_count) ++s_cur;
    follow(old);
    return s_cur != old;
}

static const char *const k_drives[4] = {"Drive A", "Drive B", "Drive C", "Drive D"};
static const char *const k_marked[3] = {"Disk of marked files", "Clear all marks", "Insert this image"};

/* Диск TR-DOS из отмеченных файлов (отметок нет - из файла под курсором) -
   в дисковод. */
static void make_disk(void)
{
    const u8 d = menu("Disk of files into", NULL, k_drives, 4);
    const u8 r = d == 0xFF ? NP_RESULT_OK : ask(NP_CMD_MOUNT_SET, s_cur, d);
    const u16 old = s_cur;
    draw_all();
    if (r == NP_RESULT_OK) return;
    /* Сборка встала на файле: курсор - на него. */
    if (r == NP_RESULT_NO_FILE || r == NP_RESULT_FRAGMENTS) {
        s_cur = get16(NP_CURSOR);
        follow(old);
    }
    refused(r);
}

static void enter(void)
{
    const u8 *row = item(s_cur);
    u8 kind, r, d;
    if (row == NULL || (row[0] & NP_ROW_LOCKED_BIT)) return;
    kind = row[0] & NP_ROW_KIND_MASK;
    if (kind >= NP_KIND_IMAGE && get16(NP_MARKED) != 0) {
        r = menu("Marked files", NULL, k_marked, kind == NP_KIND_FILE ? 2 : 3);
        if (r == 1) ask(NP_CMD_UNMARK, 0, 0);
        if (r != 2) {
            if (r == 0) {
                make_disk();
            } else {
                draw_all();
            }
            return;
        }
    } else if (kind == NP_KIND_FILE) {
        make_disk();
        return;
    }
    r = ask(NP_CMD_ENTER, s_cur, 0);
    if (r == NP_RESULT_MOVED) {
        moved();
    } else if (r == NP_RESULT_IMAGE) {
        d = pick_drive((const char *)row + 1);
        r = d == 0xFF ? NP_RESULT_OK : ask(NP_CMD_MOUNT, 0, d);
        draw_all();
        if (r == NP_RESULT_FAIL) note(" Image not inserted");
    } else {
        note(" Can not open");
    }
}

/* Автоповтор удержанного пробела, в опросах клавиатуры: до первого повтора
   и между следующими. Каркас клавиатуры повторяет только стрелки. */
#define REP_DELAY 25
#define REP_RATE  3
#define REP_OFF   0xFF

void main(void)
{
    u8 port, k;
    u8 held = 0; /* опросов с удержанным пробелом; REP_OFF - до отпускания */
    u16 old;
    bool_t shown;

    if (nav_screen_save()) return;
    PAGE_V[NP_REQ] = PAGE_V[NP_ACK];
    if (ask(NP_CMD_OPEN, 0, 0) == NP_RESULT_FAIL) {
        nav_screen_restore();
        return;
    }
    port = PAGE[NP_PORT7FFD];
    shown = (port & 0x28) == 0x08; /* виден экран 7, 7FFD не закрыт */
    if (shown) nav_out_7ffd(port & 0xF7);
    moved();

    for (;;) {
        nav_wait_ms(20);
        fw_kbd_scan();
        k = fw_kbd_take();
        /* Пробел без CAPS SHIFT (с ним это Esc). */
        if ((fw_kbd_matrix[7] & 1) || !(fw_kbd_matrix[0] & 1)) {
            held = 0;
        } else if (k == 0 && held != REP_OFF && ++held == REP_DELAY) {
            held = REP_DELAY - REP_RATE;
            k = ' ';
        }
        if (k == 0) continue;
        if (s_note) draw_help();
        if (k == FW_KEY_ESC) break;
        if (k == FW_KEY_ENTER) {
            enter();
        } else if (k == FW_KEY_DEL) {
            if (ask(NP_CMD_BACK, 0, 0) == NP_RESULT_MOVED) moved();
        } else if (k == ' ') {
            /* На последней строке повтор снимал бы и ставил отметку. */
            if (!mark()) held = REP_OFF;
        } else {
            old = s_cur;
            if (k == FW_KEY_UP && s_cur > 0) --s_cur;
            if (k == FW_KEY_DOWN && s_cur + 1 < s_count) ++s_cur;
            if (k == FW_KEY_LEFT) s_cur = 0;
            if (k == FW_KEY_RIGHT && s_count) s_cur = s_count - 1;
            if (s_cur != old) follow(old);
        }
    }
    /* Выход - когда клавиши отпущены. Esc - это CAPS SHIFT + SPACE, то есть
       BREAK: зажатый, он доходит до прерванной программы, и TR-DOS бросает
       её дисковую операцию (цикл ожидания INTRQ опрашивает BREAK). */
    for (;;) {
        u8 r, up = 0x1F;
        fw_kbd_scan();
        for (r = 0; r < 8; ++r)
            up &= fw_kbd_matrix[r];
        if ((up & 0x1F) == 0x1F) break;
        nav_wait_ms(20);
    }
    ask(NP_CMD_LEAVE, 0, 0);
    nav_screen_restore();
    if (shown) nav_out_7ffd(port);
}
