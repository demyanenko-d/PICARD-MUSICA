/*
 * ui.c - экран плеера 0.5.
 *
 * Дека распаковывается из пререндера один раз, дальше рисуются только
 * поля, и только когда значение изменилось. Анимация (строка инфо, строка
 * чипов, VU, спектр) идёт от ui_update, его зовут на каждом круге главного
 * цикла; рисует он не чаще раза в кадр.
 */

#include <stddef.h>
#include <string.h>
#include "ui.h"
#include "../common/bus_client.h"

extern const u8 ui_deck[];      /* экран 6912 байт, PackBits */
extern const u8 ui_seg7[];      /* 0-9 : /, по 15 байт: строки 1..15 */
extern const u8 ui_digits4[];   /* 0-9 : /, по 6 байт: строки 1..6 */
extern const u8 ui_repeat[];    /* 4 значка 16x8, по 16 байт */
extern const u8 fw_font[];    /* по строкам: [строка 1..7 - 1][код] */

/* ui6.s: текст 6x8 поверх с пикселя ui6_x строки знакомест ui6_row;
   адрес байта знакоместа col строки пикселей y. */
extern u8 ui6_x, ui6_row, ui6_max;
void ui_puts6(const char *s);
u8 *scr(u8 col, u8 y);
/* ui6.s: строка браузера из ui6_buf - n символов, знакоместа 1..30. */
extern char ui6_buf[UI_PATH_CHARS];
void ui_line6(u8 row, u8 n);

/* dots.s: строка инфо - буфер 74 x 7 точек. */
void dots_init(void);
void dots_clear(void);
void dots_draw(u8 off);
void dots_hscroll(u8 column);
void dots_lift(void);
void dots_put(u8 col, u8 bits);
void spec_tick(void);           /* dots.s */
void vu_tick(void);             /* dots.s */

#define ATTR(row, col) (*(u8 *)(0x5800u + (u16)(row) * 32u + (col)))

/* Атрибуты: бумага голубая панели, чернила чёрные. */
#define A_PANEL     0x28
#define A_CPU_HOT   0x2A        /* красные чернила, CPU >= 90% */
#define A_LAMP_OFF  0x10        /* бумага красная */
#define A_LAMP_ON   0x20        /* бумага зелёная */
#define A_PLAYING   0x30        /* чёрным по жёлтому */
#define A_CURSOR    0x4F        /* белым по синему */
#define A_CURSOR_PLAYING 0x4E   /* жёлтым по синему */
#define A_DECK      0x38        /* кнопки: чёрные чернила по белой бумаге */
#define A_RED       0x3A        /* горящая кнопка - чернила цветом */
#define A_GREEN     0x3C
#define A_YELLOW    0x3E

/* Кнопки деки, строка 7: два знакоместа с col; горящая - цветные чернила. */
#define BTN_ROW     7
#define BTN_STOP    8
#define BTN_PLAY    10
#define BTN_FWD     12
#define BTN_REPEAT  15
#define FLASH_FRAMES 7          /* 0.14 с */

/* Поля деки. */
#define STATUS_X    2
#define FORMAT_X    72
#define REPEAT_COL  14
#define CLOCK_COL   17          /* 11 знакомест: мм:сс/мм:сс */
#define CPU_COL     12
#define CH_COL      15
#define BAR_X       138         /* прогресс: x = 138..220, y = 19..20 */
#define BAR_W       83
/* VU (рамка x = 79..128, L от 102 влево, R от 105 вправо) и спектр (полосы
   x = 2 + 4i, y = 9..31) рисует dots.s, их размеры - там. */
#define CHIP_ROW    3           /* строка чипов: x = 72..223 */
#define CHIP_X      72
#define CHIP_W      152
#define TYPE_ROW    6

/* Браузер: текст с x = 8, атрибут столбцов 1..30 - рамка панели не
   перекрашивается. */
#define LIST_COL    1
#define LIST_COLS   30
#define PATH_ROW    9
#define LIST_ROW    10

/* Высокая память 0xB7C0..0xBAFF (crt0.s): метаданные трека и состояние
   анимации. Буфер и таблицы точек - 0xBB00..0xBB67 (dots.s). */
#define TAGS        6           /* 0x10..0x14 и 0x17 */
#define TAG_LEN     80
#define CHIPS       8
typedef struct {
    u8   tag_len[TAGS];
    char tag[TAGS][TAG_LEN];
    char chip[CHIPS][12];       /* имя, ноль в конце; число - перед именем */
    u8   chip_cnt[CHIPS];
    u8   params[16];
    u8   params_len;
    u8   klass;
    u8   spec_h[16], spec_peak[16], spec_hold[16];
    u8   vu[2];
    char text[64];              /* строка инфо, собранная из чисел */
    char chips[64];             /* строка чипов */
} HiRam;
extern HiRam ui_hi;            /* 0xB7C0, адрес задан в dots.s */
/* dots.s берёт поля спектра по смещениям, а хвост HiRam с 0xBAE0 - под
   максимумы с платы и переменные спектра. */
