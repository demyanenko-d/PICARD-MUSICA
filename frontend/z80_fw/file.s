;; file.s - открыть файл и читать из него.
;;
;; Файлов ровно два: рабочий (номер 0) и префетч (номер 1). Каждому своя
;; страница под отрезки, названная при инициализации. Всё состояние - в
;; двух постоянных структурах, раздачи памяти нет.
;;
;; Позиция переводится в номер сектора через отрезки, результат
;; кэшируется: пока чтение идёт вперёд по одному отрезку, пересчёт
;; сводится к сложению.
;;
;; Счётчики живут в памяти, а не в регистрах: чтение сектора портит всё,
;; включая B и IX.

        .module file
        .globl  _fat_use
        .globl  _fat_find
        .globl  _fat_open
        .globl  _fat_adopt
        .globl  _fat_seek
        .globl  _fat_read_sectors
        .globl  _fat_read_bytes
        .globl  _fat_read_dest
        .globl  _fat_file_size
        .globl  _fat_file_pos
        .globl  _fat_file_ext_count

        .globl  _fat_dir_next
        .globl  _fat_ent_name
        .globl  _fat_ent_clus
        .globl  _fat_ent_size
        .globl  _fat_build_extents
        .globl  _fat_ext_count
        .globl  _fat_clus
        .globl  _fat_lba
        .globl  _fat_clus_to_lba
        .globl  _fat_spc_shift
        .globl  _fat_sec_buf
        .globl  _fat_lower
        .globl  _fat_page_set
        .globl  _fat_page_work
        .globl  _fat_page_pre
        .globl  _fat_port_shadow
        .globl  _sd_read_sector
        .globl  _sd_read_sector_to_port
        .globl  _sd_read_multi_to_port
        .globl  _sd_multi_count

;; Смещения полей дескриптора.
FH_PAGE   .equ  0               ; страница с отрезками
FH_SIZE   .equ  1               ; размер файла (4)
FH_POS    .equ  5               ; текущая позиция в байтах (4)
FH_ECNT   .equ  9               ; сколько отрезков (2)
FH_EIDX   .equ  11              ; какой отрезок сейчас в кэше (2)
FH_ESTART .equ  13              ; сектор файла, с которого он начинается (4)
FH_ESECS  .equ  17              ; сколько в нём секторов (4)
FH_ELBA   .equ  21              ; сектор тома, с которого он начинается (4)
FH_LEN    .equ  25

        .area   _DATA

fat_file0:      .ds     FH_LEN
fat_file1:      .ds     FH_LEN
fat_fh:         .ds     2       ; с каким работаем сейчас

_fat_read_dest::.ds     1       ; 0 - в память по IX, 1 - прямо в порт платы

;; Наружу отдаём копиями, чтобы раскладку дескриптора можно было менять.
_fat_file_size::      .ds  4
_fat_file_pos::       .ds  4
_fat_file_ext_count:: .ds  2

fat_find_name:  .ds     2
ff_tmp:         .ds     4
ff_run:         .ds     2       ; кластеров в отрезке
ff_eidx:        .ds     2       ; номер отрезка, который кладём в кэш
ff_sec:         .ds     4       ; номер сектора внутри файла
ff_off:         .ds     2       ; смещение внутри сектора
ff_left:        .ds     2       ; сколько ещё осталось прочитать
ff_chunk:       .ds     2       ; сколько берём на этом витке
ff_dst:         .ds     2       ; куда класть в точном режиме
ff_in_ext:      .ds     4       ; секторов до конца текущего отрезка
ff_batch:       .ds     1       ; сколько взяли одной командой
ff_saved_page:  .ds     1

        .area   _CODE

;; -- Выбор файла --

;; A = 0 рабочий, 1 префетч.
_fat_use::
        or      a
        jr      nz, fu_one
        ld      hl, #fat_file0
        jr      fu_set
fu_one:
        ld      hl, #fat_file1
fu_set:
        ld      (fat_fh), hl
        ret

;; HL = поле дескриптора со смещением A. Портит DE.
fh_field:
        ld      hl, (fat_fh)
        ld      d, #0
        ld      e, a
        add     hl, de
        ret

