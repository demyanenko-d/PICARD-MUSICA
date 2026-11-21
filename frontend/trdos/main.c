/*
 * main.c - навигатор по карте и плеер.
 *
 * Верхний уровень - устройства карты: Z-Controller и две карты DivMMC. Вход
 * в устройство поднимает его карту и том; трек, открытый на другом, играет
 * дальше - файл читается со своего устройства.
 *
 * Экран поделён надвое: сверху дека плеера, снизу список файлов. Обе половины живут одновременно, следующий трек
 * выбирается не прерывая текущий.
 *
 * Звук делает плата, сюда приходят только запросы на данные. Главный цикл
 * обслуживает две очереди событий: нажатия и запросы платы.
 */

#include "fw.h"
#include "ui.h"
#include "../common/bus_client.h"

/**
 * Ответ на BUS_EV_READ: отдать len байт с offset и подтвердить. Не
 * прочиталось - bus_nak, плата попросит то же окно.
 */
void plat_send_data(u32 offset, u16 len);

/** Ответ на BUS_EV_READ_FAST: то же, но ровно BUS_WINDOW_BYTES байт. */
void plat_send_data_fast(u32 offset);

/* -- Навигатор -- */

/* Верхний уровень навигатора. */
#define DEV_NONE 0xFF
static const char *const s_dev_name[FW_DEV_COUNT] = { "ZC", "DIVMMC1", "DIVMMC2" };
static u8 s_dev = DEV_NONE;     /* чей том смонтирован */
static bool_t s_in_dev;         /* показан каталог устройства, а не их список */
/* 1 - в корне устройства: первая строка - ".." к списку устройств, у
   корня FAT своей нет. Записи таблицы каталога сдвинуты на неё. */
static u8 s_up;

static u16 s_count;             /* сколько записей в текущем каталоге */
static u16 s_cursor;            /* какая выбрана */
static u16 s_top;               /* какая первая на экране */
static u8  s_depth;             /* насколько глубоко ушли от корня */

/* Кластеры каталогов на пути от корня: по ним возвращаемся назад.
   Восьми уровней хватает: глубже на музыкальной карте не забираются. */
#define MAX_DEPTH 8
static u32 s_path[MAX_DEPTH];

/* Тот же путь строкой "/имя/имя/" для показа; s_path_end[d] - её длина на
   глубине d. От имени каталога - до NAME_KEEP - 1 знаков: в строку
   влезает 40, длинные всё равно не показать целиком, а прошивка отдаёт до
   260 байт. */
#define NAME_KEEP 16
static char s_path_str[MAX_DEPTH * NAME_KEEP + 2];
static u8 s_path_end[MAX_DEPTH];

/* Первая показываемая запись: "." - ссылка каталога на самого себя,
   смотреть в ней нечего. Прошивка её отдаёт намеренно, решать нам. */
static u16 s_first;

static char s_playing[32];      /* имя того, что играет */
static u16  s_play_idx;         /* какая запись играет - для автоперехода */
static bool_t s_play_active;    /* есть чему доигрывать */

/* Где лежит играющее: кластер каталога и признак, что оно есть. Нужно для
   подсветки, и живёт дольше s_play_active - тот про автопереход и гаснет
   при смене каталога, а подсветка обязана вернуться, когда вернулись. */
static u32 s_play_dir;
static u8 s_play_dev;
static bool_t s_play_marked;
static bool_t s_here;           /* играющее - в показанном каталоге */

static u32  s_idle;             /* пустых кругов подряд, пока не заиграло */

/* Режим повтора, клавиша M: всё по кругу, один трек, шафл, каталог один
   раз и стоп. */
#define REP_ALL     0
#define REP_ONE     1
#define REP_SHUFFLE 2
#define REP_ONCE    3
static u8 s_repeat;

/* Шафл как в магнитоле: порядок случайный, без повторов до конца круга.
   Регистр сдвига с полным периодом обходит 1..2^n-1 по разу; номер записи -
   значение минус один, лишние пропускаются. */
static u16 s_lfsr, s_lfsr_taps;
static const u16 s_taps[] = { 0x3, 0x5, 0x9, 0x12, 0x21, 0x41, 0x8E, 0x108, 0x204, 0x402, 0x829, 0x100D, 0x2015 };