_Static_assert(offsetof(HiRam, spec_h) == 608 && offsetof(HiRam, spec_peak) == 624 && offsetof(HiRam, spec_hold) == 640 &&
                   offsetof(HiRam, vu) == 656,
               "dots.s spectrum and VU offsets");
_Static_assert(sizeof(HiRam) <= 0xBAE0 - 0xB7C0, "HiRam runs into the tail used by dots.s");
#define HI (&ui_hi)
/* Строки массивов HiRam - только через эти указатели: SDCC 4.5 собирает
   HI->tag[slot][i] с переменным i в запись по одному адресу. */
#define TAG(slot)  ((char *)HI->tag + (u16)(slot) * TAG_LEN)
#define CHIP(slot) ((char *)HI->chip + (u16)(slot) * 12u)

/* Строки 1..7 знакоместа row, столбцы col..col+n-1; keep - биты первого
   байта, которые остаются (рамка на x = 0). Строка 0 у шрифта пустая. */
static void clear6(u8 row, u8 col, u8 n, u8 keep)
{
    u8 line, k;
    for (line = 1; line < 8; ++line) {
        u8 *p = scr(col, (u8)(row * 8u + line));
        *p &= keep;
        for (k = 1; k < n; ++k) p[k] = 0;
    }
}

static void text6(u8 x, u8 row, const char *s, u8 max)
{
    ui6_x = x;
    ui6_row = row;
    ui6_max = max;
    ui_puts6(s);
}

/* Деку из ui_deck на экран. PackBits: n < 0x80 - n + 1 байт как есть,
   иначе следующий байт n - 0x7F раз; до конца атрибутов, 0x5B00. */
static void unpack_deck(void) __naked
{
    __asm
    ld   hl, #_ui_deck
    ld   de, #0x4000
00001$:
    ld   a, (hl)
    inc  hl
    cp   #0x80
    jr   nc, 00002$
    ld   c, a
    ld   b, #0
    inc  bc
    ldir
    jr   00004$
00002$:
    sub  #0x7F
    ld   b, a
    ld   a, (hl)
    inc  hl
00003$:
    ld   (de), a
    inc  de
    djnz 00003$
00004$:
    ld   a, d
    cp   #0x5B
    jr   c, 00001$
    ret
    __endasm;
}

/* Число в строку без нулей впереди; возвращает конец. Цифры вычитанием:
   32-битное деление SDCC тянет из библиотеки 332 байта. */
static const u32 s_pow10[] = {1000000000ul, 100000000ul, 10000000ul, 1000000ul, 100000ul, 10000ul, 1000ul, 100ul, 10ul};

static char *put_u32(char *d, u32 v)
{
    const u32 *p;
    char c;
    bool_t lead = TRUE;
    for (p = s_pow10; p != s_pow10 + sizeof(s_pow10) / sizeof(s_pow10[0]); ++p) {
        for (c = '0'; v >= *p; ++c) v -= *p;
        if (c != '0' || !lead) { *d++ = c; lead = FALSE; }
    }
    *d++ = (char)('0' + (u8)v);
    return d;
}

/* BCD u32 младшими цифрами вперёд (метаданные) - в строку без нулей впереди. */
static char *put_bcd(char *d, const u8 *p)
{
    u8 i = 8;
    bool_t lead = TRUE;
    while (i--) {
        u8 dg = (u8)((p[i >> 1] >> ((i & 1) ? 4 : 0)) & 0x0F);
        if (dg || !lead || i == 0) { *d++ = (char)('0' + dg); lead = FALSE; }
    }
    return d;
}

static char *put_str(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    return d;
}

/* -- Поля деки -- */

static void seg7(u8 col, u8 glyph)
{
    /* y = 1..7 - знакоместо 0, 8..15 - знакоместо 1: внутри знакоместа
       строка ниже - на 256 дальше, адрес считается дважды, а не 15 раз. */
    const u8 *g = ui_seg7 + (u16)glyph * 15u;
    u8 *p = scr(col, 1);
    u8 y;
    for (y = 1; y < 16; ++y) {
        *p = *g++;
        p = y == 7 ? scr(col, 8) : p + 256;
    }
}

/* Две цифры числа 0..99 в g[0], g[1]: вычитанием - деление на 10 у SDCC
   стоит вызова библиотеки. */
static void digits2(u8 *g, u8 v)
{
    u8 t = 0;
    while (v >= 10) {
        v -= 10;
        ++t;
    }
    g[0] = t;
    g[1] = v;
}

/* Пара цифр 4x8 одним байтом в знакоместе col строки 1 (y = 9..14). */
static void pair4(u8 col, u8 v)
{
    const u8 *hi, *lo;
    u8 r, d[2];
    u8 *p;
    if (v > 99) v = 99;
    digits2(d, v);
    hi = ui_digits4 + (u16)d[0] * 6u;
    lo = ui_digits4 + (u16)d[1] * 6u;
    p = scr(col, 9); /* y = 9..14 - одно знакоместо */
    for (r = 0; r < 6; ++r, p += 256) *p = (u8)((hi[r] & 0xF0) | (lo[r] & 0x0F));
}