;; -- Поиск по имени --
;;
;; Каталог должен быть уже открыт (_fat_dir_open). Сравнение без учёта
;; регистра, и для латиницы, и для кириллицы.
;;
;; Вход:  HL -> искомое имя, CP866, ноль в конце.
;; Выход: A=1 нашли, всё в _fat_ent_*; A=0 не нашли; CY=1 ошибка карты.
_fat_find::
        ld      (fat_find_name), hl
ffn_next:
        call    _fat_dir_next
        ret     c
        or      a
        ret     z
        ld      hl, (fat_find_name)
        ld      de, #_fat_ent_name
        call    fat_name_eq
        jr      nz, ffn_next
        ld      a, #1
        or      a
        ret

;; Сравнить строки (HL) и (DE) без учёта регистра. Z=1 - совпали.
fat_name_eq:
        ld      a, (de)
        call    _fat_lower
        ld      c, a
        ld      a, (hl)
        call    _fat_lower
        cp      c
        ret     nz
        or      a
        ret     z               ; обе кончились разом
        inc     hl
        inc     de
        jr      fat_name_eq

;; -- Открытие --
;;
;; Вход:  A = номер файла (0/1); _fat_ent_clus и _fat_ent_size заполнены
;;        поиском по каталогу.
;; Выход: CY=0. CY=1 - карта не ответила либо файл раздроблен сильнее,
;;        чем держит страница (что именно, скажет _fat_ext_overflow).
_fat_open::
        push    af
        call    _fat_use
        pop     af

        ;; Страница под отрезки: у рабочего своя, у префетча своя.
        or      a
        jr      nz, fo_pre
        ld      a, (_fat_page_work)
        jr      fo_page
fo_pre:
        ld      a, (_fat_page_pre)
fo_page:
        ld      hl, (fat_fh)
        ld      (hl), a         ; FH_PAGE = 0
        ld      (ff_saved_page), a

        ld      hl, #_fat_clus
        ld      de, #_fat_ent_clus
        call    ff_copy4
        ld      a, (ff_saved_page)
        call    _fat_build_extents
        ret     c

        ld      a, #FH_SIZE
        call    fh_field
        ld      de, #_fat_ent_size
        call    ff_copy4

        ld      a, #FH_POS
        call    fh_field
        call    ff_zero4

        ld      a, #FH_ECNT
        call    fh_field
        ld      de, (_fat_ext_count)
        ld      (hl), e
        inc     hl
        ld      (hl), d

        ;; Пустой файл: отрезков нет, кэшировать нечего.
        ld      a, d
        or      e
        jr      z, fo_publish

        ld      a, #FH_ESTART
        call    fh_field
        call    ff_zero4
        ld      hl, #0
        call    fat_cache_ext
fo_publish:
        call    fat_publish
        or      a
        ret

;; -- Принять префетченный файл --
;;
;; Файл открыт как префетч (номер 1) и становится рабочим. Диск не
;; трогается: меняются ролями страницы и переезжает дескриптор.
;;
;; Именно меняются, а не "берём префетчевую": иначе следующий префетч лёг
;; бы в страницу с отрезками играющего файла.
_fat_adopt::
        ld      a, (_fat_page_work)
        ld      b, a
        ld      a, (_fat_page_pre)
        ld      (_fat_page_work), a
        ld      a, b
        ld      (_fat_page_pre), a

        ld      hl, #fat_file1
        ld      de, #fat_file0
        ld      bc, #FH_LEN
        ldir

        ;; Префетч пуст: отрезков ноль, иначе случайное чтение уедет по
        ;; чужой странице.
        ld      hl, #fat_file1 + FH_ECNT
        ld      (hl), #0
        inc     hl
        ld      (hl), #0

        xor     a
        call    _fat_use
        jp      fat_publish

;; Скопировать наружу то, что вызывающему может понадобиться.
fat_publish:
        ld      a, #FH_SIZE
        call    fh_field
        ex      de, hl
        ld      hl, #_fat_file_size
        call    ff_copy4
        ld      a, #FH_POS
        call    fh_field
        ex      de, hl
        ld      hl, #_fat_file_pos
        call    ff_copy4
        ld      a, #FH_ECNT
        call    fh_field
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ld      (_fat_file_ext_count), de
        ret

