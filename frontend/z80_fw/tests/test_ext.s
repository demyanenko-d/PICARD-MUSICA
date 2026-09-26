;; test_ext.s - свернуть цепочки файлов в отрезки и показать, что вышло.
;;
;; Файлы берём из корня; если там их нет (как в настоящем архиве, где в
;; корне одни каталоги), спускаемся в первый подкаталог.
;;
;; Результат по 0xA000:
;;   +0   0 успех, 1 карта, 2 монтирование, 3 страницы не годятся,
;;        4 ошибка чтения, 5 не нашли, где брать файлы
;;   +1   сколько файлов обработано
;;   +2   записи подряд, по одной на файл:
;;          +0  1, если отрезков больше, чем влезло в страницу
;;          +1  сколько отрезков вышло (2 байта)
;;          +3  сами отрезки, но не больше 24: кластер (4), сколько (2)
;;
;; Метки: 1 - начало построения отрезков, 2 - конец.

        .module test_ext
        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_dir_open
        .globl  _fat_dir_open_root
        .globl  _fat_dir_next
        .globl  _fat_ent_attr
        .globl  _fat_ent_clus
        .globl  _fat_ent_name
        .globl  _fat_build_extents
        .globl  _fat_ext_count
        .globl  _fat_ext_overflow
        .globl  _fat_clus
        .globl  _fat_page_set
        .globl  _fat_page_check
        .globl  _fat_page_work
        .globl  _fat_page_pre
        .globl  _fat_page_dir
        .globl  _fat_port_shadow
        .globl  start

RESULT  .equ    0xA000
TABLE   .equ    0xA002
MAXFILE .equ    24
EXTREC  .equ    24              ; сколько отрезков выписываем на файл
DBGMARK .equ    0x33            ; метка времени для эмулятора, не бордюр

        .area   _DATA
te_count:       .ds     1
te_ptr:         .ds     2       ; куда писать следующую запись результата
te_files:       .ds     MAXFILE*4   ; первые кластеры найденных файлов
te_sub:         .ds     4       ; кластер первого подкаталога
te_has_sub:     .ds     1
te_saved_page:  .ds     1       ; что было в окне до нашего вмешательства

        .area   _CODE

start::
        ld      sp, #0x9F00
        xor     a
        ld      (te_count), a
        ld      (te_has_sub), a

        ;; Страницы называем сами - библиотека своей памяти не занимает.
        ;; Теневая копия 0x7FFD с погашенным номером страницы, но с живыми
        ;; старшими битами: если библиотека их затрёт, машина на железе
        ;; сменит ПЗУ, и это должно быть видно.
        ld      a, #0x10
        ld      (_fat_port_shadow), a
        ld      a, #1
        ld      (_fat_page_work), a
        ld      a, #3
        ld      (_fat_page_pre), a
        ld      a, #4
        ld      (_fat_page_dir), a
        call    _fat_page_check
        jr      nc, te_pages_ok
        ld      a, #3
        jp      te_fail
te_pages_ok:

        call    _sd_init
        jr      nc, te_init_ok
        ld      a, #1
        jp      te_fail
te_init_ok:
        call    _fat_mount
        jr      nc, te_mount_ok
        ld      a, #2
        jp      te_fail
te_mount_ok:

        call    _fat_dir_open_root
        jr      nc, te_scan
        ld      a, #4
        jp      te_fail

;; -- Собрать первые кластеры файлов --
te_scan:
        call    _fat_dir_next
        jr      c, te_io
        or      a
        jr      z, te_scanned

        ld      a, (_fat_ent_attr)
        and     #0x08
        jr      nz, te_scan     ; метка тома
        ld      a, (_fat_ent_attr)
        and     #0x10
        jr      z, te_is_file

        ;; Каталог: запомнить первый подходящий на случай пустого корня.
        ld      a, (te_has_sub)
        or      a
        jr      nz, te_scan
        ld      a, (_fat_ent_name)
        cp      #0x2E           ; "." и ".." не в счёт
        jr      z, te_scan
        ;; Скрытые и системные тоже мимо: Windows кладёт в корень свой
        ;; System Volume Information, и мерить на нём нечего.
        ld      a, (_fat_ent_attr)
        and     #0x06
        jr      nz, te_scan
        ld      hl, #te_sub
        ld      de, #_fat_ent_clus
        call    te_copy4
        ld      a, #1
        ld      (te_has_sub), a
        jr      te_scan

