/*
 * main.c - навигатор по карте и плеер.
 *
 * Экран поделён надвое: сверху шесть строк про воспроизведение, снизу
 * список файлов. Обе половины живут одновременно, следующий трек
 * выбирается не прерывая текущий.
 *
 * Звук делает плата, сюда приходят только запросы на данные. Главный цикл
 * обслуживает две очереди событий: нажатия и запросы платы.
 */

#include "fw.h"
#include "ui.h"
#include "../common/bus_client.h"

/*
 * Ядро плеера, платформонезависимое: знает только протокол
 * (bus_client.h). Всё, что упирается в машину, объявлено как plat_* и
 * реализовано ниже.
 */

/**
 * Ответ на BUS_EV_READ: отдать len байт с offset и подтвердить. Не
 * прочиталось - bus_nak, плата попросит то же окно.
 */
void plat_send_data(u32 offset, u16 len);

/** Ответ на BUS_EV_READ_FAST: то же, но ровно BUS_WINDOW_BYTES байт. */
void plat_send_data_fast(u32 offset);

/** Телеметрия обновилась - показать. */
void ui_update(void);

/* -- Навигатор -- */

static u16 s_count;             /* сколько записей в текущем каталоге */
static u16 s_cursor;            /* какая выбрана */
static u16 s_top;               /* какая первая на экране */
static u8  s_depth;             /* насколько глубоко ушли от корня */

/* Кластеры каталогов на пути от корня: по ним возвращаемся назад.
   Восьми уровней хватает: глубже на музыкальной карте не забираются. */
#define MAX_DEPTH 8
static u32 s_path[MAX_DEPTH];

/* Имена каталогов на том же пути - для строки полного пути. Держим не всё
   имя: в строку влезает 32 знака, длинные всё равно не показать целиком, а
   прошивка отдаёт до 260 байт. */
#define NAME_KEEP 16
static char s_names[MAX_DEPTH][NAME_KEEP];
static char s_path_buf[MAX_DEPTH * NAME_KEEP + 2];

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
static bool_t s_play_marked;
static u16 s_open_fails;         /* сколько раз не открылся файл, для строки отказа */

static u32  s_idle;             /* пустых кругов подряд, пока не заиграло */

/* Сколько таких кругов терпим. Провалив разбор файла, плата просто
   сбрасывается - отдельного "не смогла" в проводе нет, и затянувшаяся
   тишина на загрузке это единственный признак. */
#define SESSION_TRIES 2000000ul

