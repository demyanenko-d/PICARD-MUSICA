;; test_load.s - во что обходится загрузка настоящих файлов двумя путями.
;;
;; Берём первый подкаталог корня, выписываем оттуда все файлы, а потом
;; прогоняем их целиком: сначала через память (INIR в буфер, оттуда OTIR
;; на плату), потом прямо в порт. Оба прогона - по одним и
;; тем же файлам и в одном запуске, границы размечены записью в 0xFE.
;;
;; Считается всё, что есть в настоящей загрузке, а не только пересылка:
;; ходьба по цепочке кластеров, чтение секторов FAT, команда на каждый
;; сектор. Иначе число получилось бы красивее правды.
;;
;; Каталог выписывается ЗАРАНЕЕ, до чтения файлов: обход каталога и чтение
;; файла делят один буфер сектора, и вперемешку они бы затирали друг друга.
;;
;; Результат по 0xA000:
;;   +0   0 успех, 1 карта, 2 монтирование, 3 нет подкаталога,
;;        4 ошибка чтения, 5 цепочка кончилась раньше размера
;;   +1   сколько файлов прогнали
;; Таблица по 0xA200, по 8 байт на файл: кластер (4), размер (4).
;;
;; Метки: 1 - начало пути через память, 2 - начало прямого, 3 - конец.

        .module test_load
        .globl  _sd_init
        .globl  _sd_read_sector
        .globl  _sd_read_sector_to_port
        .globl  _fat_mount
        .globl  _fat_dir_open
        .globl  _fat_dir_open_root
        .globl  _fat_dir_next
        .globl  _fat_ent_name
        .globl  _fat_ent_attr
        .globl  _fat_ent_clus
        .globl  _fat_ent_size
        .globl  _fat_clus
        .globl  _fat_lba
        .globl  _fat_clus_to_lba
        .globl  _fat_next_clus
        .globl  _fat_spc
        .globl  _fat_sec_buf
        .globl  start

RESULT  .equ    0xA000
TABLE   .equ    0xA200
MAXFILE .equ    64

BUS_DAT .equ    0x67            ; тот же порт, что у драйвера карты
DBGMARK .equ    0x33            ; метка времени для эмулятора, не бордюр

        .area   _DATA
tl_count:       .ds     1
tl_secs:        .ds     2       ; секторов осталось в текущем файле
tl_in_clus:     .ds     1       ; секторов осталось в текущем кластере
tl_ptr:         .ds     2       ; место в таблице

        .area   _CODE

start::
        ld      sp, #0x9F00
        xor     a
        ld      (tl_count), a

        call    _sd_init
        jr      nc, tl_init_ok
        ld      a, #1
        jp      tl_fail
tl_init_ok:
        call    _fat_mount
        jr      nc, tl_mount_ok
        ld      a, #2
        jp      tl_fail
tl_mount_ok:

;; -- Найти первый подкаталог корня --
        call    _fat_dir_open_root
        jr      nc, tl_root_ok
        ld      a, #4
        jp      tl_fail
tl_root_ok:
tl_find:
        call    _fat_dir_next
        jr      c, tl_io
        or      a
        jr      z, tl_no_dir
        ld      a, (_fat_ent_attr)
        and     #0x10
        jr      z, tl_find
        ld      a, (_fat_ent_name)
        cp      #0x2E           ; "." и ".." не годятся
        jr      z, tl_find
        ;; Скрытые и системные тоже: Windows кладёт в корень свой System
        ;; Volume Information, и мерить на нём нечего.
        ld      a, (_fat_ent_attr)
        and     #0x06
        jr      nz, tl_find

        ;; Спускаемся в него.
        ld      hl, #_fat_clus
        ld      de, #_fat_ent_clus
        call    tl_copy4
        call    _fat_dir_open
        jr      nc, tl_collect
        ld      a, #4
        jp      tl_fail

tl_no_dir:
        ld      a, #3
        jp      tl_fail
tl_io:
        ld      a, #4
        jp      tl_fail

;; -- Выписать файлы каталога в таблицу --
tl_collect:
        ld      hl, #TABLE
        ld      (tl_ptr), hl
tl_next_ent:
        call    _fat_dir_next
        jr      c, tl_io
        or      a
        jr      z, tl_collected
        ld      a, (_fat_ent_attr)
        and     #0x18           ; каталог или метка тома - мимо
        jr      nz, tl_next_ent
        ld      a, (_fat_ent_size+3)
        or      a
        jr      nz, tl_next_ent ; больше 16 МБ не считаем

        ld      hl, (tl_ptr)
        ld      de, #_fat_ent_clus
        call    tl_copy4
        ld      de, #_fat_ent_size
        call    tl_copy4
        ld      (tl_ptr), hl

        ld      a, (tl_count)
        inc     a
        ld      (tl_count), a
        cp      #MAXFILE
        jr      c, tl_next_ent
tl_collected:

;; -- Прогон 1: через память --
        ld      hl, #ld_via_ram
        ld      (tl_call+1), hl
        ld      a, #1
        out     (DBGMARK), a
        call    tl_run_all
        jr      c, tl_run_failed

;; -- Прогон 2: напрямую --
        ld      hl, #ld_direct
        ld      (tl_call+1), hl
        ld      a, #2
        out     (DBGMARK), a
        call    tl_run_all
        jr      c, tl_run_failed

