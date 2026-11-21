;; menu.s - ПЗУ-конфигуратор платы: терминал.
;;
;; Меню строит прошивка платы: в странице на 0x2000 сетка 32x24 (знак и
;; атрибут) и список изменённых отрезков кадра; здесь только вывод
;; отрезков и пересылка нажатий. Строка 0 - название, она своя: текст при
;; старте и перелив цвета в прерывании кадра.

        .module menu
        .globl  _main

        .globl  _fw_scr_cls
        .globl  _fw_scr_at
        .globl  _fw_scr_puts
        .globl  _fw_kbd_get
        .globl  _fw_kbd_flush

        .include "page.inc"

;; Цвета: чернила | бумага << 3 | яркость 0x40.
A_N             .equ    0x0F    ; светло-серый по синему
A_DEAD          .equ    0x57    ; плата молчит: белый по красному, ярко

        .area   _CODE

;; ===================================================================
;; Вход

;; Из кадрового прерывания: строка заголовка каждый кадр то жёлтым, то
;; зелёным. Только атрибуты, 32 байта.
TITLE_CELLS     .equ    32
TITLE_X         .equ    0       ; вся строка заголовка
cfg_frame::
        ld      a, (title_on)
        or      a
        ret     z
        ;; Чернила 6 и 4 по очереди по синей бумаге; без яркости: она
        ;; светлит и бумагу.
        ld      a, (rnd)
        xor     #2
        ld      (rnd), a
        add     a, #4 | 0x08
        ld      hl, #0x5800 + TITLE_X
        ld      b, #TITLE_CELLS
tf_loop:
        ld      (hl), a
        inc     hl
        djnz    tf_loop
        ret

