// SoundSinth Player - плагин Wild Commander: окно с телеметрией платы и
// выдача ей файла по запросу.

#include "inc/wc_api.h"
#include "inc/txtlib.h"
#include "inc/clk.h"
#include "bus_client.h"

#define EIHALT \
    __asm \
    ei \
    halt \
    __endasm;

/*
 * Обмен с платой - на 3.5 МГц: всё, что трогает порт
 * BUS_PORT_CMD или читает BUS_PORT_DAT. Запись в BUS_PORT_DAT
 * (bus_send/bus_send_fast), экран и диск остаются на 14 МГц.
 */
#define ON_BUS(call) do { clk_3m5(); call; clk_14m(); } while (0)

void plat_send_data(u32 offset, u16 len);
void plat_send_data_fast(u32 offset);
void ui_update(void);

/* Экран: окно Wild Commander. */

#define WND_W 50
#define WND_H 14        // без нужды не менять: на 15 строках WC портит экран

// Разделители рисует WC, каждый занимает свою строку - текста там быть
// не может. Строка 2 - разделитель шапки.
#define ROW_TITLE      1
#define ROW_BOARD      3
#define ROW_FILE       4
#define ROW_STATE      6    // starting / playing / finished
#define ROW_TIME       7    // "Loading: N / M" либо "Time: ММ:СС / ММ:СС"
#define ROW_COUNTS     8    // сэмплы, паттерны, инструменты
#define ROW_PSRAM      9    // сэмплы загружено / в очереди / всего
#define ROW_LOAD      10    // голоса и нагрузка ядра
#define ROW_HELP       (WND_H - 2)

// Смещение от низа окна: строка = (WND_H - 1) - from_bottom.
#define DIV1_FROM_BOTTOM 8  // -> строка 5: между File и состоянием
#define DIV2_FROM_BOTTOM 2  // -> строка 11: между нагрузкой и подсказкой

static wc_window_t s_wnd;

// txtlib-буфер: s_buf[0]=ёмкость, s_buf[1]=длина, [2..]=текст.
// print_line дополняет строку пробелами до ширины содержимого, границ
// буфера не проверяя, - отсюда проверки ниже.
// Вторая - на случай win.width == 0, когда txtlib берёт дефолтные 96.
static char s_buf[96];
typedef char s_buf_fits_window[(sizeof(s_buf) >= (2 + WND_W - 4)) ? 1 : -1];
typedef char s_buf_fits_default[(sizeof(s_buf) >= (2 + 96 - 4)) ? 1 : -1];

/** Строка "подпись + текст". value может быть 0. */
static void draw_line(u8 row, const char *label, const char *value, u8 attr)
{
    buf_clear(s_buf);
    buf_append_str(s_buf, label);
    if (value) buf_append_str(s_buf, value);
    print_line(&s_wnd, row, s_buf, attr);
}

/** Дописать в буфер "подпись + число". */
static void add_num(const char *label, u16 value)
{
    buf_append_str(s_buf, label);
    buf_append_u16_dec(s_buf, value);
}

// Строки, которые за сессию не меняются.
static void draw_static(void)
{
    draw_line(ROW_TITLE, "ugly media player", 0, WC_COLOR(WC_BLACK, WC_BRIGHT_WHITE));
    draw_line(ROW_BOARD, "Board: ", bus_board_found ? bus_board_name : (char*)"not found",
              WC_COLOR(WC_BLACK, bus_board_found ? WC_BRIGHT_GREEN : WC_BRIGHT_RED));
    draw_line(ROW_FILE, "File: ", wc_file_name, WC_COLOR(WC_BLACK, WC_WHITE));

    draw_line(ROW_HELP, "[<--] [-->] [Space] next [Esc] exit", 0, WC_COLOR(WC_BLACK, WC_WHITE));
}

// Значения bus_state плотные, 0..BUS_STATE_ENDED - отсюда таблица.
static const char *const k_state_text[] = {
    "State: starting", "State: playing", "State: finished"
};
static const u8 k_state_color[] = { WC_YELLOW, WC_BRIGHT_GREEN, WC_BRIGHT_CYAN };

