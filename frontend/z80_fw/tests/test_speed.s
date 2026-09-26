;; test_speed.s - сколько на самом деле выходит, когда файл грузит сама
;; библиотека.
;;
;; Замер идёт настоящим путём, тем же, каким пойдёт плеер: найти файл в
;; каталоге,
;; построить отрезки, прочитать целиком на плату.
;;
;; Открытие меряется отдельно от чтения: это разные вещи, и валить их в
;; одно число значило бы прятать, что открытие почти бесплатно.
;;
;; Результат по 0xA000:
;;   +0   0 успех, 1 карта, 2 монтирование, 3 страницы, 4 чтение,
;;        5 не нашли, где брать файлы, 6 открыть не удалось
;;   +1   сколько файлов прогнали
;;   +2   суммарно байт (4)
;;
;; Метки: 1 - начало открытия очередного файла, 2 - начало его чтения,
;;        3 - конец. Так видно и то и другое.

        .module test_speed
        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_dir_open
        .globl  _fat_dir_open_root
        .globl  _fat_dir_next
        .globl  _fat_ent_attr
        .globl  _fat_ent_name
        .globl  _fat_ent_clus
        .globl  _fat_ent_size
        .globl  _fat_open
        .globl  _fat_read_sectors
        .globl  _fat_read_dest
        .globl  _fat_file_size
        .globl  _fat_clus
        .globl  _fat_page_check
        .globl  _fat_page_work
        .globl  _fat_page_pre
        .globl  _fat_page_dir
        .globl  _fat_port_shadow
        .globl  start

RESULT  .equ    0xA000
TABLE   .equ    0xA200          ; кластер+размер на файл, по 8 байт
MAXFILE .equ    16
DBGMARK .equ    0x33            ; метка времени для эмулятора, не бордюр

        .area   _DATA
ts_count:       .ds     1
ts_ptr:         .ds     2
ts_sub:         .ds     4
ts_has_sub:     .ds     1
ts_total:       .ds     4
ts_secs:        .ds     2

        .area   _CODE

start::
        ld      sp, #0x9F00
        xor     a
        ld      (ts_count), a
        ld      (ts_has_sub), a
        ld      hl, #ts_total
        call    ts_zero4

        ;; Страницы берём НЕЧЕРЕДУЮЩИЕСЯ: на 128K банки 1, 3, 5 и 7
        ;; тормозит ULA. Свободных ровно три - 0, 4, 6, - и нам ровно
        ;; столько и нужно.
        ld      a, #0x10
        ld      (_fat_port_shadow), a
        ld      a, #0
        ld      (_fat_page_work), a
        ld      a, #4
        ld      (_fat_page_pre), a
        ld      a, #6
        ld      (_fat_page_dir), a
        call    _fat_page_check
        jp      c, ts_bad_pages

        call    _sd_init
        jp      c, ts_no_card
        call    _fat_mount
        jp      c, ts_no_mount

        call    _fat_dir_open_root
        jp      c, ts_io

;; -- Собрать файлы --
ts_scan:
        call    _fat_dir_next
        jp      c, ts_io
        or      a
        jr      z, ts_scanned

        ld      a, (_fat_ent_attr)
        and     #0x08
        jr      nz, ts_scan             ; метка тома
        ld      a, (_fat_ent_attr)
        and     #0x10
        jr      z, ts_is_file

        ld      a, (ts_has_sub)
        or      a
        jr      nz, ts_scan
        ld      a, (_fat_ent_name)
        cp      #0x2E
        jr      z, ts_scan
        ld      a, (_fat_ent_attr)
        and     #0x06                   ; скрытые и системные мимо
        jr      nz, ts_scan
        ld      hl, #ts_sub
        ld      de, #_fat_ent_clus
        call    ts_copy4
        ld      a, #1
        ld      (ts_has_sub), a
        jr      ts_scan

