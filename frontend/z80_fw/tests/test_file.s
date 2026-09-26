;; test_file.s - открыть файл по имени и прочитать его двумя режимами.
;;
;; Имя ищем длинное и с кириллицей: если поиск сравнивает не то и не так,
;; на таком имени это видно сразу.
;;
;; Проверяются байты, а не "программа не упала": эмулятор сверяет всё
;; прочитанное с образом. Отдельно - что точный режим отдаёт то же самое,
;; что быстрый, но начиная с середины сектора: именно там ошибка в
;; пересчёте позиции и живёт.
;;
;; Результат по 0xA000:
;;   +0   0 успех, дальше коды бед (см. ниже)
;;   +1   отрезков у файла (2 байта)
;;   +3   размер файла (4 байта)
;;   +7   позиция после быстрого чтения (4 байта)
;;   +11  позиция после точного чтения (4 байта)
;; Данные быстрого чтения - по 0xA200 (4 сектора = 2048 байт).
;; Данные точного чтения - по 0xAA00 (700 байт с позиции 1234).

        .module test_file
        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_dir_open_root
        .globl  _fat_find
        .globl  _fat_open
        .globl  _fat_use
        .globl  _fat_seek
        .globl  _fat_read_sectors
        .globl  _fat_read_bytes
        .globl  _fat_read_dest
        .globl  _fat_file_size
        .globl  _fat_file_pos
        .globl  _fat_file_ext_count
        .globl  _fat_page_check
        .globl  _fat_page_work
        .globl  _fat_page_pre
        .globl  _fat_page_dir
        .globl  _fat_port_shadow
        .globl  start

RESULT  .equ    0xA000
FASTBUF .equ    0xA200
SLOWBUF .equ    0xAA00
SLOWPOS .equ    1234
SLOWLEN .equ    700
CROSSBUF .equ   0xAD00
CROSSPOS .equ   4000            ; отрезок кончается на 4096
CROSSLEN .equ   700
FAR2BUF .equ    0xB000
FAR2POS .equ    8192            ; ровно начало третьего отрезка

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
        jr      nc, tf_pages_ok
        ld      a, #3
        jp      tf_fail
tf_pages_ok:

        call    _sd_init
        jr      nc, tf_init_ok
        ld      a, #1
        jp      tf_fail
tf_init_ok:
        call    _fat_mount
        jr      nc, tf_mount_ok
        ld      a, #2
        jp      tf_fail
tf_mount_ok:

        call    _fat_dir_open_root
        jr      nc, tf_root_ok
        ld      a, #4
        jp      tf_fail
tf_root_ok:

        ld      hl, #tf_name
        call    _fat_find
        jp      c, tf_io
        or      a
        jr      nz, tf_found
        ld      a, #5           ; не нашли по имени
        jp      tf_fail
tf_found:

        xor     a
        call    _fat_open
        jr      nc, tf_open_ok
        ld      a, #6
        jp      tf_fail
tf_open_ok:

        ;; Что получилось при открытии.
        ld      hl, (_fat_file_ext_count)
        ld      (RESULT+1), hl
        ld      hl, #RESULT+3
        ld      de, #_fat_file_size
        call    tf_copy4

;; -- Быстрый режим: четыре сектора с начала --
        xor     a
        ld      (_fat_read_dest), a     ; в память
        ld      ix, #FASTBUF
        ld      bc, #4
        call    _fat_read_sectors
        jr      nc, tf_fast_ok
        ld      a, #7
        jp      tf_fail
tf_fast_ok:
        ld      hl, #RESULT+7
        ld      de, #_fat_file_pos
        call    tf_copy4

;; -- Точный режим: 700 байт с позиции 1234 --
;;
;; Позиция нарочно не кратна сектору и лежит во втором секторе, а длина
;; такая, что чтение переваливает через границу ещё раз.
        ld      de, #0
        ld      hl, #SLOWPOS
        call    _fat_seek
        ld      ix, #SLOWBUF
        ld      bc, #SLOWLEN
        call    _fat_read_bytes
        jr      nc, tf_slow_ok
        ld      a, #8
        jp      tf_fail
tf_slow_ok:
        ld      hl, #RESULT+11
        ld      de, #_fat_file_pos
        call    tf_copy4

;; -- Второй файл: раздроблённый, чтение ЧЕРЕЗ границу отрезка --
;;
;; У "Через один.xm" каждый кластер - свой отрезок, то есть отрезок
;; кончается каждые 4096 байт. Читаем с 4000-го: кусок приходится на два
;; РАЗНЫХ и не соседних куска диска. Если пересчёт позиции ошибается,
;; вторая половина приедет не оттуда.
        call    _fat_dir_open_root
        jr      nc, tf_root2_ok
        ld      a, #4
        jp      tf_fail
tf_root2_ok:
        ld      hl, #tf_name2
        call    _fat_find
        jp      c, tf_io
        or      a
        jr      nz, tf_found2
        ld      a, #9
        jp      tf_fail
tf_found2:
        xor     a
        call    _fat_open
        jr      nc, tf_open2_ok
        ld      a, #10
        jp      tf_fail
tf_open2_ok:
        ld      hl, (_fat_file_ext_count)
        ld      (RESULT+15), hl

        ld      de, #0
        ld      hl, #CROSSPOS
        call    _fat_seek
        ld      ix, #CROSSBUF
        ld      bc, #CROSSLEN
        call    _fat_read_bytes
        jr      nc, tf_cross_ok
        ld      a, #11
        jp      tf_fail
tf_cross_ok:

        ;; И быстрым режимом - с начала третьего отрезка.
        ld      de, #0
        ld      hl, #FAR2POS
        call    _fat_seek
        xor     a
        ld      (_fat_read_dest), a
        ld      ix, #FAR2BUF
        ld      bc, #2
        call    _fat_read_sectors
        jr      nc, tf_far_ok
        ld      a, #12
        jp      tf_fail
tf_far_ok:

        xor     a
tf_fail:
        ld      (RESULT+0), a
        halt
tf_io:
        ld      a, #4
        jp      tf_fail

tf_copy4:
        ld      b, #4
tf_copy4_l:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    tf_copy4_l
        ret

;; "Через один.xm" в CP866 - 20 кластеров, 20 отрезков.
tf_name2:
        .db     0x97, 0xa5, 0xe0, 0xa5, 0xa7, 0x20
        .db     0xae, 0xa4, 0xa8, 0xad
        .db     0x2e, 0x78, 0x6d, 0x00

;; "Длинный сплошной.it" в CP866 - 400 кластеров, один отрезок.
tf_name:
        .db     0x84, 0xab, 0xa8, 0xad, 0xad, 0xeb, 0xa9, 0x20
        .db     0xe1, 0xaf, 0xab, 0xae, 0xe8, 0xad, 0xae, 0xa9
        .db     0x2e, 0x69, 0x74, 0x00