/* мм:сс/мм:сс; неизвестная длительность - 00:00. Рисуются только
   сменившиеся знаки: за секунду обычно один. */
static u8 s_clock_glyph[11] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void draw_clock(u8 pm, u8 ps, u8 dm, u8 ds)
{
    u8 g[11], i;
    digits2(g, pm); g[2] = 10;
    digits2(g + 3, ps); g[5] = 11;
    digits2(g + 6, dm); g[8] = 10;
    digits2(g + 9, ds);
    for (i = 0; i < 11; ++i) {
        if (g[i] == s_clock_glyph[i]) continue;
        s_clock_glyph[i] = g[i];
        seg7((u8)(CLOCK_COL + i), g[i]);
    }
}

/* Прогресс: до solid - сплошь, до dotted - через пиксель. Полоса байтами,
   а не по пикселю: знакоместо - первые s точек сплошные, первые d -
   штриховка по нечётным x (0x55: знакоместо начинается с
   чётного x). Строки 19 и 20 - в одном знакоместе, вторая на 256 дальше.
   Края полосы (x 136..137 и 221..223) не трогаются. */
#define BAR_BYTES ((BAR_X + BAR_W - 1) / 8 - BAR_X / 8 + 1)
static const u8 s_lead[9] = {0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF};

static void draw_bar(u8 solid, u8 dotted)
{
    u8 *p = scr(BAR_X / 8, 19);
    u8 sb = (u8)(solid + (BAR_X & 7)), db = (u8)(dotted + (BAR_X & 7)), lo = 0, c;
    for (c = 0; c < BAR_BYTES; ++c, lo += 8, ++p) {
        u8 s = sb > lo ? (u8)(sb - lo) : 0, d = db > lo ? (u8)(db - lo) : 0, keep, b;
        if (s > 8) s = 8;
        if (d > 8) d = 8;
        keep = c == 0 ? (u8)~(0xFFu >> (BAR_X & 7)) : c == BAR_BYTES - 1 ? (u8)(0xFFu >> ((BAR_X + BAR_W - 1) % 8 + 1)) : 0;
        b = (u8)((s_lead[s] | (s_lead[d] & 0x55)) & (u8)~keep);
        p[0] = (u8)((p[0] & keep) | b);
        p[256] = (u8)((p[256] & keep) | b);
    }
}

/* a * BAR_W / b; a не больше b, при b = 0 ноль. Сложением: остаток копится
   до b, сумма меньше 2b - точно до b < 0x8000, дальше оба делятся на 2. */
static u8 bar_frac(u16 a, u16 b)
{
    u16 acc = 0;
    u8 q = 0, i;
    if (!b) return 0;
    if (a > b) a = b;
    while (b & 0x8000u) { a >>= 1; b >>= 1; }
    for (i = 0; i < BAR_W; ++i) {
        acc += a;
        if (acc >= b) { acc -= b; ++q; }
    }
    return q;
}

static void btn(u8 col, u8 attr)
{
    ATTR(BTN_ROW, col) = attr;
    ATTR(BTN_ROW, col + 1) = attr;
}

/* Статус. */
static const char s_stop[] = "STOP", s_pause[] = "PAUSE";
static const char *const s_states[] = {"LOAD", "PLAY", "END", "ERROR"};

/* Статус словом и кнопками: играть нечего - STOP красная; грузится и пауза -
   PLAY жёлтая, играет - зелёная. */
static void draw_status(const char *s)
{
    clear6(0, 0, 4, 0x80);
    text6(STATUS_X, 0, s, 5);
    btn(BTN_STOP, s == s_stop || s == s_states[BUS_STATE_ENDED] || s == s_states[BUS_STATE_FAILED] ? A_RED : A_DECK);
    btn(BTN_PLAY, s == s_states[BUS_STATE_PLAYING] ? A_GREEN : s == s_states[BUS_STATE_LOADING] || s == s_pause ? A_YELLOW : A_DECK);
}

/* Формат: до n (не больше 4) символов s или до нуля. */
static void draw_format(const char *s, u8 n)
{
    clear6(0, FORMAT_X / 8u, 4, 0);
    text6(FORMAT_X, 0, s, n);
}

/* Размер играющего файла - в правом углу строки типа: шесть знаков,
   четыре под число и два под единицу (" 450KB", "1.25MB", "11.5MB"),
   чтобы не скакал. Рисуется раз на файл (ui_set_file); строка типа его
   угол не трогает: ей байты 0..22, размеру 23..27. */
#define SIZE_X      186
#define SIZE_COL    23
static char s_size_text[7];

static void type_line(const char *s)
{
    clear6(TYPE_ROW, 0, SIZE_COL, 0x80);
    text6(2, TYPE_ROW, s, (SIZE_X - 2) / 6);
}