ts_is_file:
        ld      a, (ts_count)
        cp      #MAXFILE
        jr      nc, ts_scan
        call    ts_slot
        ld      de, #_fat_ent_clus
        call    ts_copy4
        ld      de, #_fat_ent_size
        call    ts_copy4
        ld      a, (ts_count)
        inc     a
        ld      (ts_count), a
        jr      ts_scan

ts_scanned:
        ld      a, (ts_count)
        or      a
        jr      nz, ts_go
        ld      a, (ts_has_sub)
        or      a
        jp      z, ts_nowhere
        ld      hl, #_fat_clus
        ld      de, #ts_sub
        call    ts_copy4
        call    _fat_dir_open
        jp      c, ts_io
        xor     a
        ld      (ts_has_sub), a
        jp      ts_scan

;; -- Прогнать каждый файл целиком на плату --
ts_go:
        ld      a, #1
        ld      (_fat_read_dest), a     ; прямо в порт платы
        ld      hl, #TABLE
        ld      (ts_ptr), hl
        ld      a, (ts_count)
        ld      b, a
ts_one:
        push    bc

        ld      a, #1
        out     (DBGMARK), a

        ;; Открыть: кластер и размер уже собраны, подсовываем их напрямую.
        ld      hl, (ts_ptr)
        ld      de, #_fat_ent_clus
        call    ts_copy4_to_de
        ld      de, #_fat_ent_size
        call    ts_copy4_to_de
        ld      (ts_ptr), hl
        xor     a
        call    _fat_open
        jp      c, ts_openfail

        ;; Секторов в файле = (размер + 511) / 512.
        call    ts_sectors
        ld      (ts_secs), hl

        ;; Суммарный объём - по размеру, а не по секторам: скорость надо
        ;; считать на полезные байты.
        ld      hl, #ts_total
        ld      de, #_fat_file_size
        call    ts_add32

        ld      a, #2
        out     (DBGMARK), a

        ld      bc, (ts_secs)
        ld      a, b
        or      c
        jr      z, ts_next
        call    _fat_read_sectors
        jp      c, ts_io
ts_next:
        pop     bc
        djnz    ts_one

        ld      a, #3
        out     (DBGMARK), a
        xor     a
ts_fail:
        ld      (RESULT+0), a
        ld      a, (ts_count)
        ld      (RESULT+1), a
        ld      hl, #RESULT+2
        ld      de, #ts_total
        call    ts_copy4
        halt

ts_bad_pages:   ld a, #3
                jr ts_fail
ts_no_card:     ld a, #1
                jr ts_fail
ts_no_mount:    ld a, #2
                jr ts_fail
ts_io:          ld a, #4
                jr ts_fail
ts_nowhere:     ld a, #5
                jr ts_fail
ts_openfail:    ld a, #6
                jr ts_fail

;; -- Мелочи --

ts_slot:
        ld      a, (ts_count)
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        add     hl, hl          ; по 8 байт на файл
        ld      de, #TABLE
        add     hl, de
        ret

;; HL -> размер (старший байт заведомо 0). Выход: HL = секторов.
ts_sectors:
        ld      hl, #_fat_ent_size
        ld      e, (hl)
        inc     hl
        ld      c, (hl)
        inc     hl
        ld      b, (hl)
        ld      h, b
        ld      l, c
        srl     h
        rr      l
        ld      a, e
        or      a
        jr      nz, ts_sec_up
        ld      a, c
        and     #1
        ret     z
ts_sec_up:
        inc     hl
        ret

ts_copy4:
        ld      b, #4
ts_c4:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    ts_c4
        ret

ts_copy4_to_de:
        ld      b, #4
ts_c4d:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        djnz    ts_c4d
        ret

ts_zero4:
        xor     a
        ld      b, #4
ts_z4:
        ld      (hl), a
        inc     hl
        djnz    ts_z4
        ret

ts_add32:
        ld      b, #4
        or      a
ts_a32:
        ld      a, (de)
        adc     a, (hl)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    ts_a32
        ret