/* Сколько пустых кругов главного цикла на загрузке терпим, пока плата не
   сказала BUS_STATE_FAILED: прошивка без этого состояния, провалив разбор
   файла, просто сбрасывается, и признак отказа - только тишина. */
#define SESSION_TRIES 2000000ul

/* Имя в dst, не больше max - 1 знаков и ноль; возвращает место нуля. */
static char *copy_name(char *dst, const char *src, u8 max)
{
    while (*src && --max) *dst++ = *src++;
    *dst = 0;
    return dst;
}

/* -- Каталог -- */

/** Запись под курсором - это ".."? */
static bool_t ent_is_updir(void)
{
    return (bool_t)(fat_ent_name[0] == '.' && fat_ent_name[1] == '.' && fat_ent_name[2] == 0);
}

/** Запись idx списка в fat_ent_*. */
static void ent_get(u16 idx)
{
    if (idx < s_up) {
        fat_ent_name[0] = '.';
        fat_ent_name[1] = '.';
        fat_ent_name[2] = 0;
        fat_ent_attr = FW_ATTR_DIR;
        fat_ent_clus = 0;
        return;
    }
    fw_dir_get(idx - s_up);
}

/** Кластер текущего каталога; у корня нулевой. */
static u32 cur_dir_clus(void)
{
    return s_depth ? s_path[s_depth - 1] : 0;
}

/* Ответить на запрос данных; остальные события - телеметрия, её уже
   разобрал bus_client. */
static void serve(u8 ev)
{
    if (ev == BUS_EV_READ_FAST) plat_send_data_fast(bus_req_offset);
    else if (ev == BUS_EV_READ) plat_send_data(bus_req_offset, bus_req_length);
}

/* Опрос платы изнутри долгой работы: чтение каталога, сортировка,
   отрисовка списка. Не чаще раза в кадр, без ожидания: что плата
   выставила, то и разобрать, затем экран. reads == FALSE - карта занята
   каталогом, запрос чтения ждёт. Конец трека разбирает главный цикл:
   здесь карту под новый файл не открыть. */
static void bus_service(bool_t reads)
{
    static u8 last;
    u8 n, ev;
    if ((u8)fw_ticks == last || !bus_board_found) return;
    last = (u8)fw_ticks;
    for (n = 0; n < 4; ++n) {
        ev = bus_poll_now(reads);
        if (ev == BUS_EV_NONE) break;
        serve(ev);
    }
    ui_update();
}

/* Вызов из dirsort.s, не чаще раза в кадр. Карта свободна; отдача данных
   переключает страницу, поэтому страница каталога включается заново. */
void dir_sort_tick(void)
{
    bus_service(TRUE);
    fw_dir_page(fat_page_dir);
}

/* Вызов из цикла чтения каталога (dir.s, _fat_dir_hook). */
static void dir_load_tick(void)
{
    bus_service(FALSE);
}

/* -- Список -- */

/* Цвет записи idx: курсор и играющее. */
static u8 mark(u16 idx)
{
    return (u8)((idx == s_cursor ? UI_MARK_CURSOR : 0) | (s_here && idx == s_play_idx ? UI_MARK_PLAYING : 0));
}

/* Видимая строка с записью idx; за концом каталога - пустая. */
static void row_draw(u16 idx)
{
    const u8 row = (u8)(idx - s_top);
    if (idx >= s_count) {
        ui_draw_entry(row, "", 0, 0);
        return;
    }
    if (!s_in_dev) {
        ui_draw_entry(row, s_dev_name[idx], 1, mark(idx));
        return;
    }
    ent_get(idx);
    ui_draw_entry(row, fat_ent_name, (u8)((fat_ent_attr & FW_ATTR_DIR) != 0), mark(idx));
}

/* Цвет записи idx, если она на экране: текст там уже верный. */
static void row_attr(u16 idx)
{
    if (idx >= s_top && idx < s_top + UI_LIST_ROWS) ui_entry_attr((u8)(idx - s_top), mark(idx));
}

static void list_draw(void)
{
    u16 idx;
    for (idx = s_top; idx < s_top + UI_LIST_ROWS; ++idx) {
        row_draw(idx);
        bus_service(TRUE);
    }
}

/* Курсор на запись idx. Окно стоит - перекрасить две строки; сдвинулось на
   строку - сдвинуть экран и нарисовать открывшуюся; иначе весь список. */