/* -- Строка инфо --
 *
 * Пункты по кругу, пустые пропускаются. Короткий текст стоит 5 секунд,
 * длинный прокручивается до конца и стоит секунду.
 */

enum { IT_TITLE, IT_AUTHOR, IT_GAME, IT_SYSTEM, IT_DATE, IT_PROGRAM, IT_FORMAT, IT_FILE, IT_COUNT,
       IT_BOARD = IT_COUNT, IT_PLAYER, IT_KEYS, IT_ALARM };

static const char *const s_labels[] = {
    "TITLE", "AUTHOR", "GAME", "SYSTEM", "DATE", "PROGRAM", "FORMAT", "FILE",
    "BOARD TYPE", "PLAYER", "HOTKEY", "ERROR",
};

/* Клавиши по одной на пункт: [Спектрум] [USB], общая - одна. */
static const char *const s_keys[] = {
    "PLAY [ENTER]",
    "PARENT FOLDER [CS+0] [BACKSPACE]",
    "NEXT TRACK [SPACE]",
    "PAUSE [P]",
    "FORWARD [F]",
    "REPEAT MODE [M]",
    "UP/DOWN [CS+7/CS+6] [UP/DOWN]",
    "PAGE UP/DOWN [CS+5/CS+8] [LEFT/RIGHT]",
};
#define KEYS_N (sizeof(s_keys) / sizeof(s_keys[0]))

/* Круг до первого файла - PLAYER, BOARD, клавиши по одной; первым PLAYER:
   ui_init зовётся до опроса платы, и BOARD показал бы NO BOARD, пока она не
   опрошена. */
static u8 s_item = IT_KEYS;
static u8 s_key = KEYS_N - 1;   /* клавиша пункта IT_KEYS */
static const char *s_text;      /* текст пункта */
static u8 s_text_len;
static u8 s_ci, s_gk;           /* символ и столбец глифа следующего столбца точек */
static u8 s_in, s_out;          /* шагов подъёма: въезд и уход */
static volatile bool_t s_paused;    /* главный цикл готовит пункт */
static volatile bool_t s_need_next; /* пункт кончился, нужен следующий */
static u16 s_shifts;            /* сдвигов влево: хвост длинного пункта */
static u16 s_timer;
static u8 s_acc;                /* кадров с прошлого шага */

static const char *s_alarm;
static const char *s_file;     /* играющий файл; 0 - ещё не запускали */
static u32 s_file_size;
static u8 s_seq;                /* bus_track_seq, по которому собраны метаданные */

/* Следующий столбец точек: биты 0..6 - строки шрифта 1..7. Пробел - 3
   столбца, символ - 5 и зазор. */
static u8 next_column(void)
{
    u8 c, w, r, bits = 0;
    const u8 *g;
    if (s_ci >= s_text_len) return 0;
    c = (u8)s_text[s_ci];
    w = c == ' ' ? 3 : 6;
    if (s_gk < 5 && c != ' ') {
        const u8 m = (u8)(0x80u >> s_gk);
        g = fw_font + 6 * 256 + c;
        for (r = 0; r < 7; ++r, g -= 256) {
            bits <<= 1;
            if (*g & m) bits |= 1;
        }
    }
    if (++s_gk >= w) { s_gk = 0; ++s_ci; }
    return bits;
}

static u16 text_cols(void)
{
    u8 i;
    u16 n = 0;
    for (i = 0; i < s_text_len; ++i) n += s_text[i] == ' ' ? 3u : 6u;
    return n;
}

/* Подписи параметров трека (кадр 0x02): MIDI и трекер. */
static const char *const s_params_mid[] = {" TRK ", " PPQ ", " BPM", 0};
static const char *const s_params_trk[] = {" CH SPD ", " ", " BPM ", " POS", 0};

/* Текст пункта; FALSE - пункт пустой. */
static bool_t item_text(u8 it)
{
    char *d = HI->text;
    s_text = HI->text;
    switch (it) {
    case IT_BOARD:
        d = put_str(d, "\x0f ");
        d = put_str(d, bus_board_found ? bus_board_name : "NO BOARD");
        break;
    case IT_PLAYER:
        d = put_str(d, "\x0e UGLY PLAYER 0.5");
        break;
    case IT_KEYS:
        d = put_str(d, s_keys[s_key]);
        break;
    case IT_ALARM:
        d = put_str(d, s_alarm);
        break;
    case IT_FORMAT: {
        /* Числа BCD по 4 байта, за каждым подпись. */
        const char *const *f = HI->klass == 2 ? s_params_mid : s_params_trk;
        const u8 *p = HI->params;
        if (HI->params_len < 12) return FALSE;
        for (; *f; ++f, p += 4) {
            d = put_bcd(d, p);
            d = put_str(d, *f);
        }
        break;
    }
    case IT_FILE:
        d = put_str(d, s_file);
        d = put_str(d, "  ");
        d = put_u32(d, s_file_size);
        break;
    default:
        /* Пустые и односимвольные теги - мимо круга. */
        s_text = TAG(it);
        s_text_len = HI->tag_len[it];
        while (s_text_len && s_text[s_text_len - 1] == ' ') --s_text_len;
        return (bool_t)(s_text_len > 1);
    }
    *d = 0;
    s_text_len = (u8)(d - HI->text);
    /* BOARD и PLAYER - заглавными. */
    if (it == IT_BOARD) {
        for (d = HI->text; *d; ++d) if (*d >= 'a' && *d <= 'z') *d = (char)(*d - 32);
    }
    return TRUE;
}

