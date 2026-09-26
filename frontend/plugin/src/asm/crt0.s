;------------------------------------------------------------------------------
; crt0.s - стартовый код WC-плагина (SDCC Z80, --no-std-crt0).
;
; Линкуется ПЕРВЫМ в цепочке .rel, иначе _plugin_entry не попадёт по
; --code-loc (#8000).
;
; Входные параметры WC (при CALL plugin_base):
;   A'  = индекс расширения файла
;   BC  = указатель на имя файла (z-строка)
;   HL  = размер файла (младшее слово)
;   DE  = размер файла (старшее слово)
;   IX  = указатель на структуру активной панели WC
;
; Выход: A = код возврата (0=ESC, 2=следующий, 4=предыдущий)
;
; Адресное пространство:
;   #8000-#BFFF = код + данные плагина
;   #C000-#FFFF = окно VPL, которым распоряжается WC
;
; gsinit обнуляет _DATA и копирует _INITIALIZER -> _INITIALIZED: без этого
; при повторном запуске (NEXT/PREV) статические переменные держат мусор от
; прошлого.
;------------------------------------------------------------------------------

        .module crt0
        .globl  _main
        .globl  _wc_exit_code
        .globl  _wc_file_idx
        .globl  _wc_file_count

        ; Линкер-символы для runtime-инициализации C
        .globl  s__CODE
        .globl  l__CODE
        .globl  s__DATA
        .globl  l__DATA
        .globl  s__INITIALIZED
        .globl  s__INITIALIZER
        .globl  l__INITIALIZER

;------------------------------------------------------------------------------
; Точка входа: WC зовёт её как "CALL plugin_base". Параметры - в шапке.
;------------------------------------------------------------------------------
        .area _CODE

_plugin_entry::
        ; Стек не переключаем - работаем на стеке WC.

        ; Параметры WC - на стек: gsinit обнулит _DATA, до него писать
        ; в глобалы нельзя.
        push    hl              ; size_lo
        push    de              ; size_hi
        push    bc              ; file_name ptr

        ex      af, af'
        push    af              ; file_ext (в A)
        ex      af, af'

        ;--- Сохранить HL'/DE' (номер объекта / кол-во файлов) ---
        exx
        push    hl              ; file_idx  (HL')
        push    de              ; file_count (DE')
        exx

        ; Обнулить _DATA, разложить начальные значения.
        call    gsinit

        ; Разложить параметры WC по (теперь чистым) глобалам.
        pop     de              ; file_count (DE')
        ld      (_wc_file_count), de
        pop     hl              ; file_idx (HL')
        ld      (_wc_file_idx), hl

        pop     af
        ld      (_wc_file_ext), a

        pop     bc
        ld      (_wc_file_name), bc

        pop     de              ; size_hi
        pop     hl              ; size_lo
        ld      (_wc_file_size+0), hl
        ld      (_wc_file_size+2), de

        call    _main

        ; main() возвращает void, код выхода - в глобале.
        ld      a, (_wc_exit_code)

        ret

;------------------------------------------------------------------------------
; Глобальные переменные параметров WC (читаются из main.c)
;------------------------------------------------------------------------------
        .area _DATA

_wc_file_ext::  .db 0
_wc_file_name:: .dw 0
_wc_file_size:: .dw 0, 0   ; 32-bit LE
_wc_exit_code:: .db 0
_wc_file_idx::  .dw 0       ; HL': номер объекта в панели (от 1)
_wc_file_count::.dw 0       ; DE': кол-во объектов в панели (вкл. "..")

;------------------------------------------------------------------------------
; Инициализация C runtime: обнулить _DATA (аналог BSS) и скопировать
; _INITIALIZER -> _INITIALIZED (переменные с начальным значением).
;------------------------------------------------------------------------------
        .area _GSINIT

gsinit::
        ; -- 1. Обнулить _DATA --
        ld      bc, #l__DATA
        ld      a, b
        or      a, c
        jr      z, 00001$       ; l__DATA == 0 -> нечего обнулять

        ld      hl, #s__DATA
        ld      (hl), #0        ; первый байт = 0
        dec     bc
        ld      a, b
        or      a, c
        jr      z, 00001$       ; l__DATA == 1 -> уже готово

        ld      d, h
        ld      e, l
        inc     de              ; DE = s__DATA + 1
        ldir                    ; заполнить остаток нулями
00001$:

        ; -- 2. Скопировать _INITIALIZER -> _INITIALIZED --
        ld      bc, #l__INITIALIZER
        ld      a, b
        or      a, c
        jr      z, 00002$       ; нет инициализированных данных

        ld      hl, #s__INITIALIZER   ; источник (в ROM / code page 1)
        ld      de, #s__INITIALIZED   ; приёмник (в RAM / _DATA area)
        ldir
00002$:

        .area _GSFINAL

gsfinal::
        ret