/*
 * Перерисовать меняющееся по ходу игры. Состояние и загрузка приходят
 * разными кадрами и при фоновой догрузке верны обе сразу ("playing" плюс
 * "Loading: 12 / 47") - отсюда две строки.
 */
void ui_update(void)
{
    static u8 drawn_frame;
    static bool_t drawn_any;
    u8 now = *(volatile u8 *)WC_SYS_TMN;   // счётчик кадров WC, ++ на INT

    u8 st;
    u8 loading;

    if (drawn_any && now == drawn_frame) return;
    drawn_any = TRUE;
    drawn_frame = now;

    st = (bus_state <= BUS_STATE_ENDED) ? bus_state : BUS_STATE_PLAYING;
    // До кадра с очередью считаем, что загрузка идёт.
    loading = (st == BUS_STATE_LOADING) || !bus_psram_valid || bus_psram_queued;

    draw_line(ROW_STATE, k_state_text[st], 0, WC_COLOR(WC_BLACK, k_state_color[st]));

    buf_clear(s_buf);
    if (loading) {
        buf_append_str(s_buf, "Loading: ");
        if (bus_psram_valid) {
            buf_append_u16_dec(s_buf, bus_psram_loaded);
            add_num(" / ", bus_psram_total);
        } else {
            buf_append_str(s_buf, "...");
        }
        print_line(&s_wnd, ROW_TIME, s_buf, WC_COLOR(WC_BLACK, WC_YELLOW));
    } else if (!bus_file_info_valid) {
        buf_append_str(s_buf, "Loading: ...");
        print_line(&s_wnd, ROW_TIME, s_buf, WC_COLOR(WC_BLACK, WC_YELLOW));
    } else {
        buf_append_str(s_buf, "Time: ");
        buf_append_mmss(s_buf, (u16)bus_pos_min * 60u + bus_pos_sec);
        buf_append_str(s_buf, " / ");
        buf_append_mmss(s_buf, (u16)bus_dur_min * 60u + bus_dur_sec);
        print_line(&s_wnd, ROW_TIME, s_buf, WC_COLOR(WC_BLACK, WC_WHITE));
    }

    // Независимо от состояния: плата шлёт их после разбора метаданных,
    // до старта звука.
    if (bus_file_info_valid) {
        buf_clear(s_buf);
        add_num("Samples: ", bus_samples);
        add_num("  Patterns: ", bus_patterns);
        add_num("  Instr: ", bus_instruments);
        print_line(&s_wnd, ROW_COUNTS, s_buf, WC_COLOR(WC_BLACK, WC_WHITE));
    }

    // Счётчики сэмплов, не страниц PSRAM.
    if (bus_psram_valid) {
        buf_clear(s_buf);
        add_num("Samples loaded: ", bus_psram_loaded);
        add_num("  queued: ", bus_psram_queued);
        add_num("  total: ", bus_psram_total);
        print_line(&s_wnd, ROW_PSRAM, s_buf, WC_COLOR(WC_BLACK, WC_WHITE));
    }

    // Обе цифры за один период усреднения.
    if (bus_load_valid) {
        buf_clear(s_buf);
        add_num("Voices: ", bus_voices);
        /* Восклицательные знаки - сброс голосов по перегрузке, 0..3. */
        {
            u8 i;
            for (i = 0; i < bus_cull && i < 3; ++i) buf_append_str(s_buf, "!");
        }
        add_num("   CPU: ", bus_cpu);
        buf_append_str(s_buf, "%");
        print_line(&s_wnd, ROW_LOAD, s_buf,
                   WC_COLOR(WC_BLACK, bus_cpu >= 90 ? WC_BRIGHT_RED : WC_WHITE));
    }
}