/* Перерисовка строки инфо - сразу после прерывания кадра: луч ещё в
   бордюре и до y = 35 не дойдёт, пока строка рисуется, - без разрыва. */

/* Пункт поднимается снизу (ряд точек за LIFT_FRAMES кадров) и стоит:
   короткий 5 секунд, длинный 3. Хвост длинного въезжает справа (точка за 2
   кадра), потом 3 секунды. Уходит пункт вверх, так же по ряду. */
#define DOTS_COLS   74
#define LIFT_FRAMES 3

static void start_item(u8 it)
{
    u16 cols;
    u8 col;
    s_paused = TRUE;
    s_item = it;
    item_text(it);
    s_ci = 0;
    s_gk = 0;
    /* Видимая часть - в буфер сразу: подъём рисует её рядами. */
    dots_clear();
    for (col = 0; col < DOTS_COLS; ++col) dots_put(col, next_column());
    cols = text_cols();
    s_in = 7;
    s_out = 7;
    s_shifts = cols > DOTS_COLS ? cols - DOTS_COLS : 0;
    s_timer = cols > DOTS_COLS ? 150 : 250;
    s_acc = LIFT_FRAMES - 1;    /* первый ряд - в следующем кадре */
    s_need_next = FALSE;
    s_paused = FALSE;
    type_line(s_labels[it]);
}

/* Следующий непустой пункт круга. */
static void next_item(void)
{
    u8 it = s_item, n;
    s_paused = TRUE;            /* item_text меняет текст, который читает кадр */
    if (s_alarm) { start_item(IT_ALARM); return; }
    /* Играть нечего - до первого файла и когда трек доиграл или не принят, а
       следующий не запущен: PLAYER, BOARD, клавиши по одной и снова. */
    if (!s_file || !bus_session_started || bus_state == BUS_STATE_ENDED || bus_state == BUS_STATE_FAILED) {
        if (it == IT_PLAYER) it = IT_BOARD;
        else if (it == IT_BOARD) { s_key = 0; it = IT_KEYS; }
        else if (it == IT_KEYS && s_key + 1u < KEYS_N) ++s_key;
        else it = IT_PLAYER;
        start_item(it);
        return;
    }
    if (it >= IT_COUNT) it = IT_COUNT - 1;
    for (n = 0; n < IT_COUNT; ++n) {
        it = (u8)((it + 1) % IT_COUNT);
        if (item_text(it)) break;
    }
    start_item(it);
}

/* Кадр строки инфо - из прерывания, сразу за обратным ходом луча: шаг
   (до 16 тысяч тактов) готов раньше, чем луч дойдёт до y = 35. Главный цикл
   пункт не рисует, а только готовит; пока готовит - s_paused. */
static void info_frame(void)
{
    if (s_paused || s_need_next) return;
    ++s_acc;
    if (s_in) {
        if (s_acc < LIFT_FRAMES) return;
        s_acc = 0;
        dots_draw((u8)(8 - s_in));
        --s_in;
        return;
    }
    if (s_timer) { --s_timer; s_acc = 0; return; }
    if (s_shifts) {
        if (s_acc < 2) return;
        s_acc = 0;
        dots_hscroll(next_column());
        if (--s_shifts == 0) s_timer = 150; /* хвост дочитать: 3 секунды */
        return;
    }
    if (s_out) {
        if (s_acc < LIFT_FRAMES) return;
        s_acc = 0;
        dots_lift();
        --s_out;
        return;
    }
    s_need_next = TRUE;         /* следующий пункт готовит главный цикл */
}

/* -- Метаданные трека (0x1B) -- */

static u8 s_line_type, s_line_next;
static bool_t s_chips_dirty;

static void meta_clear(void)
{
    u8 i;
    for (i = 0; i < TAGS; ++i) HI->tag_len[i] = 0;
    for (i = 0; i < CHIPS; ++i) *CHIP(i) = 0;
    HI->params_len = 0;
    HI->klass = 0;
    s_line_type = 0;
    s_chips_dirty = TRUE;
}

