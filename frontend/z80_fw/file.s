;; file.s - открыть файл и читать из него.
;;
;; Открытый файл один. Его отрезки лежат в странице _fat_page_work,
;; дескриптор - по постоянному адресу. Раздачи памяти нет.
;;
;; Файл помнит свой носитель: устройство карты и параметры тома. Каталог
;; можно открыть на другом устройстве, файл читается со своего.
;;
;; Позиция переводится в номер сектора через отрезки, результат
;; кэшируется: пока чтение идёт вперёд по одному отрезку, пересчёт
;; сводится к сложению.
;;
;; Счётчики живут в памяти, а не в регистрах: чтение сектора портит всё,
;; включая B и IX.

        .module file
        .globl  _fat_open
        .globl  _fat_seek
        .globl  _fat_read_sectors
        .globl  _fat_read_bytes
        .globl  _fat_read_dest
        .globl  _fat_file_size
        .globl  _fat_file_pos
        .globl  _fat_file_ext_count

        .globl  _fat_ent_clus
        .globl  _fat_ent_size
        .globl  _fat_build_extents
        .globl  _fat_ext_count
        .globl  _fat_clus
        .globl  _fat_lba
        .globl  _fat_clus_to_lba
        .globl  _fat_spc_shift
        .globl  _fat_data_start
        .globl  fat_clus_to_lba_with
        .globl  _sd_use
        .globl  _sd_dev
        .globl  _fat_sec_buf
        .globl  _fat_page_work
        .globl  fat_page_enter
        .globl  fat_page_leave
        .globl  _sd_read_sector
        .globl  _sd_read_sector_to_port
        .globl  _sd_read_multi_to_port
        .globl  _sd_multi_count
        .globl  fat_copy4
        .globl  fat_zero4
        .globl  fat_add32
        .globl  fat_load_dehl
        .globl  fat_shl_n

        .area   _DATA

;; Дескриптор файла. Размер, позиция и число отрезков видны снаружи как
;; есть.
_fat_file_size::      .ds  4
_fat_file_pos::       .ds  4    ; позиция в байтах
_fat_file_ext_count:: .ds  2    ; сколько отрезков
fh_eidx:              .ds  2    ; какой отрезок сейчас в кэше
fh_estart:            .ds  4    ; сектор файла, с которого он начинается
fh_esecs:             .ds  4    ; сколько в нём секторов
fh_elba:              .ds  4    ; сектор тома, с которого он начинается
fh_dev:               .ds  1    ; устройство карты (_sd_use)
fh_shift:             .ds  1    ; log2 секторов в кластере тома
fh_data:              .ds  4    ; первый сектор области данных тома

_fat_read_dest::.ds     1       ; 0 - в память по IX, 1 - прямо в порт платы

ff_tmp:         .ds     4
ff_sec:         .ds     4       ; номер сектора внутри файла
ff_off:         .ds     2       ; смещение внутри сектора
ff_left:        .ds     2       ; сколько ещё осталось прочитать
ff_chunk:       .ds     2       ; сколько берём на этом витке
ff_dst:         .ds     2       ; куда класть в точном режиме
ff_in_ext:      .ds     4       ; секторов до конца текущего отрезка
ff_batch:       .ds     1       ; сколько взяли одной командой

        .area   _CODE

;; -- Открытие --
;;
;; Вход:  _fat_ent_clus и _fat_ent_size заполнены записью каталога.
;; Выход: CY=0. CY=1 - карта не ответила либо файл раздроблен сильнее,
;;        чем держит страница (что именно, скажет _fat_ext_overflow).
_fat_open::
        ld      hl, #_fat_clus
        ld      de, #_fat_ent_clus
        call    fat_copy4
        ld      a, (_fat_page_work)
        call    _fat_build_extents
        ret     c

        ld      a, (_sd_dev)
        ld      (fh_dev), a
        ld      a, (_fat_spc_shift)
        ld      (fh_shift), a
        ld      hl, #fh_data
        ld      de, #_fat_data_start
        call    fat_copy4

        ld      hl, #_fat_file_size
        ld      de, #_fat_ent_size
        call    fat_copy4
        ld      hl, #_fat_file_pos
        call    fat_zero4
        ld      de, (_fat_ext_count)
        ld      (_fat_file_ext_count), de

        ;; Пустой файл: отрезков нет, кэшировать нечего.
        ld      a, d
        or      e
        ret     z               ; CY=0 от or

        ld      hl, #fh_estart
        call    fat_zero4
        ld      hl, #0
        jp      fat_cache_ext

