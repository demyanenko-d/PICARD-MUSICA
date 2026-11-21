;; page.s - переключение страниц памяти Spectrum 128 (порт 0x7FFD).
;;
;; Библиотека своей памяти не занимает: страницы под отрезки цепочек и под
;; таблицу каталога называет вызывающий, до монтирования тома.
;;
;; Порт 0x7FFD только для записи, поэтому библиотека ведёт теневую копию.
;; Вызывающий обязан положить в _fat_port_shadow то значение, с которым
;; работает сам: кроме номера страницы там выбор экрана, выбор ПЗУ и
;; защёлка, и затереть их - значит переключить ПЗУ посреди работы.
;;
;; Окно - 0xC000..0xFFFF: ни код, ни стек, ни буферы вызывающего там
;; лежать не должны, они уедут вместе со страницей. Прерывания при
;; переключении не запрещаем: обработчик в ПЗУ живёт ниже 0x4000 и
;; пользуется стеком вызывающего, а тот ниже 0xC000. Иначе прерывания были
;; бы закрыты на всё построение отрезков, сотни миллисекунд.

        .module page
        .globl  _fat_page_set
        .globl  _fat_page_work
        .globl  _fat_page_dir
        .globl  _fat_port_shadow
        .globl  _fat_page_check
        .globl  fat_page_enter
        .globl  fat_page_leave

        .area   _DATA

_fat_page_work::   .ds  1       ; отрезки файла
_fat_page_dir::    .ds  1       ; таблица каталога
_fat_port_shadow:: .ds  1       ; теневая копия 0x7FFD
fat_page_saved:    .ds  1       ; что было в окне до fat_page_enter

        .area   _CODE

;; Включить страницу A, запомнив, что было в 0x7FFD; fat_page_leave
;; вернёт это. Вызывающий про наши переключения знать не обязан. Ячейка
;; одна: вложенных включений в библиотеке нет. HL и DE не портятся.
fat_page_enter::
        ld      c, a
        ld      a, (_fat_port_shadow)
        ld      (fat_page_saved), a
        ld      a, c

;; Включить страницу A в окно 0xC000. Портит A, BC и флаги.
_fat_page_set::
        and     #0x07
        ld      c, a
        ld      a, (_fat_port_shadow)
        and     #0xF8
        or      c
fat_page_out:
        ld      (_fat_port_shadow), a
        ld      bc, #0x7FFD
        out     (c), a
        ret

fat_page_leave::
        ld      a, (fat_page_saved)
        jr      fat_page_out

;; Проверить, что названные страницы годятся. CY=1 - не годятся.
;;
;; Страницы 2 и 5 отвергаем: на 128K они уже видны в окнах 0x8000 и
;; 0x4000. Включённая в 0xC000, она дала бы ту же память дважды, и
;; запись отрезков легла бы поверх кода или экрана.
_fat_page_check::
        ld      a, (_fat_page_dir)
        call    fat_page_bad
        ret     c

        ;; И различаться они обязаны, иначе отрезки файла лягут поверх
        ;; таблицы каталога.
        ld      hl, #_fat_page_work
        cp      (hl)
        jr      z, fat_page_fail
        ld      a, (hl)

;; A не портит.
fat_page_bad:
        cp      #8
        jr      nc, fat_page_fail
        cp      #2
        jr      z, fat_page_fail
        cp      #5
        jr      z, fat_page_fail
        or      a
        ret
fat_page_fail:
        scf
        ret
