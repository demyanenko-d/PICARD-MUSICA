;; scr.s - вывод на экран Спектрума.
;;
;; Экран 256x192 точек, 32x24 знакоместа. Пиксели лежат по 0x4000, атрибуты
;; по 0x5800, и то и другое в странице 5 - она видна всегда, переключение
;; страниц вывод не задевает.
;;
;; Подряд идут не строки знакомест, а строки точек внутри трети экрана.
;; Для знакоместа
;; (x, y) адрес первой строки точек:
;;
;;     0x4000 + ((y & 0x18) << 8) + ((y & 7) << 5) + x
;;
;; а остальные семь лежат на +0x100 друг от друга. Отсюда весь вывод
;; символа - восемь записей с шагом 256, без единого умножения.
;;
;; Атрибут знакоместа - просто 0x5800 + y*32 + x.

        .module scr
        .globl  _fw_scr_cls
        .globl  _fw_scr_at_hl
        .globl  _fw_scr_putc
        .globl  _fw_scr_puts
        .globl  _fw_scr_attr
        .globl  _fw_scr_fill_attr
        .globl  _fw_scr_x
        .globl  _fw_scr_y

        .globl  _fw_font

SCR_PIX .equ    0x4000
SCR_ATR .equ    0x5800

        .area   _DATA

_fw_scr_x::     .ds     1       ; куда пойдёт следующий символ
_fw_scr_y::     .ds     1
_fw_scr_attr::  .ds     1       ; каким цветом печатаем

        .area   _CODE

;; Очистить экран и залить атрибуты значением A.
_fw_scr_cls::
        ld      (_fw_scr_attr), a
        push    af

        ld      hl, #SCR_PIX
        ld      (hl), #0
        ld      de, #SCR_PIX+1
        ld      bc, #6144-1
        ldir

        pop     af
        call    _fw_scr_fill_attr

        xor     a
        ld      (_fw_scr_x), a
        ld      (_fw_scr_y), a
        ret

;; Залить все атрибуты значением A.
_fw_scr_fill_attr::
        ld      hl, #SCR_ATR
        ld      (hl), a
        ld      de, #SCR_ATR+1
        ld      bc, #768-1
        ldir
        ret

;; Поставить курсор: H = x (0..31), L = y (0..23).
;;
;; _hl на конце: рядом живёт переходник fw_scr_at для Си, берущий
;; координаты там, где их кладёт SDCC.
_fw_scr_at_hl::
        ld      a, h
        ld      (_fw_scr_x), a
        ld      a, l
        ld      (_fw_scr_y), a
        ret

;; Адрес первой строки точек текущего знакоместа -> HL.
;; Портит A и DE.
scr_addr:
        ld      a, (_fw_scr_y)
        and     #0x18           ; треть экрана
        add     a, #0x40        ; старший байт: 0x40, 0x48 или 0x50
        ld      h, a
        ld      a, (_fw_scr_y)
        and     #0x07           ; строка знакомест внутри трети
        rrca                    ; x32 - тремя вращениями вправо дешевле,
        rrca                    ; чем пятью сложениями влево
        rrca
        ld      l, a
        ld      a, (_fw_scr_x)
        or      l
        ld      l, a
        ret

;; Адрес атрибута текущего знакоместа -> HL.
scr_attr_addr:
        ld      a, (_fw_scr_y)
        rrca
        rrca
        rrca                    ; y*32, но старшие биты уехали вправо
        ld      l, a
        and     #0x03           ; они и есть старшая часть смещения
        add     a, #0x58
        ld      h, a
        ld      a, l
        and     #0xE0
        ld      l, a
        ld      a, (_fw_scr_x)
        or      l
        ld      l, a
        ret

;; Напечатать символ A в текущем месте и сдвинуть курсор.
;;
;; Перевод строки (0x0D) и возврат каретки понимаются.
_fw_scr_putc::
        cp      #0x0D
        jr      z, scr_newline
        push    af

        call    scr_addr
        ex      de, hl          ; DE - куда рисуем

        pop     af
        ld      l, a
        ld      h, #0
        add     hl, hl          ; x8: у символа восемь строк
        add     hl, hl
        add     hl, hl
        ld      bc, #_fw_font
        add     hl, bc          ; HL - картинка символа

        ld      b, #8
scr_putc_row:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     d               ; следующая строка точек - ровно +256
        djnz    scr_putc_row

        call    scr_attr_addr
        ld      a, (_fw_scr_attr)
        ld      (hl), a

        ;; Сдвинуть курсор, при нужде - на следующую строку.
        ld      a, (_fw_scr_x)
        inc     a
        cp      #32
        jr      c, scr_putc_x
        xor     a
        ld      (_fw_scr_x), a
        jr      scr_down
scr_putc_x:
        ld      (_fw_scr_x), a
        ret

scr_newline:
        xor     a
        ld      (_fw_scr_x), a
scr_down:
        ld      a, (_fw_scr_y)
        inc     a
        cp      #24
        jr      c, scr_down_ok
        ld      a, #23          ; ниже экрана не уходим: прокрутки нет
scr_down_ok:
        ld      (_fw_scr_y), a
        ret

;; Напечатать строку по HL, ноль в конце.
_fw_scr_puts::
        ld      a, (hl)
        or      a
        ret     z
        inc     hl
        push    hl
        call    _fw_scr_putc
        pop     hl
        jr      _fw_scr_puts