static void cursor_to(u16 idx)
{
    const u16 old = s_cursor, top = s_top;
    if (idx >= s_count) return;

    s_cursor = idx;
    if (s_cursor < s_top) s_top = s_cursor;
    if (s_cursor >= s_top + UI_LIST_ROWS) s_top = s_cursor - UI_LIST_ROWS + 1;
    if (s_top == top + 1 || s_top + 1 == top) {
        ui_scroll_list((bool_t)(s_top > top));
        row_draw(s_top > top ? s_top + UI_LIST_ROWS - 1 : s_top);
    } else if (s_top != top) {
        list_draw();
        return;
    }
    row_attr(old);
    row_attr(s_cursor);
}

/* Страница вперёд или назад: окно и курсор сдвигаются на UI_LIST_ROWS, курсор
   остаётся в той же строке экрана. У края списка окно упирается, курсор
   встаёт на крайнюю запись. */
static void page_move(bool_t down)
{
    u16 c, t;

    if (s_count <= s_first) return;
    if (down) {
        c = s_cursor + UI_LIST_ROWS;
        if (c >= s_count) c = s_count - 1u;
        t = s_top + UI_LIST_ROWS >= s_count ? s_top
          : s_top + UI_LIST_ROWS * 2u > s_count ? s_count - UI_LIST_ROWS
          : s_top + UI_LIST_ROWS;
    } else {
        c = s_cursor >= s_first + UI_LIST_ROWS ? s_cursor - UI_LIST_ROWS : s_first;
        t = s_top >= s_first + UI_LIST_ROWS ? s_top - UI_LIST_ROWS : s_first;
    }
    if (t == s_top || c < t || c >= t + UI_LIST_ROWS) { cursor_to(c); return; }
    s_top = t;
    s_cursor = c;
    list_draw();
}

/* -- Сортировка списка --
 *
 * Каталоги вперёд, дальше по имени без учёта регистра (dirsort.s).
 * Переставляется только таблица указателей: записи лежат в своей
 * странице, указатель два байта против десятков у записи. Сортируется один
 * раз при загрузке каталога, не при перерисовке.
 */

void dir_sort_ptrs(u16 n);

static void dir_sort(void)
{
    const u8 saved = (u8)(fat_port_shadow & 0x07);
    if (fat_dir_count < 2) return;
    fw_dir_page(fat_page_dir);
    dir_sort_ptrs(fat_dir_count);
    fw_dir_page(saved);
}

static void list_reload(void)
{
    s_lfsr_taps = 0;            /* круг шафла - по новому каталогу */
    if (fw_dir_load()) {
        ui_alarm("dir read error");
        s_count = 0;
        return;
    }
    dir_sort();   /* до любого fw_dir_get: он читает уже упорядоченный список */
    s_up = (u8)(s_depth == 0);
    s_count = fat_dir_count + s_up;

    /* "." в FAT всегда первая, но проверяем явно, а не по порядку. Прятать
       её достаточно тем, что окно списка ниже неё не опускается. */
    s_first = 0;
    if (s_count) {
        ent_get(0);
        if (fat_ent_name[0] == '.' && fat_ent_name[1] == 0) s_first = 1;
    }
    s_cursor = s_first;
    s_top = s_first;
    /* Играющее подсвечивается и очередь идёт только в своём каталоге:
       номер записи в чужом означает чужой файл. Ушли - автопереход
       стоит, трек на плате доигрывает; вернулись - очередь продолжается,
       а доигравший без нас трек сменится сразу (bus_state ENDED). */
    s_here = (bool_t)(s_play_marked && s_play_dev == s_dev && cur_dir_clus() == s_play_dir);
    s_play_active = s_here;
    list_draw();
}

/* Путь и список открытого каталога глубины s_depth. */
static void show_dir(void)
{
    const u8 len = s_path_end[s_depth];
    s_path_str[len] = 0;
    ui_draw_path(s_path_str, len);
    list_reload();
}

/* Открыть каталог глубины s_depth заново от корня и показать. Хранить
   открытым только текущий каталог дешевле, чем держать состояние обхода на
   каждый уровень; каталогов на пути единицы, лишние чтения незаметны. */
static bool_t reopen(void)
{
    u8 i;
    if (fw_dir_open_root()) {
        ui_alarm("no root");
        return FALSE;
    }
    for (i = 0; i < s_depth; ++i) {
        fat_ent_clus = s_path[i];
        if (fw_dir_open_ent()) {
            ui_alarm("dir open error");
            return FALSE;
        }
    }
    show_dir();
    return TRUE;
}