;; -- Кэш отрезка --
;;
;; Вход: HL = номер отрезка; FH_ESTART уже выставлен вызывающим.
;; Кладёт в дескриптор первый сектор тома этого отрезка и его длину.
fat_cache_ext:
        ld      (ff_eidx), hl

        ld      a, #FH_EIDX
        call    fh_field
        ld      de, (ff_eidx)
        ld      (hl), e
        inc     hl
        ld      (hl), d

        ;; Достать запись из страницы: заголовок 2 байта, дальше по 6.
        ld      a, (_fat_port_shadow)
        and     #0x07
        ld      (ff_saved_page), a
        ld      a, #FH_PAGE
        call    fh_field
        ld      a, (hl)
        call    _fat_page_set

        ld      hl, (ff_eidx)
        push    hl
        add     hl, hl          ; x2
        pop     de
        add     hl, de          ; x3
        add     hl, hl          ; x6
        ld      de, #0xC002
        add     hl, de

        ld      de, #ff_tmp     ; первый кластер отрезка
        ld      b, #4
fce_c:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        djnz    fce_c
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ld      (ff_run), de    ; сколько кластеров подряд

        ld      a, (ff_saved_page)
        call    _fat_page_set

        ;; Сектор тома, с которого начинается отрезок.
        ld      hl, #_fat_clus
        ld      de, #ff_tmp
        call    ff_copy4
        call    _fat_clus_to_lba
        ld      a, #FH_ELBA
        call    fh_field
        ld      de, #_fat_lba
        call    ff_copy4

        ;; Секторов в отрезке = кластеров * секторов в кластере.
        ld      hl, #ff_tmp
        ld      de, (ff_run)
        ld      (hl), e
        inc     hl
        ld      (hl), d
        inc     hl
        ld      (hl), #0
        inc     hl
        ld      (hl), #0
        ld      a, (_fat_spc_shift)
        or      a
        jr      z, fce_no_shift
fce_shift:
        push    af
        ld      hl, #ff_tmp
        call    ff_shl32
        pop     af
        dec     a
        jr      nz, fce_shift
fce_no_shift:
        ld      a, #FH_ESECS
        call    fh_field
        ld      de, #ff_tmp
        call    ff_copy4
        or      a
        ret

;; -- Позиционирование --

;; Установить позицию. Вход: DEHL = смещение в байтах (DE старшее слово).
_fat_seek::
        push    de
        push    hl
        ld      a, #FH_POS
        call    fh_field
        pop     de
        ld      (hl), e
        inc     hl
        ld      (hl), d
        pop     de
        inc     hl
        ld      (hl), e
        inc     hl
        ld      (hl), d
        jp      fat_publish

;; Разложить позицию на номер сектора и смещение в нём, подвести кэш
;; отрезка под этот сектор и посчитать _fat_lba.
;;
;; CY=1 - позиция за последним отрезком.
fat_locate:
        ld      a, #FH_POS
        call    fh_field
        ex      de, hl
        ld      hl, #ff_sec
        call    ff_copy4

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
        ld      a, #FH_ESTART
        call    fh_field
        ex      de, hl
        ld      hl, #ff_sec
        call    ff_cmp32        ; CY=1, если ff_sec < ESTART
        jr      c, fl_rewind

        ;; ff_tmp = ff_sec - ESTART
        ld      hl, #ff_tmp
        ld      de, #ff_sec
        call    ff_copy4
        ld      a, #FH_ESTART
        call    fh_field
        ex      de, hl
        ld      hl, #ff_tmp
        call    ff_sub32

        ld      a, #FH_ESECS
        call    fh_field
        ex      de, hl
        ld      hl, #ff_tmp
        call    ff_cmp32        ; CY=1, если смещение внутри отрезка
        jr      nc, fl_forward

        ;; Попали. Заодно считаем, сколько секторов осталось до конца
        ;; отрезка: по ним можно взять пачку одной командой, а за границей
        ;; отрезка сектора уже не подряд, и командой их не охватить.
        ld      a, #FH_ESECS
        call    fh_field
        ex      de, hl
        ld      hl, #ff_in_ext
        call    ff_copy4
        ld      hl, #ff_in_ext
        ld      de, #ff_tmp
        call    ff_sub32

        ;; lba = ELBA + смещение.
        ld      a, #FH_ELBA
        call    fh_field
        ex      de, hl
        ld      hl, #_fat_lba
        call    ff_copy4
        ld      hl, #_fat_lba
        ld      de, #ff_tmp
        call    ff_add32
        or      a
        ret

