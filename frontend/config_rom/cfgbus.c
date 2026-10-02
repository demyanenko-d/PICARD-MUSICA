#include "cfgbus.h"

#include "fw.h"
#include "page.h"

/* Порты платы. Обязаны совпадать с firmware_config.h. */
#define BUS_PORT_CMD 0x63
#define BUS_PORT_DAT 0x67

/* Коды платы и команды хоста - из host_protocol.h. */
#define ST_NONE 0x00
#define ST_NONE_ALT 0xFF
#define ST_CONFIG_DONE 0x1B

#define HC_DONE 0x03
#define HC_CONFIG_REFRESH 0x07
#define HC_CONFIG_EXIT 0x08

/* Ждать плату дольше этого бессмысленно: она либо уже загрузилась, либо
   не загрузится. Пять секунд в кадрах по 20 мс. */
#define WAIT_FRAMES 250u

static u8 cfg_status(void) __naked {
    __asm
        in a, (#BUS_PORT_CMD)
        ret
    __endasm;
}

/*
 * Восемь байт команды в порт статуса, следом пауза: плата разбирает
 * команду прерыванием, и байты сверх её темпа теряются молча.
 */
static void cfg_write_cmd(const u8 *cmd) __naked {
    cmd;
    __asm
        ld c, #BUS_PORT_CMD
        ld b, #8
        otir
        ld bc, #150
    00001$:
        dec bc
        ld a, b
        or a, c
        jr nz, 00001$
        ret
    __endasm;
}

/* Прочитать кадр аргументов. Каждое чтение плата обслуживает прерыванием
   и подставляет следующий байт, поэтому INIR нельзя - он вдвое быстрее. */
static void cfg_read_args(u8 count, u8 *buf) __naked {
    count;
    buf;
    __asm
        ex de, hl
        ld b, a
        ld c, #BUS_PORT_DAT
    00001$:
        in d, (c)
        ld (hl), d
        inc hl
        djnz 00001$
        ret
    __endasm;
}

static void cfg_send(u8 code, u8 arg) {
    u8 cmd[8];
    u8 i;
    cmd[0] = code;
    cmd[1] = arg;
    for (i = 2; i < 8; ++i) cmd[i] = 0;
    cfg_write_cmd(cmd);
}

/* Дождаться кода ответа. Ноль - не дождались. */
static u8 cfg_wait_status(u8 want, u16 frames) {
    u16 until = fw_ticks + frames;
    for (;;) {
        u8 st = cfg_status();
        if (st == want) return st;
        if ((u16)(fw_ticks - until) < 0x8000u) return 0;
    }
}

/* Страница на месте и наша? Машине верить на слово нельзя и в обратную
   сторону: плата могла не дойти до её заполнения. */
static bool_t page_good(void) {
    const page_header_t *h = page_hdr();
    if (h->magic != PAGE_MAGIC) return FALSE;
    if (h->version != PAGE_VERSION) return FALSE;
    if (h->field_count == 0 || h->field_count > 64) return FALSE;
    return TRUE;
}

bool_t cfg_wait_board(void) {
    u16 until = fw_ticks + WAIT_FRAMES;
    for (;;) {
        if (page_good()) return TRUE;
        if ((u16)(fw_ticks - until) < 0x8000u) return FALSE;
    }
}

bool_t cfg_refresh(void) {
    u8 args[2];
    u8 ok;
    cfg_send(HC_CONFIG_REFRESH, 0);
    if (cfg_wait_status(ST_CONFIG_DONE, 50u) == 0) return FALSE;
    /* Кадр - признак и сумма; сумма у кадра из одного байта равна ему. */
    cfg_read_args(2, args);
    ok = (args[0] == 1) && (args[1] == args[0]);
    /* Подтвердить: без этого плата ничего больше не вооружит. */
    cfg_send(HC_DONE, 0);
    return ok ? TRUE : FALSE;
}

void cfg_exit(u8 how) {
    cfg_send(HC_CONFIG_EXIT, how);
    /* Плата уводит машину в сброс и перезагружается. Ждём этого. */
    for (;;) {
    }
}
