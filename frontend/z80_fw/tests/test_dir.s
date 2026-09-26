;; test_dir.s - прочитать корневой каталог целиком и выложить, что вышло.
;;
;; Результат по 0xA000:
;;   +0   0 успех, 1 карта не поднялась, 2 монтирование, 3 каталог,
;;        4 карта не ответила при обходе
;;   +1   сколько записей вернулось
;;   +2   записи подряд, каждая:
;;          +0  атрибуты
;;          +1  первый кластер (4 байта, младший вперёд)
;;          +5  размер        (4 байта)
;;          +9  имя в CP866, ноль в конце
;;
;; Плоский вид нарочно: разбирать его в эмуляторе так же просто, как
;; заполнять здесь, и никакого выравнивания посередине.

        .module test_dir
        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_dir_open_root
        .globl  _fat_dir_next
        .globl  _fat_ent_name
        .globl  _fat_ent_attr
        .globl  _fat_ent_clus
        .globl  _fat_ent_size
        .globl  start

RESULT  .equ    0xA000

        .area   _CODE

start::
        ld      sp, #0x9F00

        xor     a
        ld      (RESULT+1), a

        call    _sd_init
        jr      nc, td_init_ok
        ld      a, #1
        jr      td_fail
td_init_ok:

        call    _fat_mount
        jr      nc, td_mount_ok
        ld      a, #2
        jr      td_fail
td_mount_ok:

        call    _fat_dir_open_root
        jr      nc, td_open_ok
        ld      a, #3
        jr      td_fail
td_open_ok:

        ld      hl, #RESULT+2
td_loop:
        push    hl
        call    _fat_dir_next
        pop     hl
        jr      c, td_io_err
        or      a
        jr      z, td_done

        ld      a, (_fat_ent_attr)
        ld      (hl), a
        inc     hl

        ld      de, #_fat_ent_clus
        call    td_copy4
        ld      de, #_fat_ent_size
        call    td_copy4

        ld      de, #_fat_ent_name
td_name:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        or      a
        jr      nz, td_name

        ld      a, (RESULT+1)
        inc     a
        ld      (RESULT+1), a
        cp      #200            ; на всякий случай: не писать за буфер
        jr      c, td_loop

td_done:
        xor     a
        ld      (RESULT+0), a
        halt

td_io_err:
        ld      a, #4
td_fail:
        ld      (RESULT+0), a
        halt

td_copy4:
        ld      b, #4
td_copy4_l:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    td_copy4_l
        ret