static void copy_name(char *dst, const char *src, u8 max)
{
    u8 i = 0;
    while (src[i] && i < (u8)(max - 1)) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

/* -- Каталог -- */

/** Полный путь в строку состояния. Длиннее 32 знаков - показываем хвост. */
static void draw_path(void)
{
    u16 len = 0;
    u8 i, j;

    s_path_buf[len++] = '/';
    for (i = 0; i < s_depth; ++i) {
        for (j = 0; j < NAME_KEEP && s_names[i][j]; ++j) s_path_buf[len++] = s_names[i][j];
        s_path_buf[len++] = '/';
    }
    s_path_buf[len] = 0;

    if (len <= 32) { ui_draw_path(s_path_buf); return; }
    s_path_buf[len - 32] = '.';
    s_path_buf[len - 31] = '.';
    s_path_buf[len - 30] = '.';
    ui_draw_path(s_path_buf + len - 32);
}

/** Запись под курсором - это ".."? */
static bool_t ent_is_updir(void)
{
    return (bool_t)(fat_ent_name[0] == '.' && fat_ent_name[1] == '.' && fat_ent_name[2] == 0);
}

/** Кластер текущего каталога; у корня нулевой. */
static u32 cur_dir_clus(void)
{
    return s_depth ? s_path[s_depth - 1] : 0;
}

static void list_draw(void)
{
    /* Играющее подсвечивается только в своём каталоге: номер записи в
       чужом означает чужой файл. */
    const bool_t here = (bool_t)(s_play_marked && cur_dir_clus() == s_play_dir);
    u16 i;
    for (i = 0; i < UI_LIST_ROWS; ++i) {
        u16 idx = s_top + i;
        if (idx >= s_count) {
            ui_draw_entry((u8)i, "", 0, 0, 0);
            continue;
        }
        fw_dir_get(idx);
        ui_draw_entry((u8)i, fat_ent_name,
                      (u8)((fat_ent_attr & FW_ATTR_DIR) != 0),
                      (u8)(idx == s_cursor),
                      (u8)(here && idx == s_play_idx));
    }
}

/* -- Сортировка списка --
 *
 * Каталоги вперёд, дальше по имени без учёта регистра.
 *
 * Переставляется только таблица указателей: записи лежат в своей
 * странице, указатель два байта против десятков у записи.
 *
 * Шелл: каталог влезает в страницу целиком, до ~680 записей. Сортируется
 * один раз при загрузке каталога, не при перерисовке.
 */

#define DIR_INDEX_TOP  0xFFFEu   /* указатель записи i лежит по DIR_INDEX_TOP - i*2 */
#define ENT_NAME_OFS   9u        /* attr(1) + кластер(4) + размер(4) */

static u16 *idx_slot(u16 i)
{
    return (u16 *)(DIR_INDEX_TOP - (u16)(i << 1));
}

/* Регистр вверх. Кроме латиницы - CP866: а-п лежат подряд с 0xA0, а р-я
   оторваны на 0xE0, поэтому смещения разные. Имена приходят и длинные
   (там регистр как у пользователя), и 8.3 (там всегда заглавные), так
   что без приведения список разъехался бы на две группы. */
static u8 fold(u8 c)
{
    if (c >= 0x61 && c <= 0x7A) return (u8)(c - 0x20);   /* a-z */
    if (c >= 0xA0 && c <= 0xAF) return (u8)(c - 0x20);   /* а-п */
    if (c >= 0xE0 && c <= 0xEF) return (u8)(c - 0x50);   /* р-я */
    return c;
}

/* 1 - запись a должна идти после b. Страница уже должна быть домаплена. */
static u8 ent_after(u16 pa, u16 pb)
{
    const u8 *ea = (const u8 *)pa;
    const u8 *eb = (const u8 *)pb;
    const u8 da = (u8)((ea[0] & FW_ATTR_DIR) != 0);
    const u8 db = (u8)((eb[0] & FW_ATTR_DIR) != 0);
    const u8 *na, *nb;

    if (da != db) return (u8)(da ? 0 : 1);   /* каталоги вперёд */

    na = ea + ENT_NAME_OFS;
    nb = eb + ENT_NAME_OFS;
    for (;;) {
        const u8 ca = fold(*na++);
        const u8 cb = fold(*nb++);
        if (ca != cb) return (u8)(ca > cb);
        if (ca == 0) return 0;               /* совпали целиком */
    }
}

static void dir_sort(void)
{
    const u8 saved = (u8)(fat_port_shadow & 0x07);
    u16 gap, i, j, tmp;

    if (s_count < 2) return;
    fw_dir_page(fat_page_dir);

    gap = 1;
    while (gap < s_count / 3u) gap = (u16)(gap * 3u + 1u);
    for (;;) {
        for (i = gap; i < s_count; ++i) {
            tmp = *idx_slot(i);
            for (j = i; j >= gap && ent_after(*idx_slot((u16)(j - gap)), tmp); j = (u16)(j - gap)) {
                *idx_slot(j) = *idx_slot((u16)(j - gap));
            }
            *idx_slot(j) = tmp;
        }
        if (gap == 1) break;                 /* gap/3 обнулился бы и зациклил */
        gap /= 3u;
    }

    fw_dir_page(saved);
}

static void list_reload(void)
{
    if (fw_dir_load()) {
        ui_alarm("dir read error");
        s_count = 0;
        return;
    }
    s_count = fat_dir_count;
    dir_sort();   /* до любого fw_dir_get: он читает уже упорядоченный список */

    /* "." в FAT всегда первая, но проверяем явно, а не по порядку. Прятать
       её достаточно тем, что окно списка ниже неё не опускается. */
    s_first = 0;
    if (s_count) {
        fw_dir_get(0);
        if (fat_ent_name[0] == '.' && fat_ent_name[1] == 0) s_first = 1;
    }
    s_cursor = s_first;
    s_top = s_first;
    /* s_play_idx указывал в прошлый список - в новом это другой файл.
       Играющий трек не трогаем (он уже на плате), но автопереход
       обрываем: продолжать очередь можно только там, где она началась. */
    s_play_active = FALSE;
    list_draw();
}

/** Открыть корень и показать его. */
static void go_root(void)
{
    s_depth = 0;
    if (fw_dir_open_root()) {
        ui_alarm("no root");
        return;
    }
    draw_path();
    list_reload();
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
    copy_name(s_names[s_depth], fat_ent_name, NAME_KEEP);
    ++s_depth;

    if (fw_dir_open_ent()) {
        ui_alarm("dir open error");
        --s_depth;
        return;
    }
    draw_path();
    list_reload();
}

static void cursor_to(u16 idx);

/** Подняться на уровень выше. */
static void go_up(void)
{
    u8 i;
    u32 came_from;
    u16 k;

    if (s_depth == 0) return;
    --s_depth;
    came_from = s_path[s_depth];

    /* Пройти путь заново от корня: хранить открытым только текущий каталог
       дешевле, чем держать состояние обхода на каждый уровень. Каталогов
       на пути единицы, и лишние чтения тут незаметны. */
    if (fw_dir_open_root()) { ui_alarm("no root"); return; }
    for (i = 0; i < s_depth; ++i) {
        fat_ent_clus = s_path[i];
        if (fw_dir_open_ent()) { ui_alarm("dir open error"); return; }
    }
    draw_path();
    list_reload();

    /* Курсор на тот каталог, из которого вышли, а не в начало списка: по
       кластеру, а не по имени - имя в записи обрезано и в своём регистре,
       а кластер у каталога один. */
    for (k = s_first; k < s_count; ++k) {
        fw_dir_get(k);
        if ((fat_ent_attr & FW_ATTR_DIR) != 0 && fat_ent_clus == came_from) {
            cursor_to(k);
            return;
        }
    }
}

/** Поставить курсор на запись idx, подкрутив прокрутку и перерисовав. */
static void cursor_to(u16 idx)
{
    if (idx >= s_count) return;

    s_cursor = idx;
    if (s_cursor < s_top) s_top = s_cursor;
    if (s_cursor >= s_top + UI_LIST_ROWS) s_top = s_cursor - UI_LIST_ROWS + 1;
    list_draw();
}

static void cursor_move(i16 delta)
{
    i16 pos = (i16)s_cursor + delta;
    if (pos < (i16)s_first) pos = (i16)s_first;
    if (pos >= (i16)s_count) pos = (i16)s_count - 1;
    if (pos < (i16)s_first) return;   /* пустой каталог */

    s_cursor = (u16)pos;
    if (s_cursor < s_top) s_top = s_cursor;
    if (s_cursor >= s_top + UI_LIST_ROWS) s_top = s_cursor - UI_LIST_ROWS + 1;
    list_draw();
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
    copy_name(s_playing, fat_ent_name, sizeof(s_playing));
    ui_field(0, UI_ROW_FILE, 32, "file:", UI_ATTR_PLAIN);
    ui_field(5, UI_ROW_FILE, 27, s_playing, UI_ATTR_TITLE);

    if (fw_open(0)) {
        /* Причина - в своей строке, которую телеметрия не перерисовывает, и
           остаётся до следующего отказа: номер отказа с начала работы, "ovf" -
           отрезков больше, чем держит страница, "io" - карта не ответила;
           число - сколько отрезков набралось, "c" - младшее слово первого
           кластера. */
        ++s_open_fails;
        ui_field_num(0, UI_ROW_ERROR, 11, "open err ", s_open_fails, UI_ATTR_ALARM);
        ui_field_num(11, UI_ROW_ERROR, 10, fat_ext_overflow ? "ovf " : "io ", fat_ext_count, UI_ATTR_ALARM);
        ui_field_num(21, UI_ROW_ERROR, 11, "c ", (u16)fat_ent_clus, UI_ATTR_ALARM);
        ui_alarm("open failed");
        s_play_active = FALSE;
        s_play_marked = FALSE;
        return;
    }

    bus_reset();
    /* Размер плата должна знать заранее, чтобы понимать, где конец файла.
       Перемотка здесь бесплатна (fw_seek встаёт куда угодно), поэтому
       порядок - по воспроизведению, а не по файлу. */
    bus_start(fat_file_size, BUS_LOAD_BY_PLAYBACK);

    s_play_idx = idx;
    s_play_active = TRUE;
    s_play_dir = cur_dir_clus();
    s_play_marked = TRUE;
    s_idle = 0;

    /* Навигатор идёт за играющим треком. Последней строкой не случайно:
       list_draw зовёт fw_dir_get на каждую видимую запись и затирает
       fat_ent_*, откуда выше берутся имя и размер файла. */
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

/**
 * Трек доиграл - переходим на следующий файл списка, каталоги пропускаем.
 * Список кончился - останавливаемся.
 */
static void play_next(void)
{
    u16 i;

    for (i = s_play_idx + 1u; i < s_count; ++i) {
        fw_dir_get(i);
        if (!(fat_ent_attr & FW_ATTR_DIR)) { play_entry(i); return; }
    }

    s_play_active = FALSE;
}

/* Перемотка бесплатна: z80_fw встаёт на произвольную позицию. */
static u8 s_window[BUS_WINDOW_BYTES];

/* Смещение кратно сектору, длина не больше остатка файла: за конец файла
   плата не просит. */

void plat_send_data(u32 offset, u16 len)
{
    fw_seek(offset);
    if (fw_read_bytes(s_window, len)) { bus_nak(); return; }

    bus_send(s_window, len);
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
    ui_field(0, UI_ROW_STATE, 15, "press any key", UI_ATTR_PLAIN);
    while (fw_kbd_get() == 0) { }
}

void main(void)
{
    u8 key;

    /* Страницы под отрезки файлов и таблицу каталога: банки 2 и 5 уже
       видны в других окнах, свободных ровно три. */
    fat_port_shadow = 0x10;
    fat_page_work = 0;
    fat_page_pre = 4;
    fat_page_dir = 6;

    fw_irq_init();
    ui_init();

    if (fw_page_check()) { fatal("bad pages"); goto done; }
    if (fw_sd_init())    { fatal("no card");   goto done; }
    if (fw_mount())      { fatal("no fat32");  goto done; }

    /* Плата может и не отозваться - навигатор от этого работать не
       перестанет, просто играть будет нечему. */
    bus_ping();
    ui_update();

    go_root();

    for (;;) {
        key = fw_kbd_get();
        switch (key) {
        case FW_KEY_UP:    cursor_move(-1); break;
        case FW_KEY_DOWN:  cursor_move(1);  break;
        /* Влево/вправо - в начало и в конец каталога.
           Постранично листать незачем: список короткий, шестнадцать строк,
           а вот до конца длинного каталога иначе добираться долго. */
        case FW_KEY_LEFT:  cursor_to(s_first); break;
        case FW_KEY_RIGHT: if (s_count) cursor_to(s_count - 1u); break;
        case FW_KEY_ENTER:
            if (s_count) { fw_dir_get(s_cursor); play_selected(); }
            break;
        case FW_KEY_DEL:
            go_up();
            break;
        case FW_KEY_SPACE:
            /* Пропустить трек. Играет он или уже доиграл - неважно:
               play_next просто идёт дальше по списку. */
            if (s_play_active) play_next();
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
            if (s_play_active && bus_board_found && bus_seek_ok) bus_transport(BUS_TRANS_FORWARD);
            break;
        case FW_KEY_ESC:
            goto done;
        default:
            break;
        }

        /* Обслужить плату. Это же и есть пауза главного цикла -
           клавиатуру опрашивает прерывание. Трек кончился или нет,
           навигатору всё равно: список остаётся живым. */
        if (bus_board_found) {
            switch (bus_poll()) {
            case BUS_EV_READ_FAST:
                plat_send_data_fast(bus_req_offset);
                s_idle = 0;
                break;
            case BUS_EV_READ:
                plat_send_data(bus_req_offset, bus_req_length);
                s_idle = 0;
                break;
            case BUS_EV_TELEMETRY:
            case BUS_EV_READY:
                ui_update();
                s_idle = 0;
                break;
            default:
                /* Плата молчит на загрузке дольше всякого разумного -
                   значит файл она не приняла. Не застреваем на нём. */
                if (s_play_active && bus_state == BUS_STATE_LOADING && ++s_idle > SESSION_TRIES) {
                    ui_alarm("file rejected");
                    play_next();
                }
                break;
            }

            /* Трек доиграл. bus_state приходит с телеметрией позиции, а
               bus_start сразу ставит его в "загрузку" - так что второй раз
               на том же ENDED мы не сработаем. */
            if (s_play_active && bus_state == BUS_STATE_ENDED) play_next();
        } else {
            fw_wait_frames(1);
        }
    }

done:
    /* Плата играет сама по себе: не сбросить - так и останется играть уже
       после выхода. До bus_ping() сюда попадают только ошибки старта, там
       платы ещё нет. */
    if (bus_board_found) bus_reset();
    fw_irq_stop();
}
