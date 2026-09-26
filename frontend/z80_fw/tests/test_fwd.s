;; test_fwd.s - сколько стоит пересылка "карта -> плата" с заходом в память
;; и без него.
;;
;; Оба пути гоняются в одном прогоне по одним и тем же секторам, подряд, а
;; границы помечаются записью в порт 0xFE: эмулятор запоминает такт каждой
;; метки. Так число получается из одного запуска, и разница не может
;; оказаться разницей между запусками.
;;
;; Метки:  1 - начало пути через память
;;         2 - начало прямого пути
;;         3 - конец
;;
;; Результат по 0xA000:
;;   +0   0 успех, 1 карта не поднялась, 2 чтение не удалось
;;
;; Байты обоих путей уходят в порт платы; их сверяет с образом проверка в
;; эмуляторе - иначе быстрый путь можно было бы "ускорить", потеряв данные.

        .module test_fwd
        .globl  _sd_init
        .globl  _sd_read_sector
        .globl  _sd_read_sector_to_port
        .globl  _sd_read_multi_to_port
        .globl  _sd_multi_count
        .globl  start

RESULT  .equ    0xA000
BUS_DAT .equ    0x67            ; тот же порт, что у драйвера карты
DBGMARK .equ    0x33            ; метка времени для эмулятора, не бордюр

SECTORS .equ    32              ; сколько секторов гоняем каждым путём
FIRST   .equ    1000            ; с какого начинаем

        .area   _DATA
fwd_buf:        .ds     512
fwd_lba:        .ds     2       ; младшее слово номера сектора
fwd_cnt:        .ds     1

        .area   _CODE

start::
        ld      sp, #0x9F00

        call    _sd_init
        jr      nc, tf_ok
        ld      a, #1
        ld      (RESULT+0), a
        halt
tf_ok:

;; -- Путь через память: INIR в буфер, потом OTIR из буфера --
        ld      a, #1
        out     (DBGMARK), a

        ld      hl, #FIRST
        ld      (fwd_lba), hl
        ld      a, #SECTORS
        ld      (fwd_cnt), a
tf_slow:
        ld      de, #0
        ld      hl, (fwd_lba)
        ld      ix, #fwd_buf
        call    _sd_read_sector
        jr      c, tf_fail

        ld      hl, #fwd_buf
        ld      c, #BUS_DAT
        ld      b, #0           ; 0 = 256
        otir
        ld      b, #0
        otir

        ld      hl, (fwd_lba)
        inc     hl
        ld      (fwd_lba), hl
        ld      a, (fwd_cnt)
        dec     a
        ld      (fwd_cnt), a
        jr      nz, tf_slow

;; -- Прямой путь: с карты сразу в порт --
        ld      a, #2
        out     (DBGMARK), a

        ld      hl, #FIRST
        ld      (fwd_lba), hl
        ld      a, #SECTORS
        ld      (fwd_cnt), a
tf_fast:
        ld      de, #0
        ld      hl, (fwd_lba)
        call    _sd_read_sector_to_port
        jr      c, tf_fail

        ld      hl, (fwd_lba)
        inc     hl
        ld      (fwd_lba), hl
        ld      a, (fwd_cnt)
        dec     a
        ld      (fwd_cnt), a
        jr      nz, tf_fast

;; -- Одной командой на всю пачку (CMD18) --
        ld      a, #3
        out     (DBGMARK), a

        ld      a, #SECTORS
        ld      (_sd_multi_count), a
        ld      de, #0
        ld      hl, #FIRST
        call    _sd_read_multi_to_port
        jr      c, tf_fail

        ld      a, #4
        out     (DBGMARK), a

        xor     a
        ld      (RESULT+0), a
        halt

tf_fail:
        ld      a, #2
        ld      (RESULT+0), a
        halt