te_is_file:
        ld      a, (te_count)
        cp      #MAXFILE
        jr      nc, te_scan
        call    te_slot
        ld      de, #_fat_ent_clus
        call    te_copy4
        ld      a, (te_count)
        inc     a
        ld      (te_count), a
        jr      te_scan

te_scanned:
        ld      a, (te_count)
        or      a
        jr      nz, te_build

        ;; В корне одни каталоги - спускаемся в первый.
        ld      a, (te_has_sub)
        or      a
        jr      z, te_nowhere
        ld      hl, #_fat_clus
        ld      de, #te_sub
        call    te_copy4
        call    _fat_dir_open
        jr      c, te_io
        xor     a
        ld      (te_has_sub), a ; второй раз спускаться некуда
        jp      te_scan

te_nowhere:
        ld      a, #5
        jp      te_fail
te_io:
        ld      a, #4
        jp      te_fail

;; -- Свернуть цепочки в отрезки --
te_build:
        ld      hl, #TABLE
        ld      (te_ptr), hl

        ld      a, #1
        out     (DBGMARK), a

        ld      a, (te_count)
        ld      b, a
        ld      c, #0           ; номер файла
te_one:
        push    bc

        ld      a, c
        call    te_slot_a       ; HL -> кластер этого файла
        ld      de, #_fat_clus
        call    te_copy4_to_de

        ld      a, (_fat_page_work)
        call    _fat_build_extents
        push    af              ; CY здесь - переполнение или карта

        call    te_record

        pop     af
        pop     bc
        jr      nc, te_next
        ;; Переполнение - не беда теста: запись о нём уже выписана, идём
        ;; дальше. А вот ошибка карты - беда.
        ld      a, (_fat_ext_overflow)
        or      a
        jr      z, te_io
te_next:
        inc     c
        djnz    te_one

        ld      a, #2
        out     (DBGMARK), a
        xor     a
te_fail:
        ld      (RESULT+0), a
        ld      a, (te_count)
        ld      (RESULT+1), a
        halt

;; Выписать в результат то, что библиотека сложила в страницу.
;;
;; Страницу приходится включать заново: _fat_build_extents возвращает на
;; место ту, что была до него, - иначе вызывающий получал бы окно не с той
;; памятью, о которой думает.
te_record:
        ld      hl, (te_ptr)
        ld      a, (_fat_ext_overflow)
        ld      (hl), a
        inc     hl
        ld      de, (_fat_ext_count)
        ld      (hl), e
        inc     hl
        ld      (hl), d
        inc     hl

        ;; Сколько отрезков переписываем: не больше EXTREC.
        ld      a, d
        or      a
        jr      nz, te_rec_cap
        ld      a, e
        cp      #EXTREC+1
        jr      c, te_rec_n
te_rec_cap:
        ld      a, #EXTREC
te_rec_n:
        or      a
        jr      z, te_rec_done

        ;; Счётчик ставим ПОСЛЕ переключения страницы: _fat_page_set портит
        ;; BC (номер порта едет через него).
        push    af
        push    hl
        ld      a, (_fat_port_shadow)
        and     #0x07
        ld      (te_saved_page), a
        ld      a, (_fat_page_work)
        call    _fat_page_set
        pop     hl
        pop     af
        ld      b, a

        ld      de, #0xC002
te_rec_l:
        push    bc
        ld      b, #6
te_rec_b:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    te_rec_b
        pop     bc
        djnz    te_rec_l

        push    hl
        ld      a, (te_saved_page)
        call    _fat_page_set
        pop     hl

te_rec_done:
        ld      (te_ptr), hl
        ret

;; -- Мелочи --

;; HL -> ячейка te_files для файла номер (te_count).
te_slot:
        ld      a, (te_count)
te_slot_a:
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        ld      de, #te_files
        add     hl, de
        ret

;; (HL) <- (DE), 4 байта.
te_copy4:
        ld      b, #4
te_copy4_l:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    te_copy4_l
        ret

;; (DE) <- (HL), 4 байта.
te_copy4_to_de:
        ld      b, #4
te_c4d_l:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        djnz    te_c4d_l
        ret