;; -- Кэш отрезка --
;;
;; Вход: HL = номер отрезка; fh_estart уже выставлен вызывающим.
;; Кладёт в дескриптор первый сектор тома этого отрезка и его длину.
;; Выход: CY=0.
fat_cache_ext:
        ld      (fh_eidx), hl

        ;; Достать запись из страницы: заголовок 2 байта, дальше по 6.
        ld      a, (_fat_page_work)
        call    fat_page_enter
        ld      d, h
        ld      e, l
        add     hl, hl          ; x2
        add     hl, de          ; x3
        add     hl, hl          ; x6
        ld      de, #0xC002
        add     hl, de

        ex      de, hl          ; первый кластер отрезка
        ld      hl, #_fat_clus
        call    fat_copy4
        ex      de, hl
        ld      e, (hl)
        inc     hl
        ld      d, (hl)         ; сколько кластеров подряд
        push    de

        call    fat_page_leave

        ;; Сектор тома, с которого начинается отрезок.
        ld      a, (fh_shift)
        ld      bc, #fh_data
        call    fat_clus_to_lba_with
        ld      hl, #fh_elba
        ld      de, #_fat_lba
        call    fat_copy4

        ;; Секторов в отрезке = кластеров * секторов в кластере.
        pop     de
        ld      hl, #fh_esecs
        ld      (hl), e
        inc     hl
        ld      (hl), d
        inc     hl
        ld      (hl), #0
        inc     hl
        ld      (hl), #0
        ld      a, (fh_shift)
        ld      hl, #fh_esecs
        call    fat_shl_n
        or      a
        ret

;; -- Позиционирование --

;; Установить позицию. Вход: DEHL = смещение в байтах (DE старшее слово).
_fat_seek::
        ld      (_fat_file_pos), hl
        ld      (_fat_file_pos+2), de
        ret

;; Разложить позицию на номер сектора и смещение в нём, подвести кэш
;; отрезка под этот сектор и посчитать _fat_lba.
;;
;; CY=1 - позиция за последним отрезком.
fat_locate:
        ld      hl, #ff_sec
        ld      de, #_fat_file_pos
        call    fat_copy4

        ;; Смещение в секторе - младшие девять бит.
        ld      a, (ff_sec)
        ld      (ff_off), a
        ld      a, (ff_sec+1)
        and     #1
        ld      (ff_off+1), a

        ;; Номер сектора - позиция >> 9. Девять сдвигов подряд стоили бы
        ;; около двух тысяч тактов на КАЖДЫЙ сектор, а сдвиг на восемь
        ;; разрядов - это просто сдвижка байт: pos>>9 = (pos>>8)>>1.
        ld      a, (ff_sec+1)
        ld      (ff_sec+0), a
        ld      a, (ff_sec+2)
        ld      (ff_sec+1), a
        ld      a, (ff_sec+3)
        ld      (ff_sec+2), a
        xor     a
        ld      (ff_sec+3), a
        ld      hl, #ff_sec
        call    ff_shr32

fl_again:
        ;; Не левее ли сектор, чем начало отрезка в кэше?
        ld      hl, #ff_sec
        ld      de, #fh_estart
        call    ff_cmp32        ; CY=1, если ff_sec < ESTART
        jr      c, fl_rewind

        ;; ff_tmp = ff_sec - ESTART
        ld      hl, #ff_tmp
        ld      de, #ff_sec
        call    fat_copy4
        ld      hl, #ff_tmp
        ld      de, #fh_estart
        call    ff_sub32

        ld      hl, #ff_tmp
        ld      de, #fh_esecs
        call    ff_cmp32        ; CY=1, если смещение внутри отрезка
        jr      nc, fl_forward

        ;; Попали. Заодно считаем, сколько секторов осталось до конца
        ;; отрезка: по ним можно взять пачку одной командой, а за границей
        ;; отрезка сектора уже не подряд, и командой их не охватить.
        ld      hl, #ff_in_ext
        ld      de, #fh_esecs
        call    fat_copy4
        ld      hl, #ff_in_ext
        ld      de, #ff_tmp
        call    ff_sub32

        ;; lba = ELBA + смещение.
        ld      hl, #_fat_lba
        ld      de, #fh_elba
        call    fat_copy4
        ld      hl, #_fat_lba
        ld      de, #ff_tmp
        call    fat_add32
        or      a
        ret

