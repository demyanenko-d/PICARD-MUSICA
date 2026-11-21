;; crt0.s - вход в навигатор образов по NMI и выход из него.
;;
;; Плата подставляет это ПЗУ на 0x0000-0x1FFF с выборки 0x0066 и снимает
;; после чтения 0x1FFF - второго байта RETN на 0x1FFE; так же снимает и
;; чтение 0x3FFF, поэтому его не читает никто. Окно 0x2000-0x3FFF - банк
;; ОЗУ навигатора.
;; Прерванная программа теряет только два байта своего стека - адрес
;; возврата, его кладёт процессор.
;;
;; РАСКЛАДКА
;;
;;   0x0000          сброс машины, пока ПЗУ подставлено: выход
;;   0x0066          вход
;;   0x1700..0x1EFF  знакогенератор (область _FONT)
;;   0x1FFE          RETN выхода (область _EXIT)
;;   0x2000..0x27FF  N0: страница обмена с платой
;;   0x2800..0x3BFF  N0: данные Си
;;   0x3C00..0x3C0F  SP программы, SP в момент NMI, R программы,
;;                   временный стек
;;   0x3C10..0x3FFD  стек
;;
;; Повторный NMI (дребезг кнопки, второе нажатие) узнаётся по SP: он
;; внутри 0x3C00..0x3FFF, а у программы стека там нет - это ПЗУ. Такой NMI
;; сразу возвращается. В N1 по 0x2100 лежит сохранённый экран, а
;; 0x3C00..0x3FFF свободны: повторный NMI во время копирования экрана пишет
;; туда же, мимо сохранённого. Не переживает только NMI в первые четырнадцать
;; команд входа (около 50 мкс) и в пять последних.
;;
;; Прерывания запрещены всё время: режим IM программы не прочитать, и
;; разрешить их значило бы позвать её обработчик. Регистр I и режим IM не
;; меняются; IFF1 возвращает RETN. R возвращается всеми восемью разрядами:
;; перед первой командой программы он такой же, как в момент приёма NMI.
;; Защиты держат в нём данные и считают по нему команды. Каждая выборка
;; команды (и приём NMI) прибавляет к младшим семи разрядам единицу, разряд
;; 7 меняет только LD R,A: от приёма NMI до LD A,R - семь выборок, от LD R,A
;; до программы - тоже семь.

        .module crt0
        .globl  _main
        .globl  s__DATA
        .globl  l__DATA
        .globl  s__INITIALIZED
        .globl  s__INITIALIZER
        .globl  l__INITIALIZER

        .include "obj/nav_page.inc"

GUARD   .equ    0x3C00
PROG_SP .equ    GUARD
NMI_SP  .equ    GUARD + 2
PROG_R  .equ    GUARD + 4
TMP_TOP .equ    GUARD + 0x10
STACK   .equ    0x3FFE

        .area   _CODE

;; Машину сбросили посреди навигатора, и плата этого не застала: выход тем
;; же RETN, адрес 0 - на стек в ОЗУ машины, после сброса его не жалко. Окно
;; - снова N0: вход рассчитывает на него.
        xor     a
        out     (NP_BANK_PORT), a
        ld      sp, #0x4002
        ld      hl, #0
        push    hl
        jp      nav_exit
        .ds     0x0066 - 13

        ld      (NMI_SP), sp
        ld      sp, #TMP_TOP
        push    af
        ld      a, r            ; R программы + 7
        push    af
        ld      a, (NMI_SP + 1)
        sub     #>GUARD
        cp      #>(0x4000 - GUARD)
        jr      c, nmi_again
        pop     af
        ld      (PROG_R), a
        pop     af
        ld      sp, (NMI_SP)
        ld      (PROG_SP), sp
        ld      sp, #STACK
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy
        ex      af, af'
        exx
        push    af
        push    bc
        push    de
        push    hl

        ;; Данные Си - заново на каждый вход.
        ld      hl, #s__DATA
        ld      de, #s__DATA + 1
        ld      bc, #l__DATA - 1
        ld      (hl), #0
        ldir
        ld      bc, #l__INITIALIZER
        ld      a, b
        or      a, c
        jr      z, nmi_no_init
        ld      de, #s__INITIALIZED
        ld      hl, #s__INITIALIZER
        ldir
nmi_no_init:
        call    gsinit
        call    _main

        pop     hl
        pop     de
        pop     bc
        pop     af
        exx
        ex      af, af'
        pop     iy
        pop     ix
        pop     hl
        pop     de
        ;; R = прочитанное на входе - 14: младшие семь разрядов, разряд 7 как
        ;; был. Дальше до программы ровно семь выборок.
        ld      a, (PROG_R)
        ld      b, a
        sub     #14
        xor     b
        and     #0x7F
        xor     b
        ld      r, a
        pop     bc
        pop     af
        ld      sp, (PROG_SP)
        jp      nav_exit

;; NMI во время работы навигатора: назад, ПЗУ не снимается. RET, а не RETN:
;; RETN взял бы IFF1 из IFF2, а там состояние программы - прерывания
;; включились бы внутри навигатора.
nmi_again:
        pop     af
        pop     af
        ld      sp, (NMI_SP)
        ret

        .area   _HOME
        .area   _INITIALIZER
        .area   _GSINIT
gsinit:
        .area   _GSFINAL
        ret

        .area   _DATA
        .area   _INITIALIZED
        .area   _BSEG
        .area   _BSS
        .area   _HEAP

        .area   _EXIT
nav_exit:
        retn