/** Открыть окно и нарисовать его первый раз. Зовётся после bus_ping(). */
static void ui_init(void)
{
    wc_key_wait_release();
    wc_gedpl();

    // Центрируем сами: автоцентр по 255 на этой прошивке WC ненадёжен.
    // buf_addr = 0 - "окно не выведено, подставь адрес"; по нему wc_rresb
    // потом восстанавливает экран.
    wc_strset((char *)&s_wnd, sizeof(s_wnd), 0);
    s_wnd.type = WC_WIN_TYPE3 | WC_WIN_SHADOW;
    s_wnd.cur_mask = 0x05;
    {
        u8 scr_h = wc_get_height();            /* 25/30/36 */
        u8 scr_w = (scr_h >= 36) ? 90 : 80;
        s_wnd.x = (scr_w > WND_W) ? (scr_w - WND_W) / 2 : 0;
        s_wnd.y = (scr_h > WND_H) ? (scr_h - WND_H) / 2 : 0;
    }
    s_wnd.width = WND_W;
    s_wnd.height = WND_H;
    s_wnd.color = WC_COLOR(WC_BLACK, WC_WHITE);
    s_wnd.buf_addr = 0;
    s_wnd.divider1 = DIV1_FROM_BOTTOM;
    s_wnd.divider2 = DIV2_FROM_BOTTOM;

    buf_init(s_buf, (u8)(sizeof(s_buf) - 2));   // минус два служебных байта
    wc_prwow(&s_wnd);

    draw_static();
    ui_update();   // до первой телеметрии, по обнулённым переменным
}

/**
 * Закрыть окно и вернуть турбо из настроек WC. Обязателен на любом выходе
 * и в этом порядке: wc_rresb рисует. error != 0 - показать причину.
 */
static void ui_done(const char *error)
{
    if (error) draw_line(ROW_TIME, error, 0, WC_COLOR(WC_BLACK, WC_BRIGHT_RED));
    wc_rresb(&s_wnd);
    wc_turbopl(WC_TURBO_RESTORE, 0);
}

/**
 * Опросить клавиши. TRUE - пользователь уходит, код уже в wc_exit_code.
 * Кодом возврата не обойтись: у WC "выход" - это 0, и "ничего не нажато"
 * из него не выразить.
 */
static bool_t ui_get_user_action(void)
{
    static bool_t armed;
    static u8 polled_frame;
    u8 now = *(volatile u8 *)WC_SYS_TMN;

    // Первый заход - дождаться отпускания: у Space и стрелок в WC
    // автоповтор, и клавиша выбора трека проскочила бы его насквозь.
    if (!armed) {
        wc_key_wait_release();
        armed = TRUE;
    }

    if (now == polled_frame) return FALSE;
    polled_frame = now;

    if (wc_key_esc())                     { wc_exit_code = WC_EXIT_ESC;  return TRUE; }
    if (wc_key_space() || wc_key_right()) { wc_exit_code = WC_EXIT_NEXT; return TRUE; }
    if (wc_key_left())                    { wc_exit_code = WC_EXIT_PREV; return TRUE; }

    return FALSE;
}

/*
 * Файл через Wild Commander. Перехода на смещение у WC нет - только
 * чтение по 512 байт, пропуск блоков и перемотка в начало. Произвольный
 * доступ собирается из них, отсюда учёт позиции.
 */

// Ровно окно: смещения от платы кратны сектору, дочитывать нечего.
#define REPLY_BUF_BYTES BUS_WINDOW_BYTES
static u8 s_reply[REPLY_BUF_BYTES];
static u32 s_stream_pos;        // где сейчас стоит поток WC

static void stream_rewind(void)
{
    wc_gipagpl();
    s_stream_pos = 0;
}

// Сразу после перемотки пропуск блоков поток не двигает, следующее чтение
// идёт от нуля. Сдвигаем настоящим чтением сектора.
static void stream_prime(u32 aligned)
{
    if (aligned < 512u) return;
    wc_load512((u16)s_reply, 1);
    s_stream_pos = 512u;
}

static void stream_seek(u32 aligned)
{
    u32 blocks;

    if (aligned < s_stream_pos) {
        stream_rewind();
        stream_prime(aligned);
    }

    blocks = (aligned - s_stream_pos) / 512u;
    if (blocks > 32768u) blocks = 0;    // файл заведомо меньше 16 МБ
    while (blocks > 0) {
        u8 chunk = (blocks > 255u) ? (u8)255 : (u8)blocks;   // счётчик однобайтовый
        wc_loadnone(chunk);
        s_stream_pos += (u32)chunk * 512u;
        blocks -= chunk;
    }
}

