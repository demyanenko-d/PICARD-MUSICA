/*
 * main.c - ПЗУ-конфигуратор платы.
 *
 * Показывает настройки, которые плата положила в страницу на 0x2000, даёт
 * их править стрелками и отдаёт обратно. Правила - чем какое поле
 * ограничено и какое недоступно - считает плата: здесь только рисование.
 */

#include "cfgbus.h"
#include "fw.h"
#include "page.h"

#define SCR_W 32
#define SCR_H 24

/* Сверху заголовок и подсказка по клавишам, снизу полоса подсказки в две
   строки: описания в тридцать два знака не влезают. */
#define LIST_TOP 2
#define LIST_ROWS 20
#define HINT_ROWS 2

/* Строка списка: либо заголовок раздела, либо поле. */
#define ROW_SECTION 0xFF

#define MAX_ROWS 48

static u8 row_field[MAX_ROWS]; /* номер поля или ROW_SECTION */
static u16 row_text[MAX_ROWS]; /* смещение заголовка раздела */
static u8 row_count;

static u8 cursor;  /* выбранная строка списка */
static u8 top;     /* первая показанная строка */
static bool_t editing;

/* -- Мелочи вывода -- */

static void put_spaces(u8 n) {
    while (n--) fw_scr_putc(' ');
}

/* Строка с обрезкой по ширине: длинное описание не должно уехать на
   следующую строку и сдвинуть весь список. */
static u8 put_clipped(const char *s, u8 width) {
    u8 n = 0;
    while (*s && n < width) {
        fw_scr_putc((u8)(*s++ & 0x7F));
        ++n;
    }
    return n;
}

static u8 put_u8(u8 v) {
    u8 n = 0;
    if (v >= 100) {
        fw_scr_putc((u8)('0' + v / 100));
        ++n;
    }
    if (v >= 10) {
        fw_scr_putc((u8)('0' + (v / 10) % 10));
        ++n;
    }
    fw_scr_putc((u8)('0' + v % 10));
    return (u8)(n + 1);
}

/* -- Доступ к странице -- */

static const page_field_t *field_at(u8 i) {
    const page_header_t *h = page_hdr();
    return (const page_field_t *)page_at(h->desc_offset + ((u16)i << 4));
}

static u8 *values(void) {
    return page_at(page_hdr()->values_offset);
}

/* Значения из флеша: правка их не меняет. */
static u8 *saved(void) {
    return page_at(page_hdr()->saved_offset);
}

static bool_t field_changed(u8 i) {
    return values()[i] != saved()[i] ? TRUE : FALSE;
}

static bool_t field_active(u8 i) {
    const u8 *bits = page_at(page_hdr()->mask_offset);
    return (bits[i >> 3] & (u8)(1u << (i & 7))) ? TRUE : FALSE;
}

/* Список значений поля: счётчик лежит первым байтом, записи - после
   четырёх байт заголовка. */
static u8 choice_count(const page_field_t *f) {
    return *page_at(f->choices);
}

static const page_choice_t *choice_at(const page_field_t *f, u8 n) {
    return (const page_choice_t *)page_at(f->choices + 4u + ((u16)n << 2));
}

/* -- Построение списка строк -- */

static void build_rows(void) {
    const page_header_t *h = page_hdr();
    u8 i;
    row_count = 0;
    for (i = 0; i < h->field_count && row_count < MAX_ROWS - 1; ++i) {
        const page_field_t *f = field_at(i);
        if (f->section != 0) {
            row_field[row_count] = ROW_SECTION;
            row_text[row_count]  = f->section;
            ++row_count;
        }
        row_field[row_count] = i;
        row_text[row_count]  = f->name;
        ++row_count;
    }
}

/* Первая строка, на которой можно стоять. */
static u8 first_field_row(void) {
    u8 r;
    for (r = 0; r < row_count; ++r) {
        if (row_field[r] != ROW_SECTION && field_active(row_field[r])) return r;
    }
    return 0;
}

/* -- Рисование -- */

static void draw_value(const page_field_t *f, u8 value, u8 width) {
    u8 n = 0;
    if (f->kind == PAGE_KIND_CHOICE) {
        u8 count = choice_count(f);
        u8 k;
        for (k = 0; k < count; ++k) {
            const page_choice_t *c = choice_at(f, k);
            if (c->value == value) {
                n = put_clipped(page_str(c->name), width);
                break;
            }
        }
        if (k == count) n = put_u8(value);
    } else {
        n = put_u8(value);
    }
    if (n < width) put_spaces((u8)(width - n));
}

