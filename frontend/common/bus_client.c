#include "bus_client.h"

/* Порты статуса и данных. __naked, sdcccall(1): указатель в HL, байт в A. */

/* Статус платы. 0x00 и 0xFF значат одно и то же: команды нет. */
static u8 bus_status(void) __naked
{
    __asm
    in   a, (#BUS_PORT_CMD)
    ret
    __endasm;
}

/*
 * Отправить восемь байт команды в порт статуса.
 * После записи - пауза, пока плата не разберёт команду.
 */
static void bus_write_cmd(const u8 *cmd) __naked
{
    cmd;
    __asm
    ld   c, #BUS_PORT_CMD
    ld   b, #8
    otir
    ld   bc, #150
00004$:
    dec  bc
    ld   a, b
    or   a, c
    jr   nz, 00004$
    ret
    __endasm;
}

/*
 * Прочитать count байт из порта данных в buf.
 *
 * Порядок аргументов не случаен: sdcccall(1) кладёт (u8, u16) в A и DE, а
 * (u16, u8) - в HL и на стек, чего в __naked не разобрать.
 *
 * Каждое чтение обслуживается прерыванием платы: оно сдвигает её позицию
 * в ответе и подставляет следующий байт. Поэтому INIR здесь нельзя - он
 * читает вдвое быстрее этого цикла.
 */
static void bus_read_n(u8 count, u8 *buf) __naked
{
    count; buf;
    __asm
    ex   de, hl                ; HL = buf, дальше DE не нужен
    ld   b, a                  ; счётчик; старший байт адреса не участвует,
    ld   c, #BUS_PORT_DAT     ; плата индексирует таблицу младшим
00004$:
    in   d, (c)
    ld   (hl), d
    inc  hl
    djnz 00004$
    ret
    __endasm;
}

/* Отдать 4096 байт из buf в порт данных. */
static void bus_write_fast(const u8 *buf) __naked
{
    buf;
    __asm
    ld   d, #16      ; внешний счётчик - B занят самим OTIR
    ld   c, #BUS_PORT_DAT
00001$:
    ld   b, #0        ; 0 = 256 для OTIR
    otir
    dec  d
    jr   nz, 00001$
    ret
    __endasm;
}

/*
 * Отдать count байт из buf в порт данных, без дополнения до 4096.
 * count приходит в DE (sdcccall(1), второй аргумент).
 */
static void bus_write_slow(const u8 *buf, u16 count) __naked
{
    buf; count;
    __asm
    ; HL = buf (первый указатель, sdcccall(1)), DE = count (второй u16)
    ld   c, e
    ld   b, d
00002$:
    ld   a, (hl)
    out  (#BUS_PORT_DAT), a
    inc  hl
    dec  bc
    ld   a, b
    or   a, c
    jr   nz, 00002$
    ret
    __endasm;
}

/* BCD: две десятичные цифры на байт. */
static u8 bcd_to_u8(u8 v)
{
    return (u8)(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
}

/* -- Переменные, которые видит приложение -- */

char bus_board_name[17];
bool_t bus_board_found;

u8  bus_dur_min, bus_dur_sec;
bool_t bus_dur_valid;
u16 bus_samples, bus_patterns, bus_instruments;
bool_t bus_file_info_valid;

u8  bus_pos_min, bus_pos_sec, bus_state;
bool_t bus_paused, bus_seek_ok;
bool_t bus_session_started;

u16 bus_psram_total, bus_psram_queued, bus_psram_loaded;
bool_t bus_psram_valid;

u8  bus_voices, bus_peak_voices, bus_cpu, bus_cull;
bool_t bus_load_valid;

u32 bus_req_offset;
u16 bus_req_length;

/* -- Коды протокола. Обязаны совпадать с soundsinth::io::HostProtocol. - */

#define ST_NONE          0x00
#define ST_NONE_ALT      0xFF   /* то же, что ST_NONE */
#define ST_RESET_DONE    0x11
#define ST_READ_FAST     0x12
#define ST_READ_SLOW     0x13
#define ST_SESSION_READY 0x14
#define ST_FILE_INFO     0x15
#define ST_POSITION      0x16
#define ST_VU            0x17
#define ST_SPECTRUM      0x18
#define ST_PSRAM         0x19
#define ST_ENGINE_LOAD   0x1A

#define HC_RESET  0x01
#define HC_START  0x02
#define HC_DONE   0x03
#define HC_NAK    0x04
#define HC_TRACE  0x05
#define HC_TRANS  0x06

/* Что плата будет слать сама (поле fields в HC_START). Незаказанное она не
   шлёт вовсе - незачем гонять по шине и подтверждать. */
#define HC_FIELD_FILE_INFO 0x01
#define HC_FIELD_POSITION  0x02
#define HC_FIELD_VU        0x04
#define HC_FIELD_SPECTRUM  0x08
#define HC_FIELD_PSRAM     0x10
#define HC_FIELD_LOAD      0x20

#define HC_SECTOR_512 2     /* enum размера сектора в HC_START */

/*
 * Аргументы команд платы.
 *
 * Раскладка на проводе - младшим байтом вперёд и без выравнивания, ровно
 * как SDCC кладёт структуры на Z80, поэтому поля читаются прямо из
 * принятого кадра. Проверяется размерами ниже.
 *
 * Последний байт каждого кадра - контрольный, сумма предыдущих.
 */
typedef struct {
    u32 offset;                 /* смещение в файле */
    u16 length;                 /* сколько байт нужно плате */
} BusReadReq;

typedef union {
    u8 raw[17];

    struct {                    /* ST_READ_FAST, ST_READ_SLOW */
        BusReadReq req;
        BusReadReq copy;        /* тот же запрос ещё раз */
        u8 sum;
    } read;

    struct {                    /* ST_FILE_INFO */
        u8  dur_min, dur_sec;   /* BCD */
        u16 samples, patterns, instruments;
        u8  sum;
    } file;

    struct {                    /* ST_POSITION */
        u8 min, sec;            /* BCD */
        u8 state;
        u8 sum;
    } pos;

    struct {                    /* ST_PSRAM */
        u16 total, queued, loaded;
        u8  sum;
    } psram;

    struct {                    /* ST_ENGINE_LOAD */
        u8 voices, peak, cpu, cull;
        u8 sum;
    } load;

    struct {                    /* ST_RESET_DONE */
        char name[16];
        u8   sum;
    } board;
} BusArgs;

/* Компилятор, кладущий эти структуры иначе, обязан сломаться здесь, а не
   выдавать мусор в телеметрии. */
typedef char bus_args_layout_check[
    (sizeof(BusArgs) == 17 && sizeof(((BusArgs *)0)->read) == 13 &&
     sizeof(((BusArgs *)0)->file) == 9 && sizeof(((BusArgs *)0)->pos) == 4 &&
     sizeof(((BusArgs *)0)->psram) == 7 && sizeof(((BusArgs *)0)->load) == 5)
    ? 1 : -1];

/* Один на всех: команды не вложены друг в друга. */
static BusArgs s_args;

/*
 * Команда плате: код и семь байт аргументов, всегда восемь байт.
 *
 * Раскладка та же, что у BusArgs, и проверяется так же.
 */
typedef struct {
    u8 code;
    union {
        u8 raw[7];

        struct {                /* HC_START */
            u32 file_length;
            u8  sector_size;    /* enum: 2 = 512 байт */
            u8  fields;         /* какую телеметрию слать, HC_FIELD_* */
            u8  load_order;     /* маршрут за сэмплами, BUS_LOAD_* */
        } start;

        struct {                /* HC_DONE после bus_send */
            u16 length;         /* сколько байт ушло в порт данных */
        } sent;

        struct {                /* HC_TRANS */
            u8  op;
        } trans;

        struct {                /* HC_TRACE */
            u8  point;
            u16 a, b;
        } trace;
    } arg;
} BusCmd;

typedef char bus_cmd_layout_check[
    (sizeof(BusCmd) == 8 && sizeof(((BusCmd *)0)->arg.start) == 7 &&
     sizeof(((BusCmd *)0)->arg.sent) == 2 && sizeof(((BusCmd *)0)->arg.trace) == 5)
    ? 1 : -1];

static BusCmd s_cmd;

/* Отправить набранную в s_cmd команду: nargs занятых байт аргументов,
   хвост обнулить. */
static void cmd_send(u8 code, u8 nargs)
{
    u8 i;
    s_cmd.code = code;
    for (i = nargs; i < 7; ++i) s_cmd.arg.raw[i] = 0;
    bus_write_cmd((const u8 *)&s_cmd);
}

/* Команда без аргументов. */
#define cmd_bare(code) cmd_send((code), 0)

/* Принять n байт аргументов в s_args и сверить контрольный байт
   (последний, входит в n). FALSE - не сошлось. */
static bool_t args_get(u8 n)
{
    u8 sum = 0;
    u8 i;
    bus_read_n(n, s_args.raw);
    for (i = 0; i + 1 < n; ++i) sum = (u8)(sum + s_args.raw[i]);
    return (bool_t)(sum == s_args.raw[n - 1]);
}

/* -- Действия -- */

/*
 * Сброс: команда и обязательное чтение ответа с подтверждением.
 *
 * Одной команды мало: в ответ на сброс плата вооружает кадр со своим именем
 * и до подтверждения не вооружает больше ничего. Сброс без чтения ответа
 * вешает следующую сессию.
 *
 * Имя остаётся в s_args.board - вызывающий заберёт, если оно ему нужно.
 */
static bool_t reset_sync(void)
{
    u16 tries;

    cmd_bare(HC_RESET);

    tries = 0;
    for (;;) {
        if (bus_status() == ST_RESET_DONE) break;
        if (++tries == 0) return FALSE;
    }

    /* Одна попытка перечитать при несовпадении контрольного байта: имя
       платы на экране - индикатор исправности шины, и мусор в нём должен
       быть виден. */
    if (!args_get(sizeof(s_args.board))) {
        cmd_bare(HC_NAK);
        tries = 0;
        for (;;) {
            if (bus_status() == ST_RESET_DONE) break;
            if (++tries == 0) return FALSE;
        }
        (void)args_get(sizeof(s_args.board));
    }

    cmd_bare(HC_DONE);
    return TRUE;
}

bool_t bus_ping(void)
{
    u8 i;

    bus_board_found = FALSE;
    if (!reset_sync()) return FALSE;

    for (i = 0; i < 16; ++i) bus_board_name[i] = s_args.board.name[i];
    bus_board_name[16] = 0;

    bus_board_found = TRUE;
    return TRUE;
}

#if BUS_TRACE
void bus_trace(u8 code, u16 a, u16 b)
{
    s_cmd.arg.trace.point = code;
    s_cmd.arg.trace.a = a;
    s_cmd.arg.trace.b = b;

    cmd_send(HC_TRACE, sizeof(s_cmd.arg.trace));
}
#endif

void bus_transport(u8 op)
{
    s_cmd.arg.trans.op = op;
    cmd_send(HC_TRANS, sizeof(s_cmd.arg.trans));
}

void bus_reset(void)
{
    (void)reset_sync();
}

void bus_start(u32 file_length, u8 load_order)
{
    s_cmd.arg.start.file_length = file_length;
    s_cmd.arg.start.sector_size = HC_SECTOR_512;
    s_cmd.arg.start.fields = HC_FIELD_FILE_INFO | HC_FIELD_POSITION |
                             HC_FIELD_PSRAM | HC_FIELD_LOAD;
    s_cmd.arg.start.load_order = load_order;

    cmd_send(HC_START, sizeof(s_cmd.arg.start));

    /* Плата с этого момента грузит, и позицию засевает состоянием
       "загрузка" - но телеметрия придёт не сразу, а до неё bus_state
       держал бы состояние прошлого трека. Приложению, которое смотрит на
       BUS_STATE_ENDED, этого хватило бы, чтобы решить, что новый трек уже
       доиграл. */
    bus_state = BUS_STATE_LOADING;
    bus_session_started = 1;
}

void bus_nak(void)
{
    cmd_bare(HC_NAK);
}

void bus_send(const u8 *data, u16 len)
{
    bus_write_slow(data, len);
    s_cmd.arg.sent.length = len;
}

void bus_send_fast(const u8 *data)
{
    bus_write_fast(data);
    s_cmd.arg.sent.length = (u16)BUS_WINDOW_BYTES;
}

void bus_sent_external(u16 length)
{
    s_cmd.arg.sent.length = length;
}

void bus_done(void)
{
    cmd_send(HC_DONE, sizeof(s_cmd.arg.sent));
}

/* Разобрать одну команду платы. Возвращает событие. */
static u8 bus_handle(u8 status)
{
    switch (status) {
        case ST_READ_FAST:
        case ST_READ_SLOW:
            if (!args_get(sizeof(s_args.read))) { cmd_bare(HC_NAK); return BUS_EV_NONE; }

            /* Запрос продублирован в кадре - сверяем копии. */
            if (s_args.read.req.offset != s_args.read.copy.offset ||
                s_args.read.req.length != s_args.read.copy.length) {
                cmd_bare(HC_NAK);
                return BUS_EV_NONE;
            }

            if (s_args.read.req.length == 0u ||
                s_args.read.req.length > BUS_WINDOW_BYTES) {
                cmd_bare(HC_NAK);
                return BUS_EV_NONE;
            }

            bus_req_offset = s_args.read.req.offset;
            if (status == ST_READ_FAST) {
                /* Быстрый путь принимает только целое окно; длину из кадра
                   здесь не используем. */
                bus_req_length = (u16)BUS_WINDOW_BYTES;
                return BUS_EV_READ_FAST;
            }
            bus_req_length = s_args.read.req.length;
            return BUS_EV_READ;

        case ST_FILE_INFO:
            if (!args_get(sizeof(s_args.file))) { cmd_bare(HC_NAK); return BUS_EV_NONE; }
            /* 0xFF - длительность ещё не известна: крупный .mid дочитывается
               фоном, плата пришлёт кадр заново. */
            bus_dur_valid = (bool_t)(s_args.file.dur_min != 0xFFu);
            bus_dur_min = bus_dur_valid ? bcd_to_u8(s_args.file.dur_min) : 0;
            bus_dur_sec = bus_dur_valid ? bcd_to_u8(s_args.file.dur_sec) : 0;
            bus_samples = s_args.file.samples;
            bus_patterns = s_args.file.patterns;
            bus_instruments = s_args.file.instruments;
            bus_file_info_valid = TRUE;
            cmd_bare(HC_DONE);
            return BUS_EV_TELEMETRY;

        case ST_POSITION:
            if (!args_get(sizeof(s_args.pos))) { cmd_bare(HC_NAK); return BUS_EV_NONE; }
            bus_pos_min = bcd_to_u8(s_args.pos.min);
            bus_pos_sec = bcd_to_u8(s_args.pos.sec);
            /* Состояние - младшие два бита, остальное признаки управления. */
            bus_state = (u8)(s_args.pos.state & 0x03u);
            bus_paused = (bool_t)((s_args.pos.state & 0x04u) != 0);
            bus_seek_ok = (bool_t)((s_args.pos.state & 0x08u) != 0);
            cmd_bare(HC_DONE);
            return BUS_EV_TELEMETRY;

        case ST_PSRAM:
            if (!args_get(sizeof(s_args.psram))) { cmd_bare(HC_NAK); return BUS_EV_NONE; }
            bus_psram_total = s_args.psram.total;
            bus_psram_queued = s_args.psram.queued;
            bus_psram_loaded = s_args.psram.loaded;
            bus_psram_valid = TRUE;
            cmd_bare(HC_DONE);
            return BUS_EV_TELEMETRY;

        case ST_ENGINE_LOAD:
            if (!args_get(sizeof(s_args.load))) { cmd_bare(HC_NAK); return BUS_EV_NONE; }
            bus_voices = s_args.load.voices;
            bus_peak_voices = s_args.load.peak;
            bus_cpu = s_args.load.cpu;
            bus_cull = s_args.load.cull;
            bus_load_valid = TRUE;
            cmd_bare(HC_DONE);
            return BUS_EV_TELEMETRY;

        case ST_SESSION_READY:
            /* Аргументов нет, но контрольный байт есть у всех команд -
               так у хоста один путь чтения на все случаи. */
            if (!args_get(1)) { cmd_bare(HC_NAK); return BUS_EV_NONE; }
            cmd_bare(HC_DONE);
            return BUS_EV_READY;

        /* Кадры, которые приложению не нужны: принять и подтвердить, иначе
           плата будет ждать ответа. */
        case ST_VU:
            if (!args_get(3)) { cmd_bare(HC_NAK); return BUS_EV_NONE; }
            cmd_bare(HC_DONE);
            return BUS_EV_NONE;

        case ST_SPECTRUM:
        case ST_RESET_DONE:
            if (!args_get(17)) { cmd_bare(HC_NAK); return BUS_EV_NONE; }
            cmd_bare(HC_DONE);
            return BUS_EV_NONE;

        default:
            /* Неизвестный код: подтверждать нечего, но и висеть нельзя. */
            cmd_bare(HC_NAK);
            return BUS_EV_NONE;
    }
}

/* Пауза между опросами статуса, десятки микросекунд. */
static void micro_delay(void) __naked
{
    __asm
    ld   bc, #11
00010$:
    dec  bc
    ld   a, b
    or   a, c
    jr   nz, 00010$
    ret
    __endasm;
}

u8 bus_poll(void)
{
    u8 tries;
    u8 event = BUS_EV_NONE;

    for (tries = 0; tries < 50; ++tries) {
        u8 status = bus_status();
        if (status != ST_NONE && status != ST_NONE_ALT) {
            event = bus_handle(status);
            break;
        }
        micro_delay();
    }

    return event;
}