/*
 * Последний сектор файла может быть неполным; WC читает его целиком,
 * байты за концом файла плата не возьмёт.
 */
void plat_send_data(u32 offset, u16 len)
{
    // len проверен библиотекой и не ноль - сектор минимум один.
    u8 blocks = (u8)(((u32)len + 511u) / 512u);

    stream_seek(offset);
    wc_load512((u16)s_reply, blocks);
    s_stream_pos += (u32)blocks * 512u;

    bus_send(s_reply, len);
    ON_BUS(bus_done());
}

void plat_send_data_fast(u32 offset)
{
    stream_seek(offset);
    wc_load512((u16)s_reply, (u8)(BUS_WINDOW_BYTES / 512u));
    s_stream_pos += BUS_WINDOW_BYTES;

    bus_send_fast(s_reply);
    ON_BUS(bus_done());
}

/* Пустых кругов до отказа: провалив разбор файла, плата молча
   сбрасывается - "не смогла" в проводе нет. */
#define SESSION_TRIES 2000000ul

static u16 s_laps;   // круги главного цикла, для пульса

void main(void)
{
    u32 idle = 0;               // пустых кругов подряд, пока не заиграло

    wc_exit_code = WC_EXIT_ESC; // если что-то сорвётся по пути
    wc_cache_set(WC_CACHE_OFF);
    clk_14m();

    ON_BUS(bus_reset());
    stream_rewind();   // сброс бывает и между сессиями, файл уже прочитан

    ON_BUS(bus_trace(TRACE_ENTER, WND_H, WND_W));
    // Отдельной переменной: при BUS_TRACE=0 аргументы не вычисляются.
    {
        bool_t found;
        clk_3m5();
        found = bus_ping();
        bus_trace(TRACE_PINGED, found ? 1u : 0u, 0);
        clk_14m();
    }

    ui_init();
    if (!bus_board_found) { ui_done("Board not found - check the bus"); return; }

    clk_3m5();
    bus_trace(TRACE_SESSION, (u16)wc_file_size, (u16)(wc_file_size >> 16));
    bus_start(wc_file_size, BUS_LOAD_BY_FILE);
    clk_14m();

    // Если трек доиграет сам: последний файл в панели -> выход,
    // иначе -> следующий.
    wc_exit_code = (wc_file_idx == wc_file_count) ? WC_EXIT_ESC : WC_EXIT_NEXT;
    ON_BUS(bus_trace(TRACE_LOOP, wc_exit_code, 0));

    for (;;) {
        u8 event;

        clk_3m5();
        // Пульс - раз в 256 кругов
        if ((++s_laps & 0x00FFu) == 0u) bus_trace(TRACE_PULSE, s_laps, bus_state);
        event = bus_poll();
        clk_14m();

        switch (event) {
        case BUS_EV_READ_FAST:
            plat_send_data_fast(bus_req_offset);
            idle = 0;
            break;
        case BUS_EV_READ:
            plat_send_data(bus_req_offset, bus_req_length);
            idle = 0;
            break;
        case BUS_EV_READY:
            ON_BUS(bus_trace(TRACE_LOADED, 1, 0));
            ui_update();
            idle = 0;
            break;
        case BUS_EV_TELEMETRY:
            ui_update();
            idle = 0;
            break;
        default:
            if (bus_state == BUS_STATE_LOADING) {
                if (++idle > SESSION_TRIES) {
                    ON_BUS(bus_trace(TRACE_LOADED, 0, 0));
                    ui_done("Session rejected by board");
                    return;
                }
            } else {
                // Маркер до halt: не выйдя из него, "после" не напечатать.
                if ((s_laps & 0x00FFu) == 0u) ON_BUS(bus_trace(TRACE_HALT, s_laps, 0));
                EIHALT
            }
            break;
        }

        if (bus_state == BUS_STATE_ENDED) break;   // трек доиграл сам
        if (ui_get_user_action()) break;           // пользователь переключает
    }

    clk_3m5();
    bus_trace(TRACE_LOOP_END, wc_exit_code, 0);
    bus_reset();
    bus_trace(TRACE_EXIT, wc_exit_code, 0);
    clk_14m();
    ui_done(0);
}