void plat_track_meta(const u8 *frame)
{
    u8 type = (u8)(frame[0] & 0x7F), blk = (u8)(frame[1] >> 4), len = (u8)((frame[1] & 0x0F) + 1);
    const u8 *data = frame + 2;
    u8 off, i, slot;

    /* Новый трек начался кадром 0x15 - прежние метаданные долой. */
    if (s_seq != bus_track_seq) { s_seq = bus_track_seq; meta_clear(); }

    /* Блоки строки идут по порядку, последний - номер 15: его место - за
       предыдущим. Повтор кадра ложится на то же место. */
    if (type != s_line_type) { s_line_type = type; s_line_next = 0; }
    if (blk != 15) { off = (u8)(blk * 16u); s_line_next = (u8)(blk + 1); }
    else off = (u8)(s_line_next * 16u);

    if (type == 0x01) {
        draw_format((const char *)data, 4);
        HI->klass = data[4];
    } else if (type == 0x02) {
        for (i = 0; i < len; ++i) HI->params[i] = data[i];
        HI->params_len = len;
    } else if ((type >= 0x10 && type <= 0x14) || type == 0x17) {
        slot = type == 0x17 ? 5 : (u8)(type - 0x10);
        {
            char *d = TAG(slot) + off;
            for (i = 0; i < len && off + i < TAG_LEN; ++i) *d++ = (char)data[i];
        }
        if ((u8)(off + i) > HI->tag_len[slot]) HI->tag_len[slot] = (u8)(off + i);
        if (slot == 5) s_chips_dirty = TRUE;
    } else if (type >= 0x20 && type < 0x20 + CHIPS) {
        slot = (u8)(type - 0x20);
        {
            char *d = CHIP(slot);
            for (i = 0; i < 11 && data[i]; ++i) *d++ = (char)data[i];
            *d = 0;
        }
        HI->chip_cnt[slot] = data[15];
        s_chips_dirty = TRUE;
    }
    /* Последний кадр - круг строки инфо заново, с названия. */
    if (frame[0] & 0x80) {
        if (!s_alarm) { s_item = IT_FILE; next_item(); }
    }
}

/* -- Строка чипов: чипы VGM, программа трекера. Не влезла - сдвиг на
   пиксель за 50 кадров до упора, потом назад. -- */

/* Сдвиг s_chip_off = 6 * s_chip_ch + s_chip_px: символ и пиксель в нём. */
static u8 s_chip_len, s_chip_off, s_chip_ch, s_chip_px, s_chip_max, s_chip_timer;
static bool_t s_chip_back;

static void chips_draw(void)
{
    u8 x = (u8)(CHIP_X - s_chip_px);
    clear6(CHIP_ROW, 8, 20, 0);
    /* Символов до x = 224: (224 - x) / 6. */
    if (s_chip_len) text6(x, CHIP_ROW, HI->chips + s_chip_ch, (u8)(s_chip_px < 4 ? 25 : 26));
    clear6(CHIP_ROW, 8, 1, 0);  /* левый край: часть глифа левее x = 72 */
}

static void chips_build(void)
{
    char *d = HI->chips;
    u8 i;
    s_chips_dirty = FALSE;
    if (HI->klass == 3) {
        for (i = 0; i < CHIPS; ++i) {
            if (!*CHIP(i) || (u8)(d - HI->chips) > 48) continue;
            if (d != HI->chips) *d++ = ' ';
            if (HI->chip_cnt[i] > 1) { *d++ = (char)('0' + HI->chip_cnt[i]); *d++ = 'x'; }
            d = put_str(d, CHIP(i));
        }
    } else if (HI->klass == 1) {
        {
            const char *t = TAG(5);
            for (i = 0; i < HI->tag_len[5] && i < 40; ++i) *d++ = *t++;
        }
    }
    *d = 0;
    s_chip_len = (u8)(d - HI->chips);
    s_chip_max = (u16)s_chip_len * 6u > CHIP_W ? (u8)((u16)s_chip_len * 6u - CHIP_W) : 0;
    s_chip_off = 0;
    s_chip_ch = 0;
    s_chip_px = 0;
    s_chip_back = FALSE;
    s_chip_timer = 0;
    chips_draw();
}

static void chips_tick(u8 frames)
{
    if (s_chips_dirty) chips_build();
    if (!s_chip_max) return;
    s_chip_timer = (u8)(s_chip_timer + frames);
    if (s_chip_timer < 50) return;
    s_chip_timer = 0;
    if (s_chip_back) {
        if (s_chip_px-- == 0) { s_chip_px = 5; --s_chip_ch; }
        if (--s_chip_off == 0) s_chip_back = FALSE;
    } else {
        if (++s_chip_px == 6) { s_chip_px = 0; ++s_chip_ch; }
        if (++s_chip_off >= s_chip_max) s_chip_back = TRUE;
    }
    chips_draw();
}

/* -- VU и спектр рисуют vu_tick и spec_tick (dots.s) -- */

/* Раз в кадр из прерывания (fw_irq_hook): строка инфо первой - она ниже
   спектра и VU и должна успеть раньше луча; потом VU и спектр. Главный
   цикл в эти места экрана не пишет. */
static void ui_frame(void)
{
    info_frame();
    if (bus_session_started) {
        vu_tick();
        spec_tick();
    }
}

/* -- Состояние на экране: рисуем только изменившееся -- */