/** Открыть корень устройства и показать его. Путь - "/ИМЯ/". */
static void go_root(void)
{
    char *end;
    s_depth = 0;
    s_path_str[0] = '/';
    end = copy_name(s_path_str + 1, s_dev_name[s_dev], NAME_KEEP);
    *end = '/';
    s_path_end[0] = (u8)(end + 1 - s_path_str);
    (void)reopen();
}

/** Список устройств, курсор на устройстве cur. */
static void show_devices(u8 cur)
{
    s_in_dev = FALSE;
    s_play_active = FALSE;
    s_here = FALSE;
    s_count = FW_DEV_COUNT;
    s_first = 0;
    s_cursor = cur < FW_DEV_COUNT ? cur : 0;
    s_top = 0;
    ui_draw_path("/", 1);
    list_draw();
}

/**
 * Войти в устройство: поднять карту и том, если смонтирован чужой, и
 * показать корень. Играющий трек с другого устройства не прерывается.
 */
static void open_dev(u8 dev)
{
    if (dev != s_dev) {
        s_dev = DEV_NONE;
        sd_use(dev);
        if (fw_sd_init()) { ui_alarm("no card"); return; }
        if (fw_mount())   { ui_alarm("no fat32"); return; }
        s_dev = dev;
    }
    s_in_dev = TRUE;
    go_root();
}

/** Войти в каталог под курсором. */
static void go_into(void)
{
    if (s_depth + 1 >= MAX_DEPTH) return;

    /* Нулевой кластер - не каталог: так в FAT32 записан ".." первого
       уровня, и означает он корень. Ходить наверх - дело go_up. */
    if (fat_ent_clus == 0) { ui_alarm("bad dir"); return; }

    /* Запомнить, откуда пришли: у FAT в подкаталоге есть "..", но у корня
       его нет, и вести учёт самим надёжнее, чем разбирать особый случай.
       Имя копируем ДО открытия: оно затрёт fat_ent_*. */
    s_path[s_depth] = fat_ent_clus;
    {
        char *end = copy_name(s_path_str + s_path_end[s_depth], fat_ent_name, NAME_KEEP);
        *end = '/';
        s_path_end[s_depth + 1] = (u8)(end + 1 - s_path_str);
    }
    ++s_depth;

    if (fw_dir_open_ent()) {
        ui_alarm("dir open error");
        --s_depth;
        return;
    }
    show_dir();
}

/** Подняться на уровень выше. */
static void go_up(void)
{
    u32 came_from;
    u16 k;

    if (!s_in_dev) return;
    if (s_depth == 0) { show_devices(s_dev); return; }
    --s_depth;
    came_from = s_path[s_depth];
    if (!reopen()) return;

    /* Курсор на тот каталог, из которого вышли, а не в начало списка: по
       кластеру, а не по имени - имя в записи обрезано и в своём регистре,
       а кластер у каталога один. */
    for (k = s_first; k < s_count; ++k) {
        ent_get(k);
        if ((fat_ent_attr & FW_ATTR_DIR) != 0 && fat_ent_clus == came_from) {
            cursor_to(k);
            return;
        }
    }
}

/* -- Плеер -- */

/**
 * Отдать плате запись idx, уже прочитанную в fat_ent_* (fw_dir_get).
 *
 * Сброс перед стартом обязателен: только он останавливает то, что плата
 * играет сейчас, а память трека у неё одна.
 */
static void play_entry(u16 idx)
{
    const u16 was = s_play_idx;

    copy_name(s_playing, fat_ent_name, sizeof(s_playing));

    if (fw_open()) {
        /* "ovf" - отрезков больше, чем держит страница, иначе карта не
           ответила. */
        ui_alarm(fat_ext_overflow ? "open failed: ovf" : "open failed: io");
        s_play_active = FALSE;
        s_play_marked = FALSE;
        s_here = FALSE;
        row_attr(was);
        return;
    }

    ui_set_file(s_playing, fat_file_size);
    bus_reset();
    /* Размер плата должна знать заранее, чтобы понимать, где конец файла.
       Перемотка здесь бесплатна (fw_seek встаёт куда угодно), поэтому
       порядок - по воспроизведению, а не по файлу. */
    bus_start(fat_file_size, BUS_LOAD_BY_PLAYBACK);

    s_play_idx = idx;
    s_play_active = TRUE;
    s_play_dir = cur_dir_clus();
    s_play_dev = s_dev;
    s_play_marked = TRUE;
    s_here = TRUE;
    s_idle = 0;
    row_attr(was);

    /* Курсор - на играющую запись. Последней строкой не случайно:
       перерисовка списка зовёт fw_dir_get и затирает fat_ent_*, откуда
       выше берутся имя и размер файла. */
    cursor_to(idx);
}

