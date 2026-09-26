;; test_pre.s - префетч и обмен ролями страниц.
;;
;; Открываем рабочим один файл, префетчем - ДРУГОЙ, потом принимаем
;; префетченный и читаем. Прочитаться обязан второй файл, а не первый.
;;
;; Смысл проверки в том, что после приёма ничего не читается с диска:
;; сколько секторов ушло на приём, видно снаружи по счётчику карты. Если
;; окажется больше нуля - значит библиотека втихую перечитывает то, что
;; уже лежит в странице.
;;
;; Результат по 0xA000:
;;   +0   0 успех, дальше коды бед
;;   +1   страница рабочего до приёма
;;   +2   страница рабочего после приёма (обязана стать другой)
;;   +3   отрезков у принятого файла (2)
;;   +5   размер принятого файла (4)
;; Данные - по 0xA200: 1024 байта с начала принятого файла.
;;
;; Метки: 1 - перед приёмом, 2 - после приёма (до чтения).

        .module test_pre
        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_dir_open_root
        .globl  _fat_find
        .globl  _fat_open
        .globl  _fat_adopt
        .globl  _fat_read_sectors
        .globl  _fat_read_dest
        .globl  _fat_file_size
        .globl  _fat_file_ext_count
        .globl  _fat_page_check
        .globl  _fat_page_work
        .globl  _fat_page_pre
        .globl  _fat_page_dir
        .globl  _fat_port_shadow
        .globl  start

RESULT  .equ    0xA000
DATABUF .equ    0xA200
DBGMARK .equ    0x33            ; метка времени для эмулятора, не бордюр

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
        jp      c, tp_bad_pages

        call    _sd_init
        jp      c, tp_no_card
        call    _fat_mount
        jp      c, tp_no_mount

;; -- Рабочим - "Длинный сплошной.it" --
        call    _fat_dir_open_root
        jp      c, tp_io
        ld      hl, #tp_name1
        call    _fat_find
        jp      c, tp_io
        or      a
        jp      z, tp_notfound
        xor     a
        call    _fat_open
        jp      c, tp_openfail

;; -- Префетчем - "Через один.xm" --
        call    _fat_dir_open_root
        jp      c, tp_io
        ld      hl, #tp_name2
        call    _fat_find
        jp      c, tp_io
        or      a
        jp      z, tp_notfound
        ld      a, #1
        call    _fat_open
        jp      c, tp_openfail

        ld      a, (_fat_page_work)
        ld      (RESULT+1), a

;; -- Принять префетченный --
        ld      a, #1
        out     (DBGMARK), a
        call    _fat_adopt
        ld      a, #2
        out     (DBGMARK), a

        ld      a, (_fat_page_work)
        ld      (RESULT+2), a
        ld      hl, (_fat_file_ext_count)
        ld      (RESULT+3), hl
        ld      hl, #RESULT+5
        ld      de, #_fat_file_size
        call    tp_copy4

;; -- И прочитать: должен приехать ВТОРОЙ файл --
        xor     a
        ld      (_fat_read_dest), a
        ld      ix, #DATABUF
        ld      bc, #2
        call    _fat_read_sectors
        jp      c, tp_readfail

        xor     a
tp_fail:
        ld      (RESULT+0), a
        halt

tp_bad_pages:   ld a, #3
                jr tp_fail
tp_no_card:     ld a, #1
                jr tp_fail
tp_no_mount:    ld a, #2
                jr tp_fail
tp_io:          ld a, #4
                jr tp_fail
tp_notfound:    ld a, #5
                jr tp_fail
tp_openfail:    ld a, #6
                jr tp_fail
tp_readfail:    ld a, #7
                jr tp_fail

tp_copy4:
        ld      b, #4
tp_copy4_l:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    tp_copy4_l
        ret

;; "Длинный сплошной.it"
tp_name1:
        .db     0x84, 0xab, 0xa8, 0xad, 0xad, 0xeb, 0xa9, 0x20
        .db     0xe1, 0xaf, 0xab, 0xae, 0xe8, 0xad, 0xae, 0xa9
        .db     0x2e, 0x69, 0x74, 0x00

;; "Через один.xm"
tp_name2:
        .db     0x97, 0xa5, 0xe0, 0xa5, 0xa7, 0x20
        .db     0xae, 0xa4, 0xa8, 0xad
        .db     0x2e, 0x78, 0x6d, 0x00