static const char *s_status;
static u8 s_flash_col, s_flash_end; /* горящая кнопка (0 - нет) и кадр гашения */
static u8 s_clock[4];
static u8 s_cpu = 0xFF, s_ch = 0xFF;
static u8 s_solid = 0xFF, s_dotted = 0xFF;
static bool_t s_lamp;

void ui_init(void)
{
    u8 y;
    dots_init();
    unpack_deck();
    draw_status(s_stop);
    s_status = s_stop;
    draw_clock(0, 0, 0, 0);
    s_clock[0] = 0xFF;          /* минут 0xFF не бывает: первый кадр рисует часы */
    ui_set_repeat(0);
    for (y = 18; y <= 21; ++y) { *scr(12, y) |= 0x01; *scr(13, y) |= 0x80; } /* метка центра VU, x = 103..104 */
    /* Бордюр белый. */
    __asm__("ld a, #7\n out (#0xFE), a");
    meta_clear();
    next_item();
    fw_irq_hook = ui_frame;
}

/* Дописать s в ui6_buf с позиции n, не больше чем до max; новая длина. */
static u8 buf_put(u8 n, const char *s, u8 max)
{
    char *d = ui6_buf + n;
    while (n < max && *s) { *d++ = *s++; ++n; }
    return n;
}

void ui_draw_path(const char *path, u8 len)
{
    u8 n;
    if (len <= UI_PATH_CHARS) {
        n = buf_put(0, path, UI_PATH_CHARS);
    } else {
        /* Длиннее строки - хвост, первые три знака - точки. */
        n = buf_put(0, path + len - UI_PATH_CHARS, UI_PATH_CHARS);
        ui6_buf[0] = ui6_buf[1] = ui6_buf[2] = '.';
    }
    ui_line6(PATH_ROW, n);
}

void ui_draw_entry(u8 i, const char *name, u8 is_dir, u8 mark)
{
    u8 n = 0;

    /* Каталог - в скобках: цвет занят курсором. */
    if (is_dir) ui6_buf[n++] = '[';
    n = buf_put(n, name, (u8)(UI_PATH_CHARS - (is_dir ? 1 : 0)));
    if (is_dir) ui6_buf[n++] = ']';
    ui_line6((u8)(LIST_ROW + i), n);
    ui_entry_attr(i, mark);
}

/* Цвет по mark: нет, курсор, играет, курсор на играющем. */
static const u8 s_mark_attr[4] = {A_PANEL, A_CURSOR, A_PLAYING, A_CURSOR_PLAYING};

void ui_entry_attr(u8 i, u8 mark)
{
    u8 attr = s_mark_attr[mark];
    u8 *a = &ATTR(LIST_ROW + i, LIST_COL);
    u8 c;
    for (c = 0; c < LIST_COLS; ++c) *a++ = attr;
}

void ui_scroll_list(bool_t down)
{
    u8 i, line, dst, src;
    u8 *d, *s;
    for (i = 0; i < UI_LIST_ROWS - 1; ++i) {
        /* Вниз - строки едут вверх, копировать сверху; вверх - снизу. */
        dst = (u8)(LIST_ROW + (down ? i : UI_LIST_ROWS - 1 - i));
        src = down ? (u8)(dst + 1) : (u8)(dst - 1);
        /* Строки точек 1..7 знакоместа: следующая - на 256 дальше. */
        d = scr(LIST_COL, (u8)(dst * 8u + 1));
        s = scr(LIST_COL, (u8)(src * 8u + 1));
        for (line = 1; line < 8; ++line, d += 256, s += 256) memcpy(d, s, LIST_COLS);
        memcpy(&ATTR(dst, LIST_COL), &ATTR(src, LIST_COL), LIST_COLS);
    }
}

/* Четыре знака числа v < 10000 вправо, ведущие - пробелы; точка - перед
   знаком с номером dot (0 - без точки), она занимает его место слева. */
static void size_digits(u16 v, u8 dot)
{
    char *d = s_size_text + 3;
    u8 i;
    for (i = 0; i < 4; ++i, --d) {
        if (dot && i == dot) { *d = '.'; continue; }
        *d = (char)('0' + v % 10u);
        v /= 10u;
        if (v == 0 && (!dot || i >= dot)) { while (d > s_size_text) *--d = ' '; break; }
    }
}

static void size_format(u32 size)
{
    const u32 kb = (size + 1023u) >> 10;
    if (kb < 1000u) {
        size_digits((u16)kb, 0);
        s_size_text[4] = 'K';
    } else {
        /* Сотые МБ: 1048576 / 100 = 10485.76. */
        const u32 h = size / 10486u;
        if (h < 1000u) size_digits((u16)h, 2);                      /* 1.25 */
        else if (h < 10000u) size_digits((u16)(h / 10u), 1);        /* 11.5 */
        else size_digits((u16)(h / 100u > 9999u ? 9999u : h / 100u), 0);
        s_size_text[4] = 'M';
    }
    s_size_text[5] = 'B';
    s_size_text[6] = 0;
}