_main:
        ;; Бордюр синий, под цвет экрана; биты звука и MIC - нули.
        ld      a, #1
        out     (#0xFE), a
        ld      a, #A_N
        call    _fw_scr_cls
        ld      a, #4
        ld      l, #10
        call    _fw_scr_at
        ld      hl, #s_wait
        call    _fw_scr_puts

        ;; Страница терминала появляется после загрузки прошивки платы; 250
        ;; кадров, 5 с, - предел ожидания.
        ld      b, #250
main_wait:
        call    term_probe
        jr      z, term_main
        halt
        djnz    main_wait

        ld      a, #A_DEAD
        call    _fw_scr_cls
        ld      a, #3
        ld      l, #10
        call    _fw_scr_at
        ld      hl, #s_dead1
        call    _fw_scr_puts
        ld      a, #3
        ld      l, #12
        call    _fw_scr_at
        ld      hl, #s_dead2
        call    _fw_scr_puts
main_dead:
        halt
        jr      main_dead

;; ===================================================================
;; Терминал: экран рисует плата, здесь вывод изменённых отрезков и
;; пересылка нажатий. Строка 0 - название, она своя.

;; Страница терминала: метка 'PTRM', версия 1. Z - она. B не трогает.
term_probe:
        ld      hl, (PAGE)
        ld      de, #0x5450             ; 'PT'
        or      a
        sbc     hl, de
        ret     nz
        ld      hl, (PAGE + 2)
        ld      de, #0x4D52             ; 'RM'
        or      a
        sbc     hl, de
        ret     nz
        ld      hl, (PAGE + 4)
        ld      de, #1
        or      a
        sbc     hl, de
        ret

term_main:
        ld      a, #A_N
        call    _fw_scr_cls
        ld      hl, #s_title    ; курсор после очистки в (0, 0)
        call    _fw_scr_puts
        ld      a, #1
        ld      (title_on), a
        call    _fw_kbd_flush
        ;; Экран пуст: плата по смене счётчика пришлёт кадр целиком.
        ld      hl, #TM_STARTS
        inc     (hl)
tm_loop:
        halt
tm_keys:
        call    _fw_kbd_get
        or      a
        jr      z, tm_frame
        call    term_push
        jr      tm_keys
tm_frame:
        ;; Кадр выложен - вывести и отметить показанным: до этого плата
        ;; сетку не трогает.
        ld      a, (PAGE + TM_UPD)
        ld      hl, #PAGE + TM_SHOWN
        cp      (hl)
        jr      z, tm_loop
        ld      (tm_upd), a
        call    term_draw
        ld      a, (tm_upd)
        ld      (PAGE + TM_SHOWN), a
        jr      tm_loop

;; Нажатие A плате; кольцо полно - нажатие теряется.
term_push:
        ld      c, a
        ld      a, (PAGE + TM_ACK)
        ld      b, a
        ld      a, (PAGE + TM_REQ)
        sub     b
        cp      #TM_KEY_SLOTS
        ret     nc
        ld      a, (PAGE + TM_REQ)
        and     #TM_KEY_SLOTS - 1
        ld      e, a
        ld      d, #0
        ld      hl, #PAGE + TM_KEYS
        add     hl, de
        ld      (hl), c
        ld      hl, #PAGE + TM_REQ
        inc     (hl)
        ret

term_draw:
        ld      a, (PAGE + TM_COUNT)
        cp      #TM_ALL
        jr      z, td_all
        or      a
        ret     z
        ld      b, a
        ld      ix, #PAGE + TM_SPANS
td_span:
        push    bc
        ld      l, 0(ix)
        ld      h, 1(ix)
        ld      b, 2(ix)
        ld      a, 3(ix)
        call    draw_span
        ld      de, #4
        add     ix, de
        pop     bc
        djnz    td_span
        ret
td_all:
        ld      l, #1
ta_row:
        push    hl
        ld      h, #0
        ld      b, #32
        ld      a, #1
        call    draw_span
        pop     hl
        inc     l
        ld      a, l
        cp      #24
        jr      nz, ta_row
        ret

;; Отрезок: L - строка, H - столбец, B - длина, A - 0 только цвет, иначе
;; знаки и цвет.
draw_span:
        push    af
        ;; DE = строка * 32 + столбец: у атрибутов и у сетки страницы это
        ;; одно смещение.
        ld      a, l
        rrca
        rrca
        rrca
        ld      e, a
        and     #0x03
        ld      d, a
        ld      a, e
        and     #0xE0
        or      h
        ld      e, a
        push    de
        push    bc
        ld      hl, #TM_ATTRS
        add     hl, de
        push    hl
        ld      hl, #0x5800
        add     hl, de
        ex      de, hl
        pop     hl
        ld      c, b
        ld      b, #0
        ldir
        pop     bc
        pop     de
        pop     af
        or      a
        ret     z
        ;; Точки: младший байт адреса знакоместа - E, старший - 0x40 | D << 3.
        ld      hl, #TM_CHARS
        add     hl, de
        ld      a, d
        add     a, a
        add     a, a
        add     a, a
        or      #0x40
        ld      d, a
ds_char:
        ld      a, (hl)
        inc     hl
        push    hl
        ;; Шрифт на 0x1800: знак c - с (0x300 + c) * 8.
        ld      l, a
        ld      h, #0x03
        add     hl, hl
        add     hl, hl
        add     hl, hl
        ld      a, (hl)
        ld      (de), a
        inc     l
        inc     d
        ld      a, (hl)
        ld      (de), a
        inc     l
        inc     d
        ld      a, (hl)
        ld      (de), a
        inc     l
        inc     d
        ld      a, (hl)
        ld      (de), a
        inc     l
        inc     d
        ld      a, (hl)
        ld      (de), a
        inc     l
        inc     d
        ld      a, (hl)
        ld      (de), a
        inc     l
        inc     d
        ld      a, (hl)
        ld      (de), a
        inc     l
        inc     d
        ld      a, (hl)
        ld      (de), a
        ld      a, d
        sub     #7
        ld      d, a
        inc     e
        pop     hl
        djnz    ds_char
        ret

;; ===================================================================
;; Строки

s_title:        .asciz  "  PI-CARD MUSICA SETUP UTILITY"
s_wait:         .asciz  "WAITING FOR THE BOARD..."
s_dead1:        .asciz  "THE BOARD DOES NOT ANSWER"
s_dead2:        .asciz  "POWER OFF AND ON TO RETRY"

        .area   _DATA
title_on:       .ds     1       ; надпись переливается: меню на экране
rnd:            .ds     1       ; чётность кадра заголовка
tm_upd:         .ds     1       ; кадр терминала, который выводится