fl_rewind:
        ;; Назад - перебор отрезков с нуля.
        ld      hl, #fh_estart
        call    fat_zero4
        ld      hl, #0
        call    fat_cache_ext
        jr      fl_again

fl_forward:
        ;; Сектор дальше этого отрезка - шагнуть на следующий.
        ld      hl, (fh_eidx)
        inc     hl
        ld      de, (_fat_file_ext_count)
        push    hl
        or      a
        sbc     hl, de
        pop     hl
        ccf
        ret     c               ; отрезки кончились

        ;; ESTART += ESECS.
        push    hl
        ld      hl, #fh_estart
        ld      de, #fh_esecs
        call    fat_add32
        pop     hl
        call    fat_cache_ext
        jr      fl_again

;; -- Быстрый режим: целыми секторами --
;;
;; Вход: BC = сколько секторов; IX = куда, если _fat_read_dest = 0.
;;
;; Позиция обязана стоять на границе сектора: карта отдаёт сектор целиком.
;; Не на границе - отказ, а не подгонка.
;; Чтение - с устройства файла, после него устройство прежнее. Вход и выход
;; те же, что у frs_entry и frb_entry: BC, IX; CY.
_fat_read_sectors::
        ld      hl, #frs_entry
        jr      ff_on_dev
_fat_read_bytes::
        ld      hl, #frb_entry
ff_on_dev:
        ld      a, (_sd_dev)
        push    af
        push    hl
        push    bc
        ld      a, (fh_dev)
        call    _sd_use
        pop     bc
        pop     hl
        call    ff_jp_hl
        pop     de              ; D = прежнее устройство
        push    af
        ld      a, d
        call    _sd_use
        pop     af
        ret
ff_jp_hl:
        jp      (hl)

frs_entry:
        ld      a, b
        or      c
        ret     z
        ld      (ff_left), bc

        call    fat_locate
        ret     c
        ld      hl, (ff_off)
        ld      a, h
        or      l
        jr      z, frs_loop
        scf                     ; позиция не на границе сектора
        ret

frs_loop:
        ;; Пачкой берём только на прямом пути. В память сектора кладёт INIR
        ;; по одному, и команда там не главная статья; а на плату идёт
        ;; сплошной поток, и переспрашивать карту на каждые 512 байт -
        ;; ровно те 2 такта на байт, ради которых CMD18 и заводился.
        ld      a, (_fat_read_dest)
        or      a
        jr      z, frs_single

        call    frs_batch_n
        cp      #2
        jr      c, frs_single   ; на один сектор команду городить незачем
        ld      (_sd_multi_count), a
        ld      (ff_batch), a

        ld      hl, #_fat_lba
        call    fat_load_dehl
        call    _sd_read_multi_to_port
        ret     c

        ld      a, (ff_batch)
        call    frs_advance_n
        ld      hl, (ff_left)
        ld      a, (ff_batch)
        ld      e, a
        ld      d, #0
        or      a
        sbc     hl, de
        jr      frs_tail

frs_single:
        ld      hl, #_fat_lba
        call    fat_load_dehl
        ld      a, (_fat_read_dest)
        or      a
        jr      nz, frs_port
        call    _sd_read_sector ; IX сам уезжает на сектор вперёд
        jr      frs_after
frs_port:
        call    _sd_read_sector_to_port
frs_after:
        ret     c

        call    frs_advance
        ld      hl, (ff_left)
        dec     hl

frs_tail:
        ld      (ff_left), hl
        ld      a, h
        or      l
        jr      z, frs_done
        call    fat_locate      ; дальше может быть уже другой отрезок
        ret     c
        jp      frs_loop
frs_done:
        or      a
        ret

;; Сколько секторов можно взять одной командой: меньшее из "сколько ещё
;; заказано" и "сколько осталось в отрезке", но не больше 255 - столько
;; помещается в счётчик CMD18.
frs_batch_n:
        ld      a, (ff_in_ext+3)
        ld      hl, #ff_in_ext+2
        or      (hl)
        jr      nz, frs_bn_many
        ld      a, (ff_in_ext+1)
        or      a
        jr      nz, frs_bn_many
        ld      a, (ff_in_ext)
        jr      frs_bn_have