;; -- Прогон 3: только чтение, без выдачи на плату --
;;
;; Нужен, чтобы разложить время на части. Иначе видно только "один путь
;; быстрее другого", а сколько в этих числах самой пересылки и сколько
;; ходьбы по цепочке кластеров - не видно, и первое же расхождение с
;; расчётом не с чем сверить.
        ld      hl, #ld_read_only
        ld      (tl_call+1), hl
        ld      a, #3
        out     (DBGMARK), a
        call    tl_run_all
        jr      c, tl_run_failed

        ld      a, #4
        out     (DBGMARK), a
        xor     a
tl_fail:
        ld      (RESULT+0), a
        ld      a, (tl_count)
        ld      (RESULT+1), a
        halt

tl_run_failed:
        ld      a, e            ; код беды принесён в E
        jr      tl_fail

;; -- Прогнать все файлы таблицы --
;; CY=1 - беда, код в E.
tl_run_all:
        ld      a, (tl_count)
        or      a
        ret     z
        ld      b, a
        ld      hl, #TABLE
tl_file:
        push    bc
        push    hl              ; HL -> запись таблицы

        ld      de, #_fat_clus
        call    tl_copy4_to_de  ; _fat_clus = кластер, HL -> размер

        ;; Секторов в файле = (размер + 511) / 512.
        call    tl_sectors
        ld      (tl_secs), hl

        call    tl_load_file
        pop     hl
        pop     bc
        ret     c
        ld      de, #8
        add     hl, de
        djnz    tl_file
        or      a
        ret

;; Прочитать файл целиком. Вход: _fat_clus, tl_secs. CY=1 - беда, код в E.
tl_load_file:
        ld      hl, (tl_secs)
        ld      a, h
        or      l
        ret     z               ; пустой файл - читать нечего
;; Счётчик секторов внутри кластера - в памяти, а не в B: чтение сектора
;; портит B, у INIR и OTIR он счётчик, у прямого пути в нём старший адрес
;; порта. В B счётчик обнуляется, djnz уходит на 256 витков, и цепочка
;; кластеров почти не обходится - файл читается как сплошной.
tl_clus:
        call    _fat_clus_to_lba
        ld      a, (_fat_spc)
        ld      (tl_in_clus), a
tl_sector:
        ld      hl, #_fat_lba
        call    tl_load32
tl_call:
        call    ld_via_ram      ; адрес подставляется перед каждым прогоном
        jr      c, tl_read_err
        call    tl_inc_lba

        ld      hl, (tl_secs)
        dec     hl
        ld      (tl_secs), hl
        ld      a, h
        or      l
        jr      z, tl_file_done

        ld      hl, #tl_in_clus
        dec     (hl)
        jr      nz, tl_sector

        call    _fat_next_clus
        jr      nc, tl_clus
        ld      e, #5           ; цепочка кончилась, а сектора ещё нужны
        scf
        ret

tl_file_done:
        or      a
        ret
tl_read_err:
        ld      e, #4
        scf
        ret

;; -- Два способа прочитать сектор --
;;
;; Вход у обоих один: DEHL = номер сектора.

;; Старый: INIR в буфер, оттуда OTIR на плату - два прохода по 21 такту.
ld_via_ram:
        ld      ix, #_fat_sec_buf
        call    _sd_read_sector
        ret     c
        ld      hl, #_fat_sec_buf
        ld      c, #BUS_DAT
        ld      b, #0
        otir
        ld      b, #0
        otir
        or      a
        ret

;; Новый: с карты прямо в порт, развёрнутым циклом.
ld_direct:
        jp      _sd_read_sector_to_port

;; Только чтение в буфер - мерная линейка: то, что останется, если убрать
;; выдачу на плату целиком.
ld_read_only:
        ld      ix, #_fat_sec_buf
        jp      _sd_read_sector

;; -- Мелочи --

;; HL -> 32-битное число, выход DEHL = оно же.
tl_load32:
        inc     hl
        inc     hl
        inc     hl
        ld      d, (hl)
        dec     hl
        ld      e, (hl)
        dec     hl
        ld      a, (hl)
        dec     hl
        ld      l, (hl)
        ld      h, a
        ret

tl_inc_lba:
        ld      hl, #_fat_lba
        inc     (hl)
        ret     nz
        inc     hl
        inc     (hl)
        ret     nz
        inc     hl
        inc     (hl)
        ret     nz
        inc     hl
        inc     (hl)
        ret

;; (HL) <- (DE), 4 байта; HL уезжает вперёд.
tl_copy4:
        ld      b, #4
tl_copy4_l:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    tl_copy4_l
        ret

;; (DE) <- (HL), 4 байта; HL уезжает вперёд.
tl_copy4_to_de:
        ld      b, #4
tl_c4d_l:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        djnz    tl_c4d_l
        ret

;; Вход: HL -> размер (4 байта, старший заведомо 0). Выход: HL = секторов.
;;
;; Размер меньше 16 МБ, поэтому секторов меньше 32768 и хватает 16 бит.
tl_sectors:
        ld      e, (hl)         ; b0
        inc     hl
        ld      c, (hl)         ; b1
        inc     hl
        ld      b, (hl)         ; b2
        ld      h, b
        ld      l, c            ; HL = размер >> 8
        srl     h
        rr      l               ; HL = размер >> 9
        ;; Неполный последний сектор тоже читать.
        ld      a, e
        or      a
        jr      nz, tl_sec_up
        ld      a, c
        and     #1
        ret     z
tl_sec_up:
        inc     hl
        ret
