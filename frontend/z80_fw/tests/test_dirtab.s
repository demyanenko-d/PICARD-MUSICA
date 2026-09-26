;; test_dirtab.s - загрузить каталог в страницу и выдать его обратно.
;;
;; Результат нарочно в том же виде, что у test_dir.s: тогда его сверяет
;; та же проверка в эмуляторе, и сверяет она то, что нужно - таблица в странице
;; обязана отдавать те же записи, что даёт обход диска. Если бы формат был
;; свой, пришлось бы писать второго сверщика и верить, что оба правы.
;;
;; Результат по 0xA000:
;;   +0   0 успех, 1 карта, 2 монтирование, 3 страницы, 4 чтение,
;;        5 каталог не поместился в страницу
;;   +1   сколько записей
;;   +2   записи подряд: атрибуты (1), кластер (4), размер (4), имя, ноль

        .module test_dirtab
        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_dir_open_root
        .globl  _fat_dir_load
        .globl  _fat_dir_get
        .globl  _fat_dir_count
        .globl  _fat_dir_full
        .globl  _fat_ent_name
        .globl  _fat_ent_attr
        .globl  _fat_ent_clus
        .globl  _fat_ent_size
        .globl  _fat_page_check
        .globl  _fat_page_work
        .globl  _fat_page_pre
        .globl  _fat_page_dir
        .globl  _fat_port_shadow
        .globl  start

RESULT  .equ    0xA000

        .area   _DATA
td_i:           .ds     2
td_ptr:         .ds     2

        .area   _CODE

start::
        ld      sp, #0x9F00

        ld      a, #0x10
        ld      (_fat_port_shadow), a
        ld      a, #1
        ld      (_fat_page_work), a
        ld      a, #3
        ld      (_fat_page_pre), a
        ld      a, #4
        ld      (_fat_page_dir), a
        call    _fat_page_check
        jr      nc, td_pages_ok
        ld      a, #3
        jp      td_fail
td_pages_ok:

        call    _sd_init
        jr      nc, td_init_ok
        ld      a, #1
        jp      td_fail
td_init_ok:
        call    _fat_mount
        jr      nc, td_mount_ok
        ld      a, #2
        jp      td_fail
td_mount_ok:
        call    _fat_dir_open_root
        jr      nc, td_root_ok
        ld      a, #4
        jp      td_fail
td_root_ok:

        call    _fat_dir_load
        jr      nc, td_load_ok
        ld      a, #4
        jp      td_fail
td_load_ok:
        ld      a, (_fat_dir_full)
        or      a
        jr      z, td_fits
        ld      a, #5
        jp      td_fail
td_fits:

        ;; Выдать всё обратно - уже из страницы, а не с диска.
        ld      hl, #RESULT+2
        ld      (td_ptr), hl
        ld      hl, #0
        ld      (td_i), hl
td_loop:
        ld      hl, (td_i)
        ld      de, (_fat_dir_count)
        or      a
        sbc     hl, de
        jr      nc, td_done

        ld      hl, (td_i)
        call    _fat_dir_get
        jr      nc, td_got
        ld      a, #4
        jp      td_fail
td_got:
        ld      hl, (td_ptr)
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
        ld      (td_ptr), hl

        ld      hl, (td_i)
        inc     hl
        ld      (td_i), hl
        jr      td_loop

td_done:
        ld      hl, (_fat_dir_count)
        ld      a, l
        ld      (RESULT+1), a
        xor     a
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
