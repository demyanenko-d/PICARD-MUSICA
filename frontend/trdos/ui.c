/*
 * ui.c - рисование экрана.
 *
 * Всё рисование идёт через поля фиксированной ширины: пишем текст и добиваем
 * пробелами до конца, иначе от прошлого значения остаётся хвост.
 */

#include "ui.h"
#include "../common/bus_client.h"

static char s_num[8];

/** Число в строку, без нулей впереди. */
static const char *num_str(u16 v)
{
    u8 i = 7;
    s_num[i] = 0;
    if (v == 0) { s_num[--i] = '0'; return &s_num[i]; }
    while (v && i) {
        s_num[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    return &s_num[i];
}

/** мм:сс - то же представление, что в плагине. */
static const char *mmss(u8 mn, u8 sc)
{
    s_num[0] = (char)('0' + (mn / 10u));
    s_num[1] = (char)('0' + (mn % 10u));
    s_num[2] = ':';
    s_num[3] = (char)('0' + (sc / 10u));
    s_num[4] = (char)('0' + (sc % 10u));
    s_num[5] = 0;
    return s_num;
}

void ui_field(u8 x, u8 y, u8 width, const char *text, u8 attr)
{
    u8 n = 0;
    fw_scr_attr = attr;
    fw_scr_at(x, y);
    while (text && *text && n < width) {
        fw_scr_putc((u8)*text++);
        ++n;
    }
    while (n < width) {
        fw_scr_putc(' ');
        ++n;
    }
}

void ui_field_num(u8 x, u8 y, u8 width, const char *label, u16 value, u8 attr)
{
    u8 n = 0;
    const char *p;

    fw_scr_attr = attr;
    fw_scr_at(x, y);
    while (*label && n < width) { fw_scr_putc((u8)*label++); ++n; }
    p = num_str(value);
    while (*p && n < width) { fw_scr_putc((u8)*p++); ++n; }
    while (n < width) { fw_scr_putc(' '); ++n; }
}

/* То же, что ui_field_num, плюс до marks восклицательных знаков сразу за
   числом: движок режет голоса из-за перегрузки, звучит не всё. Отдельная
   функция, а не лишний параметр у ui_field_num: тот зовётся часто, и
   параметр стоил бы по толканию в стек на каждом вызове. */
void ui_field_num_marks(u8 x, u8 y, u8 width, const char *label, u16 value, u8 marks, u8 attr)
{
    u8 n = 0;
    const char *p;

    fw_scr_attr = attr;
    fw_scr_at(x, y);
    while (*label && n < width) { fw_scr_putc((u8)*label++); ++n; }
    p = num_str(value);
    while (*p && n < width) { fw_scr_putc((u8)*p++); ++n; }
    while (marks > 0 && n < width) { fw_scr_putc('!'); --marks; ++n; }
    while (n < width) { fw_scr_putc(' '); ++n; }
}

void ui_init(void)
{
    u8 y;

    fw_scr_cls(UI_ATTR_PLAIN);

    ui_field(0, UI_ROW_TITLE, 16, "ugly player 0.3", UI_ATTR_TITLE);
    ui_field(16, UI_ROW_TITLE, 16, "no board", UI_ATTR_ALARM);
    ui_field(0, UI_ROW_FILE, 32, "file:", UI_ATTR_PLAIN);
    ui_field(0, UI_ROW_STATE, 16, "State:", UI_ATTR_PLAIN);
    ui_field(16, UI_ROW_STATE, 16, "time:", UI_ATTR_PLAIN);

    /* Пустые поля с подписями - чтобы экран не выглядел сломанным, пока
       плата ещё ничего не прислала. */
    ui_field(UI_COL1, UI_ROW_COUNTS, 10, "samp :-", UI_ATTR_PLAIN);
    ui_field(UI_COL2, UI_ROW_COUNTS, 10, "patt :-", UI_ATTR_PLAIN);
    ui_field(UI_COL3, UI_ROW_COUNTS, 10, "inst :-", UI_ATTR_PLAIN);
    ui_field(UI_COL1, UI_ROW_LOAD, 10, "load :-", UI_ATTR_PLAIN);
    ui_field(UI_COL2, UI_ROW_LOAD, 10, "queue:-", UI_ATTR_PLAIN);
    ui_field(UI_COL3, UI_ROW_LOAD, 10, "total:-", UI_ATTR_PLAIN);
    ui_field(UI_COL1, UI_ROW_VOICES, 10, "voice:-", UI_ATTR_PLAIN);
    ui_field(UI_COL2, UI_ROW_VOICES, 10, "cpu  :-", UI_ATTR_PLAIN);
    ui_field(UI_COL3, UI_ROW_VOICES, 10, "", UI_ATTR_PLAIN);

    ui_field(0, 6, 32, "", UI_ATTR_PLAIN);

    /* Список сразу закрашиваем целиком: пустые строки серым - это видимая
       рамка списка, по ней сразу понятно, где он кончается. */
    for (y = 0; y < UI_LIST_ROWS; ++y) {
        ui_field(0, (u8)(UI_ROW_LIST + y), 32, "", UI_ATTR_LIST);
    }
}

void ui_draw_path(const char *path)
{
    ui_field(0, UI_ROW_PATH, 32, path, UI_ATTR_TITLE);
}

void ui_draw_entry(u8 i, const char *name, u8 is_dir, u8 selected, u8 playing)
{
    u8 attr;
    u8 n = 0;

    if (selected) {
        attr = playing ? UI_ATTR_CURSOR_PLAY : UI_ATTR_CURSOR;
    } else {
        attr = playing ? UI_ATTR_LIST_PLAY : UI_ATTR_LIST;
    }

    fw_scr_attr = attr;
    fw_scr_at(0, (u8)(UI_ROW_LIST + i));

    /* Каталог помечаем скобками, а не цветом: цвет уже занят курсором, и
       на выбранной строке пометка иначе пропадала бы. */
    if (is_dir) { fw_scr_putc('['); ++n; }
    while (name && *name && n < 31) { fw_scr_putc((u8)*name++); ++n; }
    if (is_dir && n < 32) { fw_scr_putc(']'); ++n; }
    while (n < 32) { fw_scr_putc(' '); ++n; }
}

void ui_alarm(const char *text)
{
    ui_field(16, UI_ROW_STATE, 16, text, UI_ATTR_ALARM);
}

void ui_update(void)
{
    /* Плата сыплет телеметрию чаще, чем экран успевает её показать: рисуем
       не больше раза в кадр. Пропущенное не теряется - кадры телеметрии
       идут потоком, и следующий же покажет свежие значения.

       drawn_any нужен, чтобы первый вызов нарисовал наверняка: счётчик
       кадров может совпасть с нулевым drawn_frame. */
    static u8 drawn_frame;
    static bool_t drawn_any;
    u8 now = (u8)fw_ticks;

    u8 attr;

    if (drawn_any && now == drawn_frame) return;
    drawn_any = TRUE;
    drawn_frame = now;

    if (bus_board_found) {
        ui_field(16, UI_ROW_TITLE, 16, bus_board_name, UI_ATTR_TITLE);
    }

    /* Состояние и его цвет - как в плагине: жёлтый пока грузится, зелёный
       когда играет, голубой когда доиграл. */
    if (!bus_session_started) {
        /* До первой сессии состояния ещё нет. Ноль в bus_state значит
           "грузится", и без этой ветки экран уверял, что идёт загрузка,
           хотя пользователь ничего не выбирал. */
        ui_field(0, UI_ROW_STATE, 16, "State: idle", UI_ATTR_PLAIN);
    } else if (bus_state == 1) {
        ui_field(0, UI_ROW_STATE, 16, bus_paused ? "State: paused" : "State: playing", UI_ATTR_PLAY);
    } else if (bus_state == 2) {
        ui_field(0, UI_ROW_STATE, 16, "State: finished", UI_ATTR_DONE);
    } else {
        ui_field(0, UI_ROW_STATE, 16, "State: loading", UI_ATTR_LOAD);
    }

    if (bus_file_info_valid) {
        u8 n = 0;
        const char *p;
        fw_scr_attr = UI_ATTR_PLAIN;
        fw_scr_at(16, UI_ROW_STATE);
        p = "time:";
        while (*p) { fw_scr_putc((u8)*p++); ++n; }
        p = mmss(bus_pos_min, bus_pos_sec);
        while (*p) { fw_scr_putc((u8)*p++); ++n; }
        fw_scr_putc('/'); ++n;
        /* Длительность крупного .mid приходит не сразу: файл дочитывается
           фоном. До этого - прочерки, а не ноль: ноль читался бы как
           "трек пустой". */
        p = bus_dur_valid ? mmss(bus_dur_min, bus_dur_sec) : "--:--";
        while (*p && n < 16) { fw_scr_putc((u8)*p++); ++n; }
        while (n < 16) { fw_scr_putc(' '); ++n; }

        ui_field_num(UI_COL1, UI_ROW_COUNTS, 10, "samp :", bus_samples, UI_ATTR_PLAIN);
        ui_field_num(UI_COL2, UI_ROW_COUNTS, 10, "patt :", bus_patterns, UI_ATTR_PLAIN);
        ui_field_num(UI_COL3, UI_ROW_COUNTS, 10, "inst :", bus_instruments, UI_ATTR_PLAIN);
    }

    if (bus_psram_valid) {
        /* Пока очередь не пуста, загрузка ещё идёт - красим жёлтым, тем же
           цветом, что и состояние "грузится". */
        attr = bus_psram_queued ? UI_ATTR_LOAD : UI_ATTR_PLAIN;
        ui_field_num(UI_COL1, UI_ROW_LOAD, 10, "load :", bus_psram_loaded, attr);
        ui_field_num(UI_COL2, UI_ROW_LOAD, 10, "queue:", bus_psram_queued, attr);
        ui_field_num(UI_COL3, UI_ROW_LOAD, 10, "total:", bus_psram_total, UI_ATTR_PLAIN);
    }

    if (bus_load_valid) {
        /* Красный на девяноста процентах - тот же порог, что в плагине:
           выше него движок уже не успевает, и это надо видеть сразу. */
        attr = (bus_cpu >= 90) ? UI_ATTR_ALARM : UI_ATTR_PLAIN;
        /* Ширина 11, а не 10: "voice:" + две цифры + три знака. Колонки
           идут с шагом 11, поле занимает столбцы 0..10 и в соседнее не
           лезет. */
        ui_field_num_marks(UI_COL1, UI_ROW_VOICES, 11, "voice:", bus_voices, bus_cull, attr);
        ui_field_num(UI_COL2, UI_ROW_VOICES, 10, "cpu  :", bus_cpu, attr);
    }
}