/** Enter на записи под курсором: каталог - войти, файл - играть. */
static void play_selected(void)
{
    /* ".." - это подъём, а не вход: кластер ".." первого уровня нулевой,
       на нём прошивка встаёт. */
    if (ent_is_updir()) { go_up(); return; }
    if (fat_ent_attr & FW_ATTR_DIR) { go_into(); return; }
    play_entry(s_cursor);
}

/** Запись i - файл, а не каталог. Оставляет её в fat_ent_*. */
static bool_t is_file(u16 i)
{
    ent_get(i);
    return (bool_t)(!(fat_ent_attr & FW_ATTR_DIR));
}

/** Следующий номер шафла; s_count, если файлов нет. */
static u16 shuffle_next(void)
{
    u16 n;
    if (!s_lfsr_taps) {
        u8 w = 0;
        u16 mask = 3;
        while (mask < s_count) { mask = (u16)(mask << 1 | 1u); ++w; }
        s_lfsr_taps = s_taps[w];
        /* Начало круга - от часов, любое из 1..mask. */
        s_lfsr = fw_ticks & mask;
        if (!s_lfsr) s_lfsr = 1;
    }
    for (n = 0; n < 0x4000u; ++n) {
        s_lfsr = (u16)((s_lfsr >> 1) ^ ((s_lfsr & 1u) ? s_lfsr_taps : 0u));
        if (s_lfsr - 1u < s_count && s_lfsr - 1u >= s_first && is_file((u16)(s_lfsr - 1u))) return (u16)(s_lfsr - 1u);
    }
    return s_count;
}

/**
 * Трек доиграл или пропущен - следующий по режиму повтора, каталоги
 * пропускаются. skip - пропуск клавишей: и в режиме "один" идём дальше.
 */
static void play_next(bool_t skip)
{
    u16 i = s_play_idx, n;

    ui_flash(UI_FLASH_NEXT);
    if (s_repeat == REP_ONE && !skip && is_file(i)) { play_entry(i); return; }
    if (s_repeat == REP_SHUFFLE) {
        i = shuffle_next();
        if (i < s_count) { play_entry(i); return; }
    } else {
        /* Со следующей по кругу до играющей включительно; "один раз" - до
           конца каталога. */
        for (n = s_count - s_first; n; --n) {
            if (++i >= s_count) {
                if (s_repeat == REP_ONCE) break;
                i = s_first;
            }
            if (is_file(i)) { play_entry(i); return; }
        }
    }

    s_play_active = FALSE;
}

/* Перемотка бесплатна: z80_fw встаёт на произвольную позицию. Окно уходит
   в порт порциями по сектору, подряд: плата считает байты порта, порции
   для неё не видны. При сбое чтения часть окна уже ушла, NAK - поверх. */
static u8 s_chunk[512];

/* Смещение кратно сектору, длина не больше остатка файла: за конец файла
   плата не просит. */

void plat_send_data(u32 offset, u16 len)
{
    u16 left = len, n;

    fw_seek(offset);
    while (left) {
        n = left < sizeof(s_chunk) ? left : (u16)sizeof(s_chunk);
        if (fw_read_bytes(s_chunk, n)) { bus_nak(); return; }
        bus_send(s_chunk, n);
        left -= n;
    }
    bus_sent_external(len);
    bus_done();
}

void plat_send_data_fast(u32 offset)
{
    u8 err;

    fw_seek(offset);

    /* Сектора идут с карты прямо в порт платы, мимо памяти. При сбое чтения
       часть окна плата уже получила, NAK приходит поверх. */
    fat_read_dest = 1;
    err = fw_read_sectors(0, (u16)(BUS_WINDOW_BYTES / 512u));
    fat_read_dest = 0;
    if (err) { bus_nak(); return; }

    bus_sent_external((u16)BUS_WINDOW_BYTES);
    bus_done();
}