static void draw_row(u8 r, u8 y) {
    const u8 sel = (r == cursor);
    u8 attr;
    if (row_field[r] == ROW_SECTION) {
        attr = FW_ATTR(FW_YELLOW, FW_BLACK) | FW_BRIGHT;
    } else if (!field_active(row_field[r])) {
        /* Недоступное видно, но не выбирается: иначе непонятно, куда
           делся пункт, который был вчера. */
        attr = FW_ATTR(FW_BLUE, FW_BLACK) | FW_BRIGHT;
    } else if (sel && editing) {
        attr = FW_ATTR(FW_BLACK, FW_YELLOW);
    } else if (sel) {
        attr = FW_ATTR(FW_BLACK, FW_CYAN);
    } else if (field_changed(row_field[r])) {
        /* Изменённое заметно и не под курсором: иначе правку увозят,
           просто пройдя по списку. */
        attr = FW_ATTR(FW_YELLOW, FW_BLACK) | FW_BRIGHT;
    } else {
        attr = FW_ATTR(FW_WHITE, FW_BLACK);
    }
    fw_scr_attr = attr;
    fw_scr_at(0, y);

    if (row_field[r] == ROW_SECTION) {
        u8 n;
        fw_scr_putc(' ');
        n = put_clipped(page_str(row_text[r]), SCR_W - 2);
        put_spaces((u8)(SCR_W - 1 - n));
        return;
    }

    {
        const page_field_t *f = field_at(row_field[r]);
        u8 n;
        /* Звёздочка в нулевом столбце: цвет цветом, а признак нужен и на
           чёрно-белом экране. */
        fw_scr_putc(field_changed(row_field[r]) ? (u8)'*' : (u8)' ');
        n = put_clipped(page_str(f->name), 19);
        put_spaces((u8)(20 - n));
        draw_value(f, values()[row_field[r]], 11);
    }
}

static void draw_list(void) {
    u8 y;
    for (y = 0; y < LIST_ROWS; ++y) {
        const u8 r = (u8)(top + y);
        if (r < row_count) {
            draw_row(r, (u8)(LIST_TOP + y));
        } else {
            fw_scr_attr = FW_ATTR(FW_WHITE, FW_BLACK);
            fw_scr_at(0, (u8)(LIST_TOP + y));
            put_spaces(SCR_W);
        }
    }
}

/* Вывести строку на две строки экрана, перенося по пробелу: описания
   длиннее тридцати двух знаков, и в одну строку от них виден огрызок. */
static void put_wrapped2(const char *s, u8 y0) {
    u8 line;
    for (line = 0; line < HINT_ROWS; ++line) {
        u8 brk = 0;
        u8 take;
        u8 k;
        u8 w;
        for (k = 0; k < SCR_W && s[k] != 0; ++k) {
            if (s[k] == ' ') brk = k;
        }
        /* Слово не рвём, если есть куда перенести. */
        take = (k == SCR_W && s[k] != 0 && brk != 0) ? brk : k;
        fw_scr_at(0, (u8)(y0 + line));
        for (w = 0; w < take; ++w) fw_scr_putc((u8)(s[w] & 0x7F));
        put_spaces((u8)(SCR_W - take));
        s += take;
        while (*s == ' ') ++s;
        if (*s == 0) break;
    }
    /* Остаток полосы - пробелами, иначе на экране виснет прошлая подсказка. */
    for (++line; line < HINT_ROWS; ++line) {
        fw_scr_at(0, (u8)(y0 + line));
        put_spaces(SCR_W);
    }
}

/* Полоса подсказки. У изменённого поля первая строка - прежнее значение:
   по нему видно, что именно уедет в сохранение. У остальных обе строки
   отданы описанию. */
static void draw_hint(void) {
    const u8 y0 = SCR_H - HINT_ROWS;
    fw_scr_attr = FW_ATTR(FW_BLACK, FW_WHITE);
    if (row_field[cursor] == ROW_SECTION) {
        u8 line;
        for (line = 0; line < HINT_ROWS; ++line) {
            fw_scr_at(0, (u8)(y0 + line));
            put_spaces(SCR_W);
        }
        return;
    }
    {
        const u8 i            = row_field[cursor];
        const page_field_t *f = field_at(i);
        if (!field_changed(i)) {
            put_wrapped2(page_str(f->comment), y0);
            return;
        }
        fw_scr_at(0, y0);
        {
            const u8 n = put_clipped(" was ", SCR_W);
            draw_value(f, saved()[i], 11);
            put_spaces((u8)(SCR_W - n - 11));
        }
        fw_scr_at(0, (u8)(y0 + 1));
        {
            const u8 n = put_clipped(page_str(f->comment), SCR_W);
            put_spaces((u8)(SCR_W - n));
        }
    }
}