frs_bn_many:
        ld      a, #255
frs_bn_have:
        ld      c, a
        ld      hl, (ff_left)
        ld      a, h
        or      a
        jr      nz, frs_bn_use_c        ; заказано заведомо больше
        ld      a, l
        cp      c
        ret     c                       ; заказано меньше - им и ограничимся
frs_bn_use_c:
        ld      a, c
        ret

;; Сдвинуть позицию на 512 байт вперёд.
frs_advance:
        ld      a, #1

;; Сдвинуть позицию на A секторов вперёд, то есть на A*512 байт: к
;; старшим трём байтам позиции прибавить A*2.
frs_advance_n:
        ld      l, a
        ld      h, #0
        add     hl, hl
        ld      de, (_fat_file_pos+1)
        add     hl, de
        ld      (_fat_file_pos+1), hl
        ret     nc
        ld      hl, #_fat_file_pos+3
        inc     (hl)
        ret

;; -- Точный режим: сколько попросили, столько и отдадим --
;;
;; Вход: BC = сколько байт, IX = куда.
;;
;; Внутри всё равно читаются целые секторы - иначе карта не умеет, - но
;; наружу это не видно: началу и хвосту достаётся кусок через буфер.
frb_entry:
        ld      a, b
        or      c
        ret     z
        ld      (ff_left), bc
        push    ix
        pop     hl
        ld      (ff_dst), hl

frb_loop:
        call    fat_locate
        ret     c

        ld      hl, #_fat_lba
        call    fat_load_dehl
        ld      ix, #_fat_sec_buf
        call    _sd_read_sector
        ret     c

        ;; Сколько отдать с этого сектора: до его конца, но не больше, чем
        ;; осталось.
        ld      hl, #512
        ld      de, (ff_off)
        or      a
        sbc     hl, de          ; сколько ещё лежит в секторе
        ld      de, (ff_left)
        push    hl
        or      a
        sbc     hl, de
        pop     hl
        jr      c, frb_take     ; в секторе меньше - берём сколько есть
        ex      de, hl          ; иначе берём весь остаток заказа
frb_take:
        ld      (ff_chunk), hl

        ;; Переложить кусок из буфера.
        ld      hl, #_fat_sec_buf
        ld      de, (ff_off)
        add     hl, de
        ld      de, (ff_dst)
        ld      bc, (ff_chunk)
        ldir
        ld      (ff_dst), de

        ;; Подвинуть позицию и уменьшить остаток.
        ld      hl, #ff_tmp
        ld      de, (ff_chunk)
        ld      (hl), e
        inc     hl
        ld      (hl), d
        inc     hl
        ld      (hl), #0
        inc     hl
        ld      (hl), #0
        ld      hl, #_fat_file_pos
        ld      de, #ff_tmp
        call    fat_add32

        ld      hl, (ff_left)
        ld      de, (ff_chunk)
        or      a
        sbc     hl, de
        ld      (ff_left), hl
        ld      a, h
        or      l
        jr      nz, frb_loop

        ld      hl, (ff_dst)
        push    hl
        pop     ix              ; приёмник - там, где остановились
        or      a
        ret

;; -- 32-битная мелочь --

;; (HL) -= (DE)
ff_sub32:
        ld      b, #4
        or      a
ff_s32:
        ld      a, (de)
        ld      c, a
        ld      a, (hl)
        sbc     a, c
        ld      (hl), a
        inc     hl
        inc     de
        djnz    ff_s32
        ret

;; Сравнить (HL) с (DE). CY=1, если (HL) < (DE). Указатели не портятся.
;;
;; Просто вычитаем побайтово с заёмом и смотрим, что осталось в переносе:
;; ни inc, ни djnz его не трогают.
ff_cmp32:
        push    hl
        push    de
        ld      b, #4
        or      a
ff_k32:
        ld      a, (de)
        ld      c, a
        ld      a, (hl)
        sbc     a, c
        inc     hl
        inc     de
        djnz    ff_k32
        pop     de
        pop     hl
        ret

;; (HL) >>= 1
ff_shr32:
        ld      de, #3
        add     hl, de
        or      a
        ld      b, #4
ff_r32:
        rr      (hl)
        dec     hl
        djnz    ff_r32
        ret