void ui_set_file(const char *name, u32 size)
{
    const char *dot = 0, *p;

    for (p = name; *p; ++p) if (*p == '.') dot = p;
    /* Формат по расширению, пока плата не прислала метаданные. */
    draw_format(dot ? dot + 1 : "", 3);
    s_file = name;
    s_file_size = size;
    size_format(size);
    clear6(TYPE_ROW, SIZE_COL, 28 - SIZE_COL, 0);
    text6(SIZE_X, TYPE_ROW, s_size_text, 6);
    s_alarm = 0;
    meta_clear();
    s_seq = bus_track_seq;
    s_item = IT_PROGRAM;        /* следующим - FILE */
    next_item();
}

void ui_set_repeat(u8 mode)
{
    const u8 *p = ui_repeat + (u16)mode * 16u + 2;
    u8 y;
    /* Строка 0 значка пустая, на y = 0 рамка. */
    for (y = 1; y < 8; ++y) {
        *scr(REPEAT_COL, y) = *p++;
        *scr(REPEAT_COL + 1, y) = *p++;
    }
}

void ui_flash(u8 what)
{
    if (s_flash_col) btn(s_flash_col, A_DECK);
    s_flash_col = what == UI_FLASH_REPEAT ? BTN_REPEAT : BTN_FWD;
    s_flash_end = (u8)((u8)fw_ticks + FLASH_FRAMES);
    btn(s_flash_col, what == UI_FLASH_FORWARD ? A_YELLOW : A_GREEN);
}

void ui_alarm(const char *text)
{
    s_alarm = text;
    start_item(IT_ALARM);
}

void ui_update(void)
{
    /* Не больше раза в кадр; пропущенные кадры анимация догоняет. */
    static u8 drawn_frame;
    u8 now = (u8)fw_ticks;
    u8 frames = (u8)(now - drawn_frame);
    const char *st;

    if (!frames) return;
    drawn_frame = now;
    if (frames > 50) frames = 50;

    if (bus_board_found != s_lamp) {
        s_lamp = bus_board_found;
        ATTR(7, 1) = s_lamp ? A_LAMP_ON : A_LAMP_OFF;
    }

    if (s_need_next) next_item();
    chips_tick(frames);

    /* По кадру гашения, не по frames: тот считает и кадры до вспышки. */
    if (s_flash_col && (u8)(now - s_flash_end) < 0x80) {
        btn(s_flash_col, A_DECK);
        s_flash_col = 0;
    }

    st = !bus_session_started ? s_stop : bus_state == BUS_STATE_PLAYING && bus_paused ? s_pause : s_states[bus_state];
    if (st != s_status) {
        s_status = st;
        draw_status(st);
    }

    if (bus_file_info_valid) {
        u8 dm = bus_dur_valid ? bus_dur_min : 0, ds = bus_dur_valid ? bus_dur_sec : 0;
        u8 solid = s_solid, dotted;

        /* Позиция, длительность и доля полосы (83 шага bar_frac) - только при
           смене часов, раз в секунду. */
        if (s_clock[0] != bus_pos_min || s_clock[1] != bus_pos_sec || s_clock[2] != dm || s_clock[3] != ds) {
            s_clock[0] = bus_pos_min; s_clock[1] = bus_pos_sec; s_clock[2] = dm; s_clock[3] = ds;
            draw_clock(bus_pos_min, bus_pos_sec, dm, ds);
            solid = bar_frac((u16)bus_pos_min * 60u + bus_pos_sec, (u16)dm * 60u + ds);
        }
        /* Штриховка - загружено сэмплов; без сэмплов сразу полная. */
        if (bus_samples == 0) {
            dotted = BAR_W;
        } else {
            /* Деление - только при смене счётчика сэмплов. */
            static u16 s_bar_ld = 0xFFFFu, s_bar_tt;
            static u8 s_bar_dotted;
            u16 ld = bus_psram_valid ? bus_psram_loaded : 0u, tt = bus_psram_valid ? bus_psram_total : 0u;
            if (ld != s_bar_ld || tt != s_bar_tt) {
                s_bar_ld = ld;
                s_bar_tt = tt;
                s_bar_dotted = bar_frac(ld, tt);
            }
            dotted = s_bar_dotted;
        }
        if (solid != s_solid || dotted != s_dotted) {
            s_solid = solid;
            s_dotted = dotted;
            draw_bar(solid, dotted);
        }
    }

    if (bus_load_valid) {
        if (bus_cpu != s_cpu) {
            u8 a = bus_cpu >= 90 ? A_CPU_HOT : A_PANEL;
            s_cpu = bus_cpu;
            pair4(CPU_COL, bus_cpu);
            ATTR(1, 10) = a; ATTR(1, 11) = a; ATTR(1, 12) = a;
        }
        if (bus_voices != s_ch) {
            s_ch = bus_voices;
            pair4(CH_COL, bus_voices);
        }
    }
}