static void draw_title(void) {
    u8 n;
    fw_scr_attr = FW_ATTR(FW_BLACK, FW_GREEN) | FW_BRIGHT;
    fw_scr_at(0, 0);
    n = put_clipped(" PI-CARD MUSICA settings", SCR_W);
    put_spaces((u8)(SCR_W - n));
    fw_scr_attr = FW_ATTR(FW_CYAN, FW_BLACK);
    fw_scr_at(0, 1);
    n = put_clipped(" ENTER edit  S save  Q quit", SCR_W);
    put_spaces((u8)(SCR_W - n));
}

/* -- Движение по списку -- */

static void scroll_to_cursor(void) {
    if (cursor < top) top = cursor;
    if (cursor >= (u8)(top + LIST_ROWS)) top = (u8)(cursor - LIST_ROWS + 1);
}

static void move_cursor(signed char step) {
    u8 r = cursor;
    for (;;) {
        const signed char next = (signed char)(r + step);
        if (next < 0 || (u8)next >= row_count) return;
        r = (u8)next;
        if (row_field[r] != ROW_SECTION && field_active(row_field[r])) break;
    }
    cursor = r;
    scroll_to_cursor();
}

/* Следующее или предыдущее допустимое значение поля. */
static u8 next_value(const page_field_t *f, u8 value, signed char step) {
    if (f->kind == PAGE_KIND_CHOICE) {
        const u8 count = choice_count(f);
        u8 k;
        for (k = 0; k < count; ++k) {
            if (choice_at(f, k)->value == value) break;
        }
        if (k == count) k = 0;
        if (step > 0) {
            k = (u8)((k + 1u) % count);
        } else {
            k = (u8)((k + count - 1u) % count);
        }
        return choice_at(f, k)->value;
    }
    if (step > 0) return (value < f->max) ? (u8)(value + 1u) : f->min;
    return (value > f->min) ? (u8)(value - 1u) : f->max;
}

static void change_value(signed char step) {
    const u8 i = row_field[cursor];
    const page_field_t *f;
    u8 *v;
    if (i == ROW_SECTION || !field_active(i)) return;
    f    = field_at(i);
    v    = values();
    v[i] = next_value(f, v[i], step);
    /* Сумма пересчитывается платой при ответе; здесь её надо привести в
       согласие, иначе страница будет отвергнута. */
    {
        const u8 count = (u8)page_hdr()->field_count;
        u8 sum         = 0;
        u8 k;
        for (k = 0; k < count; ++k) sum = (u8)(sum + v[k]);
        v[count] = sum;
    }
    /* Плата приводит значение к допустимому и пересчитывает, какие поля
       доступны: включённый DivMMC, например, отнимает выбор сброса. */
    if (!cfg_refresh()) return;
    /* Маска могла закрыть поле под курсором - сойти с него. */
    if (!field_active(i)) move_cursor(1);
}

/* -- Экран ожидания -- */

static void wait_screen(void) {
    fw_scr_cls(FW_ATTR(FW_WHITE, FW_BLACK));
    fw_scr_attr = FW_ATTR(FW_WHITE, FW_BLACK);
    fw_scr_at(0, 10);
    fw_scr_puts("  waiting for the board...");
}

static void dead_screen(void) {
    fw_scr_cls(FW_ATTR(FW_WHITE, FW_RED));
    fw_scr_attr = FW_ATTR(FW_WHITE, FW_RED) | FW_BRIGHT;
    fw_scr_at(0, 10);
    fw_scr_puts("  the board does not answer");
    fw_scr_at(0, 12);
    fw_scr_puts("  power off and on to retry");
}

void main(void) {
    wait_screen();
    if (!cfg_wait_board()) {
        dead_screen();
        for (;;) {
        }
    }

    build_rows();
    cursor = first_field_row();
    top    = 0;
    scroll_to_cursor();

    fw_scr_cls(FW_ATTR(FW_WHITE, FW_BLACK));
    draw_title();
    draw_list();
    draw_hint();
    fw_kbd_flush();

    for (;;) {
        const u8 key = fw_kbd_wait();
        switch (key) {
            case FW_KEY_UP:
                if (editing) break;
                move_cursor(-1);
                break;
            case FW_KEY_DOWN:
                if (editing) break;
                move_cursor(1);
                break;
            case FW_KEY_LEFT:
                if (editing) change_value(-1);
                break;
            case FW_KEY_RIGHT:
                if (editing) change_value(1);
                break;
            case FW_KEY_ENTER:
                editing = editing ? FALSE : TRUE;
                break;
            case FW_KEY_ESC:
                editing = FALSE;
                break;
            case 'S':
            case 's':
                cfg_exit(CFG_EXIT_SAVE);
                break;
            case 'Q':
            case 'q':
                cfg_exit(CFG_EXIT_DISCARD);
                break;
            default:
                continue;
        }
        draw_list();
        draw_hint();
    }
}