fl_rewind:
        ;; Назад - перебор отрезков с нуля.
        ld      a, #FH_ESTART
        call    fh_field
        call    ff_zero4
        ld      hl, #0
        call    fat_cache_ext
        jp      fl_again

fl_forward:
        ;; Сектор дальше этого отрезка - шагнуть на следующий.
        ld      a, #FH_EIDX
        call    fh_field
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        inc     de
        ld      (ff_eidx), de

        ld      a, #FH_ECNT
        call    fh_field
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ld      hl, (ff_eidx)
        or      a
        sbc     hl, de
        jr      nc, fl_eof      ; отрезки кончились

        ;; ESTART += ESECS. Адрес приёмника берём ПЕРВЫМ и прячем в стек:
        ;; fh_field считает смещение через DE и второй вызов затёр бы его.
        ld      a, #FH_ESTART
        call    fh_field
        push    hl
        ld      a, #FH_ESECS
        call    fh_field
        ex      de, hl
        pop     hl
        call    ff_add32

        ld      hl, (ff_eidx)
        call    fat_cache_ext
        jp      fl_again

fl_eof:
        scf
        ret

;; -- Быстрый режим: целыми секторами --
;;
;; Вход: BC = сколько секторов; IX = куда, если _fat_read_dest = 0.
;;
;; Позиция обязана стоять на границе сектора: карта отдаёт сектор целиком.
;; Не на границе - отказ, а не подгонка.
_fat_read_sectors::
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
        call    ff_load_dehl
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
        call    ff_load_dehl
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
        ;; Наружу позицию отдаём ОДИН раз: копировать её на каждом
        ;; секторе - это полтысячи тактов там, где весь сектор стоит
        ;; тринадцать.
        call    fat_publish
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

;; Сдвинуть позицию на A секторов вперёд, то есть на A*512 байт.
frs_advance_n:
        ld      l, a
        ld      h, #0
        add     hl, hl                  ; A*2 - это A*512 без младшего байта
        ld      de, #ff_tmp
        xor     a
        ld      (de), a
        inc     de
        ld      a, l
        ld      (de), a
        inc     de
        ld      a, h
        ld      (de), a
        inc     de
        xor     a
        ld      (de), a
        ld      a, #FH_POS
        call    fh_field
        ld      de, #ff_tmp
        call    ff_add32
        ret

;; Сдвинуть позицию на 512 байт вперёд.
frs_advance:
        ld      hl, #ff_tmp
        ld      (hl), #0
        inc     hl
        ld      (hl), #2        ; 0x0200 = 512
        inc     hl
        ld      (hl), #0
        inc     hl
        ld      (hl), #0
        ld      a, #FH_POS
        call    fh_field
        ld      de, #ff_tmp
        call    ff_add32
        ret

;; -- Точный режим: сколько попросили, столько и отдадим --
;;
;; Вход: BC = сколько байт, IX = куда.
;;
;; Внутри всё равно читаются целые секторы - иначе карта не умеет, - но
;; наружу это не видно: началу и хвосту достаётся кусок через буфер.
_fat_read_bytes::
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
        call    ff_load_dehl
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
        ld      a, #FH_POS
        call    fh_field
        ld      de, #ff_tmp
        call    ff_add32

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
        call    fat_publish
        or      a
        ret

;; -- 32-битная мелочь --

;; (HL) <- (DE)
ff_copy4:
        ld      b, #4
ff_c4:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    ff_c4
        ret

ff_zero4:
        ld      b, #4
        xor     a
ff_z4:
        ld      (hl), a
        inc     hl
        djnz    ff_z4
        ret

;; (HL) += (DE)
ff_add32:
        ld      b, #4
        or      a
ff_a32:
        ld      a, (de)
        adc     a, (hl)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    ff_a32
        ret

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

;; (HL) <<= 1
ff_shl32:
        or      a
        ld      b, #4
ff_l32:
        rl      (hl)
        inc     hl
        djnz    ff_l32
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

;; HL -> 32-битное число; выход DEHL = оно же.
ff_load_dehl:
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