/* -- Главный цикл -- */

/* Фатальная ошибка старта: показать и дождаться клавиши. Без ожидания
 * сообщение не прочитать - следом crt0 делает rst 0 и экран гаснет. */
static void fatal(const char *text)
{
    ui_alarm(text);
    while (fw_kbd_get() == 0) ui_update();
}

void main(void)
{
    u8 key, ev;

    /* Страницы под отрезки файла и таблицу каталога: банки 2 и 5 уже
       видны в других окнах. */
    fat_port_shadow = 0x10;
    fat_page_work = 0;
    fat_page_dir = 6;

    /* Большой каталог читается секундами: плату обслуживать изнутри. */
    fat_dir_hook = dir_load_tick;
    fw_irq_init();
    ui_init();

    if (fw_page_check()) { fatal("bad pages"); goto done; }

    /* Плата может и не отозваться - навигатор от этого работать не
       перестанет, просто играть будет нечему. */
    bus_ping();
    ui_update();

    show_devices(FW_DEV_ZC);

    for (;;) {
        key = fw_kbd_get();
        switch (key) {
        case FW_KEY_UP:    if (s_cursor > s_first) cursor_to(s_cursor - 1u); break;
        case FW_KEY_DOWN:  cursor_to(s_cursor + 1u); break;
        /* Влево/вправо - страница назад и вперёд: в длинном каталоге
           построчно долго, а прыжок в начало или конец мало что даёт. */
        case FW_KEY_LEFT:  page_move(FALSE); break;
        case FW_KEY_RIGHT: page_move(TRUE); break;
        case FW_KEY_ENTER:
            if (!s_in_dev) open_dev((u8)s_cursor);
            else if (s_count) { ent_get(s_cursor); play_selected(); }
            break;
        case FW_KEY_DEL:
            go_up();
            break;
        case FW_KEY_SPACE:
            /* Пропустить трек. Играет он или уже доиграл - неважно:
               play_next просто идёт дальше по списку. */
            if (s_play_active) play_next(TRUE);
            break;
        case 'm':
        case 'M':
            s_repeat = (u8)((s_repeat + 1u) & 3u);
            s_lfsr_taps = 0;
            ui_set_repeat(s_repeat);
            ui_flash(UI_FLASH_REPEAT);
            break;
        case 'p':
        case 'P':
            /* Пауза. Плата гасит выход затуханием и перестаёт звать движок,
               поэтому позиция стоит; повторное нажатие возвращает звук. */
            if (s_play_active && bus_board_found) bus_transport(BUS_TRANS_PAUSE);
            break;
        case 'f':
        case 'F':
            /* Перемотка вперёд на шаг. Плата гасит выход, прогоняет тики и
               возвращает звук; нажатия копятся. Назад движок не умеет. */
            if (s_play_active && bus_board_found && bus_seek_ok) {
                bus_transport(BUS_TRANS_FORWARD);
                ui_flash(UI_FLASH_FORWARD);
            }
            break;
        default:
            break;
        }

        /* Обслужить плату. Это же и есть пауза главного цикла -
           клавиатуру опрашивает прерывание. Трек кончился или нет,
           навигатору всё равно: список остаётся живым. */
        if (bus_board_found) {
            ev = bus_poll();
            serve(ev);
            if (ev != BUS_EV_NONE) s_idle = 0;
            else if (s_play_active && bus_state == BUS_STATE_LOADING) ++s_idle;

            if (s_play_active) {
                /* Трек доиграл. bus_state приходит с телеметрией позиции,
                   а bus_start сразу ставит его в "загрузку" - так что
                   второй раз на том же ENDED мы не сработаем. */
                if (bus_state == BUS_STATE_ENDED) {
                    play_next(FALSE);
                } else if (bus_state == BUS_STATE_FAILED || s_idle > SESSION_TRIES) {
                    /* Файл не принят: плата говорит это сама или молчит
                       на загрузке дольше всякого разумного. */
                    ui_alarm("file rejected");
                    play_next(TRUE);
                }
            }
        } else {
            fw_wait_frames(1);
        }
        /* Анимация экрана; рисует не чаще раза в кадр. */
        ui_update();
    }

done:
    /* Сюда - только ошибка старта, до bus_ping(): выхода из плеера нет. */
    fw_irq_stop();
}
