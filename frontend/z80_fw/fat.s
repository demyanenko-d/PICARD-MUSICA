;; fat.s - FAT32, только чтение.
;;
;; Монтирование, каталог, цепочки кластеров и отрезки файла: BPB
;; разбирается один раз, дальше всё считается от начала FAT и области
;; данных.
;;
;; Соглашение об ошибках то же, что у sd.s: CY=1 плохо, CY=0 хорошо.
;;
;; 32-битные числа лежат в памяти МЛАДШИМ БАЙТОМ ВПЕРЁД - так их удобнее
;; складывать в цикле. Не путать с полями команд SD, там старший первый.

        .module fat
        .globl  _fat_mount
        .globl  _fat_spc
        .globl  _fat_spc_shift
        .globl  _fat_start
        .globl  _fat_data_start
        .globl  _fat_root_clus
        .globl  _fat_sec_buf
        .globl  _fat_clus_to_lba
        .globl  _fat_clus
        .globl  _fat_lba

        .globl  _fat_next_clus
        .globl  _fat_dir_open
        .globl  _fat_dir_open_root
        .globl  _fat_dir_next
        .globl  _fat_ent_name
        .globl  _fat_ent_attr
        .globl  _fat_ent_clus
        .globl  _fat_ent_size

        .globl  _fat_lower
        .globl  _fat_build_extents
        .globl  _fat_ext_count
        .globl  _fat_ext_overflow

        .globl  _sd_read_sector
        .globl  _fat_page_set
        .globl  _fat_port_shadow

;; Сколько отрезков держит страница: (16384 - 2 на заголовок) / 6.
FAT_EXT_MAX .equ 2730

        .area   _DATA

_fat_sec_buf::                  ; рабочий сектор: BPB и каталоги
        .ds     512

;; Отдельный буфер под сектор FAT: иначе обход цепочки затирал бы сектор
;; каталога, по которому идём. Кэшируется - в секторе 128 записей, почти
;; вся ходьба по цепочке обходится без карты.
fat_fat_buf:     .ds  512
fat_fat_lba:     .ds  4
fat_fat_valid:   .ds  1

_fat_spc::       .ds  1         ; секторов в кластере
_fat_spc_shift:: .ds  1         ; его же log2 - умножение делаем сдвигами
_fat_start::     .ds  4         ; первый сектор FAT
_fat_data_start::.ds  4         ; первый сектор области данных
_fat_root_clus:: .ds  4         ; кластер корневого каталога

_fat_clus::      .ds  4         ; вход/выход для fat_clus_to_lba
_fat_lba::       .ds  4

fat_tmp:         .ds  4

;; С какого сектора начинается том. Ноль - том лежит с начала карты, иначе
;; это начало раздела из таблицы в нулевом секторе.
fat_part_base:   .ds  4
fat_part_ptr:    .ds  2         ; какую запись таблицы разделов смотрим
fat_part_left:   .ds  1

;; -- Состояние обхода каталога --
fat_dir_clus:    .ds  4         ; кластер, чей сектор лежит в буфере
fat_dir_sec:     .ds  1         ; номер сектора внутри кластера
fat_dir_ptr:     .ds  2         ; текущая запись в _fat_sec_buf
fat_dir_left:    .ds  1         ; сколько записей осталось в секторе
fat_io_err:      .ds  1         ; 1 - оборвались из-за карты, а не по концу

;; -- Разобранная запись каталога --
;;
;; Имя всегда в CP866 и с нулём в конце: длинное, если собралось и
;; переводится, иначе 8.3. Так же поступает WC.
;;
;; 20 записей длинного имени по 13 символов - предел формата; плюс ноль.
_fat_ent_name::  .ds  261
_fat_ent_attr::  .ds  1
_fat_ent_clus::  .ds  4
_fat_ent_size::  .ds  4

fat_lfn_sum:     .ds  1         ; контрольная сумма из записей имени
fat_lfn_seq:     .ds  1         ; какую по счёту запись ждём (считаем вниз)
fat_lfn_ok:      .ds  1         ; имя пока собирается без ошибок

;; -- Построение отрезков --
fat_ext_page:    .ds  1         ; в какую страницу складываем
fat_ext_saved:   .ds  1         ; какая страница была включена до нас
fat_ext_ptr:     .ds  2         ; куда писать следующий отрезок
fat_ext_first:   .ds  4         ; первый кластер текущего отрезка
fat_ext_run:     .ds  2         ; сколько кластеров в нём уже набралось
fat_ext_expect:  .ds  4         ; при каком следующем отрезок продолжается
fat_ext_next:    .ds  4         ; что на самом деле сказала FAT

_fat_ext_count::    .ds  2      ; сколько отрезков вышло
_fat_ext_overflow:: .ds  1      ; 1 - файл раздроблен сильнее, чем влезает

        .area   _CODE

;; -- 32-битная арифметика над памятью --
;;
;; Все три работают с 4-байтными числами по указателям, младший байт
;; первым: короче, чем таскать значения по регистрам.

;; (HL) += (DE)
add32:
        ld      b, #4
        or      a               ; сбросить перенос
add32_l:
        ld      a, (de)
        adc     a, (hl)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    add32_l
        ret

;; (HL) -= 2 - единственное вычитание, которое требуется: номер кластера
;; отсчитывается от двух.
sub32_2:
        ld      a, (hl)
        sub     #2
        ld      (hl), a
        inc     hl
        ret     nc              ; заёма нет - старшие байты не трогаем
        ld      b, #3
sub32_2_borrow:
        ld      a, (hl)
        sbc     a, #0
        ld      (hl), a
        inc     hl
        ret     nc
        djnz    sub32_2_borrow
        ret

;; (HL) <<= 1
shl32:
        or      a
        ld      b, #4
shl32_l:
        rl      (hl)
        inc     hl
        djnz    shl32_l
        ret

;; Скопировать 4 байта из (DE) в (HL)
copy32:
        ld      b, #4
copy32_l:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    copy32_l
        ret

;; -- Монтирование --
;;
;; Читает сектор 0, проверяет, что это похоже на FAT32 с сектором в 512
;; байт, и считает границы. Выход: CY=0 успех.
;; Том лежит либо с нулевого сектора (суперфлоппи), либо внутри раздела -
;; чаще второе: таблица разделов в нулевом секторе, том с сектора ~2048.
;;
;; Отличаем по содержимому: нулевой сектор похож на BPB - значит он и
;; есть, иначе ищем разделы. Тип раздела смотрим только чтобы не лезть в
;; пустые записи; решает BPB, потому что тип пишут кто во что горазд.
_fat_mount::
        ld      hl, #fat_part_base
        call    fat_zero4

        ld      de, #0
        ld      hl, #0
        ld      ix, #_fat_sec_buf
        call    _sd_read_sector
        jp      c, fat_mount_fail

        call    fat_check_bpb
        jp      nc, fat_mount_parse

        ;; Не BPB - значит, надежда на таблицу разделов. Без подписи там и
        ;; смотреть нечего.
        ld      a, (_fat_sec_buf + 510)
        cp      #0x55
        jp      nz, fat_mount_fail
        ld      a, (_fat_sec_buf + 511)
        cp      #0xAA
        jp      nz, fat_mount_fail

        ld      hl, #_fat_sec_buf + 0x1BE
        ld      (fat_part_ptr), hl
        ld      a, #4
        ld      (fat_part_left), a

fat_part_try:
        ld      hl, (fat_part_ptr)
        ld      bc, #4
        add     hl, bc
        ld      a, (hl)                 ; тип раздела
        or      a
        jr      z, fat_part_next        ; пустая запись

        ;; Начальный сектор раздела лежит по +8, младшим байтом вперёд.
        ld      hl, (fat_part_ptr)
        ld      bc, #8
        add     hl, bc
        ex      de, hl
        ld      hl, #fat_part_base
        call    copy32

        ld      a, (fat_part_base+0)
        ld      hl, #fat_part_base+1
        or      (hl)
        inc     hl
        or      (hl)
        inc     hl
        or      (hl)
        jr      z, fat_part_next        ; раздел с нулевого сектора не бывает

        ld      hl, #fat_part_base
        call    load32_dehl
        ld      ix, #_fat_sec_buf
        call    _sd_read_sector
        jr      c, fat_part_next
        call    fat_check_bpb
        jr      nc, fat_mount_parse

fat_part_next:
        ld      hl, (fat_part_ptr)
        ld      bc, #16
        add     hl, bc
        ld      (fat_part_ptr), hl
        ld      hl, #fat_part_left
        dec     (hl)
        jr      nz, fat_part_try
        jp      fat_mount_fail

;; -- Разбор BPB --
;;
;; Сюда попадаем, когда в буфере лежит проверенный BPB, а fat_part_base
;; говорит, с какого сектора начинается том.
fat_mount_parse:
        ;; Секторов в кластере обязана быть степень двойки: умножаем
        ;; сдвигами. Заодно считаем log2.
        ld      a, (_fat_sec_buf + 13)
        ld      (_fat_spc), a
        ld      b, a
        dec     a
        and     b
        jp      nz, fat_mount_fail      ; не степень двойки

        ld      a, b
        ld      b, #0
fat_spc_log:
        srl     a
        jr      z, fat_spc_done
        inc     b
        jr      fat_spc_log
fat_spc_done:
        ld      a, b
        ld      (_fat_spc_shift), a

        ;; fat_start = начало раздела + reserved
        ld      hl, #_fat_start
        ld      a, (_fat_sec_buf + 14)
        ld      (hl), a
        inc     hl
        ld      a, (_fat_sec_buf + 15)
        ld      (hl), a
        inc     hl
        xor     a
        ld      (hl), a
        inc     hl
        ld      (hl), a

        ;; Смещение раздела прибавляем один раз, здесь: дальше всё
        ;; считается от fat_start.
        ld      hl, #_fat_start
        ld      de, #fat_part_base
        call    add32

        ;; data_start = fat_start + num_fats * fat_size32
        ld      hl, #_fat_data_start
        ld      de, #_fat_start
        call    copy32

        ld      a, (_fat_sec_buf + 16)  ; сколько копий FAT
        or      a
        jp      z, fat_mount_fail
        ld      c, a
fat_add_fat:
        ld      hl, #_fat_data_start
        ld      de, #_fat_sec_buf + 36  ; FATsz32 лежит младшим байтом вперёд
        call    add32
        dec     c
        jr      nz, fat_add_fat

        ;; Корневой кластер.
        ld      hl, #_fat_root_clus
        ld      de, #_fat_sec_buf + 44
        call    copy32

        or      a                       ; CY=0
        ret

fat_mount_fail:
        scf
        ret

;; Похоже ли содержимое буфера на BPB тома FAT32? CY=0 - да.
;;
;; Проверяем четыре поля сразу: у таблицы разделов на этом месте начало
;; загрузочного кода, одно совпадение там набраться может, четыре - нет.
fat_check_bpb:
        ld      a, (_fat_sec_buf + 510)         ; подпись
        cp      #0x55
        jr      nz, fat_bpb_no
        ld      a, (_fat_sec_buf + 511)
        cp      #0xAA
        jr      nz, fat_bpb_no

        ld      a, (_fat_sec_buf + 11)          ; байт на сектор = 512
        or      a
        jr      nz, fat_bpb_no
        ld      a, (_fat_sec_buf + 12)
        cp      #0x02
        jr      nz, fat_bpb_no

        ld      a, (_fat_sec_buf + 13)          ; секторов в кластере
        or      a
        jr      z, fat_bpb_no
        ld      b, a
        dec     a
        and     b
        jr      nz, fat_bpb_no                  ; обязана быть степень двойки

        ld      a, (_fat_sec_buf + 16)          ; копий FAT
        or      a
        jr      z, fat_bpb_no

        ;; FATsz32 не ноль - это и отличает FAT32 от FAT16 и от мусора.
        ld      a, (_fat_sec_buf + 36)
        ld      hl, #_fat_sec_buf + 37
        or      (hl)
        inc     hl
        or      (hl)
        inc     hl
        or      (hl)
        jr      z, fat_bpb_no

        or      a                               ; CY=0
        ret
fat_bpb_no:
        scf
        ret

;; Обнулить 4 байта по (HL).
fat_zero4:
        xor     a
        ld      b, #4
fat_zero4_l:
        ld      (hl), a
        inc     hl
        djnz    fat_zero4_l
        ret

;; -- Кластер -> первый его сектор --
;;
;; lba = data_start + (clus - 2) * spc
;;
;; Вход: _fat_clus. Выход: _fat_lba.
_fat_clus_to_lba::
        ld      hl, #fat_tmp
        ld      de, #_fat_clus
        call    copy32

        ld      hl, #fat_tmp
        call    sub32_2

        ld      a, (_fat_spc_shift)
        or      a
        jr      z, fat_c2l_add
fat_c2l_shift:
        ld      hl, #fat_tmp
        push    af
        call    shl32
        pop     af
        dec     a
        jr      nz, fat_c2l_shift

fat_c2l_add:
        ld      hl, #_fat_lba
        ld      de, #_fat_data_start
        call    copy32
        ld      hl, #_fat_lba
        ld      de, #fat_tmp
        call    add32
        ret

;; Вход: HL -> 32-битное число в памяти. Выход: DEHL = оно же - в том виде,
;; в каком его ждёт _sd_read_sector.
load32_dehl:
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

;; -- Следующий кластер цепочки --
;;
;; Вход: _fat_clus. Выход: _fat_clus = следующий, CY=0.
;; CY=1 - цепочка кончилась или карта не ответила (что именно, говорит
;; fat_io_err).
_fat_next_clus::
        ;; Сектор FAT = fat_start + clus/128. Делить семью сдвигами вправо
        ;; дорого, а clus>>7 - это (clus>>8)<<1 плюс седьмой бит младшего
        ;; байта: сдвиг всего один.
        ld      hl, #fat_tmp
        ld      de, #_fat_clus+1
        ld      b, #3
fnc_copy:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    fnc_copy
        xor     a
        ld      (fat_tmp+3), a

        ld      hl, #fat_tmp
        call    shl32
        ld      a, (_fat_clus)
        rlca                    ; седьмой бит уехал в перенос
        jr      nc, fnc_no_bit
        ld      hl, #fat_tmp
        inc     (hl)            ; после сдвига младший байт чётный, не переполнится
fnc_no_bit:
        ld      hl, #fat_tmp
        ld      de, #_fat_start
        call    add32

        ;; Этот сектор FAT уже в буфере?
        ld      a, (fat_fat_valid)
        or      a
        jr      z, fnc_read
        ld      hl, #fat_tmp
        ld      de, #fat_fat_lba
        ld      b, #4
fnc_same:
        ld      a, (de)
        cp      (hl)
        jr      nz, fnc_read
        inc     hl
        inc     de
        djnz    fnc_same
        jr      fnc_have

fnc_read:
        ld      hl, #fat_fat_lba
        ld      de, #fat_tmp
        call    copy32
        xor     a
        ld      (fat_fat_valid), a      ; пока не прочитали - не верить
        ld      hl, #fat_tmp
        call    load32_dehl
        ld      ix, #fat_fat_buf
        call    _sd_read_sector
        jr      nc, fnc_read_ok
        ld      a, #1
        ld      (fat_io_err), a
        scf
        ret
fnc_read_ok:
        ld      a, #1
        ld      (fat_fat_valid), a

fnc_have:
        ;; Смещение внутри сектора = (clus & 127) * 4.
        ld      a, (_fat_clus)
        and     #0x7F
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        ld      de, #fat_fat_buf
        add     hl, de

        ;; Старшие четыре бита в FAT32 зарезервированы - гасим их сразу,
        ;; иначе сравнение с меткой конца цепочки не сойдётся.
        ld      de, #_fat_clus
        ld      b, #3
fnc_get:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        djnz    fnc_get
        ld      a, (hl)
        and     #0x0F
        ld      (de), a

        ;; Конец цепочки - всё, что не меньше 0x0FFFFFF8.
        cp      #0x0F
        jr      nz, fnc_ok
        ld      a, (_fat_clus+2)
        cp      #0xFF
        jr      nz, fnc_ok
        ld      a, (_fat_clus+1)
        cp      #0xFF
        jr      nz, fnc_ok
        ld      a, (_fat_clus)
        cp      #0xF8
        jr      c, fnc_ok
        scf                     ; цепочка кончилась, fat_io_err не трогаем
        ret
fnc_ok:
        or      a
        ret

;; -- Открыть каталог --
;;
;; Вход: _fat_clus - первый кластер каталога. Выход: CY=0.
_fat_dir_open::
        ld      hl, #fat_dir_clus
        ld      de, #_fat_clus
        call    copy32
        xor     a
        ld      (fat_dir_sec), a
        ld      (fat_lfn_ok), a
        ld      (fat_io_err), a
        jp      fat_dir_load

;; То же для корня.
_fat_dir_open_root::
        ld      hl, #_fat_clus
        ld      de, #_fat_root_clus
        call    copy32
        jp      _fat_dir_open

;; Прочитать текущий сектор каталога в _fat_sec_buf.
fat_dir_load:
        ld      hl, #_fat_clus
        ld      de, #fat_dir_clus
        call    copy32
        call    _fat_clus_to_lba

        ld      a, (fat_dir_sec)
        ld      hl, #fat_tmp
        ld      (hl), a
        inc     hl
        xor     a
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (hl), a
        ld      hl, #_fat_lba
        ld      de, #fat_tmp
        call    add32

        ld      hl, #_fat_lba
        call    load32_dehl
        ld      ix, #_fat_sec_buf
        call    _sd_read_sector
        jr      nc, fdl_ok
        ld      a, #1
        ld      (fat_io_err), a
        scf
        ret
fdl_ok:
        ld      hl, #_fat_sec_buf
        ld      (fat_dir_ptr), hl
        ld      a, #16          ; 512/32 записей в секторе
        ld      (fat_dir_left), a
        or      a
        ret

;; Перейти к следующему сектору каталога. CY=1 - секторов больше нет.
fat_dir_advance:
        ld      a, (fat_dir_sec)
        inc     a
        ld      b, a
        ld      a, (_fat_spc)
        cp      b
        jr      z, fda_next_clus
        ld      a, b
        ld      (fat_dir_sec), a
        jp      fat_dir_load
fda_next_clus:
        ld      hl, #_fat_clus
        ld      de, #fat_dir_clus
        call    copy32
        call    _fat_next_clus
        ret     c               ; цепочка каталога кончилась
        ld      hl, #fat_dir_clus
        ld      de, #_fat_clus
        call    copy32
        xor     a
        ld      (fat_dir_sec), a
        jp      fat_dir_load

;; -- Следующая запись каталога --
;;
;; Выход: A=1 - запись разобрана в _fat_ent_*;
;;        A=0 - каталог кончился;
;;        CY=1 - карта не ответила.
;;
;; Записи "." и ".." возвращаются как есть: решать, показывать их или нет,
;; должен навигатор, а не библиотека.
_fat_dir_next::
        xor     a
        ld      (fat_io_err), a
        ld      (fat_lfn_ok), a

fdn_entry:
        ld      a, (fat_dir_left)
        or      a
        jr      nz, fdn_have
        call    fat_dir_advance
        jr      nc, fdn_entry
        ld      a, (fat_io_err)
        or      a
        jr      nz, fdn_io_err
        jr      fdn_end

fdn_have:
        dec     a
        ld      (fat_dir_left), a
        ld      hl, (fat_dir_ptr)

        ld      a, (hl)
        or      a
        jr      z, fdn_end      ; нулевой первый байт - дальше каталог пуст
        cp      #0xE5
        jr      z, fdn_skip     ; стёртая запись

        push    hl
        ld      bc, #11
        add     hl, bc
        ld      a, (hl)
        pop     hl
        cp      #0x0F
        jr      z, fdn_lfn
        and     #0x08
        jr      nz, fdn_skip    ; метка тома
        jp      fdn_short

fdn_skip:
        ;; Пропущенная запись рвёт набранное длинное имя: оно относилось
        ;; бы к другому файлу.
        xor     a
        ld      (fat_lfn_ok), a
        call    fdn_step
        jr      fdn_entry

fdn_end:
        xor     a
        ret

fdn_io_err:
        scf
        ret

;; Сдвинуть указатель на следующую 32-байтную запись.
fdn_step:
        ld      hl, (fat_dir_ptr)
        ld      bc, #32
        add     hl, bc
        ld      (fat_dir_ptr), hl
        ret

;; -- Часть длинного имени --
;;
;; На диске такие записи лежат ПЕРЕД короткой и в обратном порядке: первой
;; идёт последняя часть имени, помеченная битом 0x40. Номер части сразу
;; говорит, куда её класть, и имя собирается одним проходом вперёд. Буфер
;; имени отдельно от буфера сектора, поэтому имя на границе секторов цело.
fdn_lfn:
        ld      a, (hl)
        bit     6, a
        jr      z, fl_cont

        ;; Начало нового имени.
        and     #0x3F
        jr      z, fl_bad       ; нулевого номера не бывает
        cp      #21
        jr      nc, fl_bad      ; длиннее предела формата
        ld      (fat_lfn_seq), a

        ;; Ноль в самый конец имени: если длина кратна 13, своего
        ;; терминатора у имени нет.
        call    fat_lfn_slot
        ld      (hl), #0

        ld      hl, (fat_dir_ptr)
        ld      bc, #13
        add     hl, bc
        ld      a, (hl)
        ld      (fat_lfn_sum), a
        ld      a, #1
        ld      (fat_lfn_ok), a
        jr      fl_chars

fl_cont:
        ;; Продолжение: номер обязан идти ровно на единицу вниз, и имя до
        ;; сих пор должно быть целым.
        and     #0x3F
        ld      b, a
        ld      a, (fat_lfn_ok)
        or      a
        jr      z, fl_next
        ld      a, (fat_lfn_seq)
        cp      b
        jr      nz, fl_bad
        ld      hl, (fat_dir_ptr)
        ld      bc, #13
        add     hl, bc
        ld      a, (hl)
        ld      hl, #fat_lfn_sum
        cp      (hl)
        jr      nz, fl_bad

fl_chars:
        ;; Символы кладём по номеру части: (seq-1)*13.
        ld      a, (fat_lfn_seq)
        dec     a
        call    fat_lfn_slot
        ex      de, hl
        ld      hl, (fat_dir_ptr)
        call    fat_lfn_chars
        ld      a, (fat_lfn_seq)
        dec     a
        ld      (fat_lfn_seq), a
fl_next:
        call    fdn_step
        jp      fdn_entry

fl_bad:
        xor     a
        ld      (fat_lfn_ok), a
        jr      fl_next

;; A = номер части. Выход: HL = _fat_ent_name + A*13.
fat_lfn_slot:
        ld      l, a
        ld      h, #0
        ld      d, h
        ld      e, l
        add     hl, hl          ; 2a
        add     hl, hl          ; 4a
        add     hl, de          ; 5a
        add     hl, hl          ; 10a
        add     hl, de          ; 11a
        add     hl, de          ; 12a
        add     hl, de          ; 13a
        ld      de, #_fat_ent_name
        add     hl, de
        ret

;; Разложить 13 символов записи: HL -> запись, DE -> куда класть.
fat_lfn_chars:
        push    hl
        inc     hl
        ld      b, #5
        call    fat_lfn_run
        pop     hl
        ret     c
        push    hl
        ld      bc, #14
        add     hl, bc
        ld      b, #6
        call    fat_lfn_run
        pop     hl
        ret     c
        ld      bc, #28
        add     hl, bc
        ld      b, #2
        jp      fat_lfn_run

;; B символов из (HL) в UTF-16 -> (DE) в CP866.
;; CY=1 - дальше разбирать нечего: имя кончилось либо не переводится.
;;
;; Перевод тот же, что у WC: латиница до 0x80 как есть, кириллица U+04xx
;; в ALT-раскладку, всё остальное - отказ и откат на имя 8.3.
fat_lfn_run:
        ld      a, (hl)
        ld      c, a            ; младший байт
        inc     hl
        ld      a, (hl)         ; старший
        inc     hl
        or      a
        jr      z, flr_ascii
        cp      #0x04
        jr      nz, flr_bad
        ld      a, c
        cp      #0x01
        jr      z, flr_yo_hi    ; U+0401 Ё
        cp      #0x10
        jr      c, flr_bad
        cp      #0x40
        jr      c, flr_a_p      ; U+0410..043F -> 0x80..0xAF
        cp      #0x50
        jr      c, flr_r_ya     ; U+0440..044F -> 0xE0..0xEF
        cp      #0x51
        jr      z, flr_yo_lo    ; U+0451 ё
        jr      flr_bad
flr_ascii:
        ld      a, c
        or      a
        jr      z, flr_end      ; 0x0000 - имя кончилось
        cp      #0x80
        jr      nc, flr_bad
        jr      flr_put
flr_a_p:
        add     a, #0x70
        jr      flr_put
flr_r_ya:
        add     a, #0xA0
        jr      flr_put
flr_yo_hi:
        ld      a, #0xF0
        jr      flr_put
flr_yo_lo:
        ld      a, #0xF1
flr_put:
        ld      (de), a
        inc     de
        djnz    fat_lfn_run
        or      a
        ret
flr_end:
        ld      (de), a         ; A=0 - терминатор
        scf
        ret
flr_bad:
        xor     a
        ld      (fat_lfn_ok), a
        scf
        ret

;; -- Короткая запись: она и закрывает имя --
fdn_short:
        ;; Длинное имя годится, только если оно собралось целиком (дошли до
        ;; первой части) и его контрольная сумма сходится с именем 8.3.
        ;; Сумма тут не формальность: она единственное, что связывает
        ;; записи длинного имени с короткой.
        ld      a, (fat_lfn_ok)
        or      a
        jr      z, fs_short_name
        ld      a, (fat_lfn_seq)
        or      a
        jr      nz, fs_short_name
        ld      hl, (fat_dir_ptr)
        call    fat_short_sum
        ld      hl, #fat_lfn_sum
        cp      (hl)
        jr      z, fs_fields    ; имя уже лежит в _fat_ent_name

fs_short_name:
        call    fat_short_name

fs_fields:
        ld      hl, (fat_dir_ptr)
        ld      bc, #11
        add     hl, bc
        ld      a, (hl)
        ld      (_fat_ent_attr), a

        ;; Номер кластера собран из двух половин, лежащих врозь: младшая на
        ;; +26, старшая на +20.
        ld      hl, (fat_dir_ptr)
        ld      bc, #26
        add     hl, bc
        ld      a, (hl)
        ld      (_fat_ent_clus+0), a
        inc     hl
        ld      a, (hl)
        ld      (_fat_ent_clus+1), a
        ld      hl, (fat_dir_ptr)
        ld      bc, #20
        add     hl, bc
        ld      a, (hl)
        ld      (_fat_ent_clus+2), a
        inc     hl
        ld      a, (hl)
        ld      (_fat_ent_clus+3), a

        ld      hl, (fat_dir_ptr)
        ld      bc, #28
        add     hl, bc
        ex      de, hl
        ld      hl, #_fat_ent_size
        call    copy32

        call    fdn_step
        ld      a, #1
        or      a               ; CY=0
        ret

;; Контрольная сумма имени 8.3 - та же, что записана в частях длинного
;; имени. Вход: HL -> запись. Выход: A.
fat_short_sum:
        xor     a
        ld      b, #11
fss_l:
        rrca
        add     a, (hl)
        inc     hl
        djnz    fss_l
        ret

;; Имя 8.3 -> строка в _fat_ent_name: хвостовые пробелы долой, точка
;; только если расширение непустое.
fat_short_name:
        ld      hl, (fat_dir_ptr)
        ld      de, #_fat_ent_name
        ld      b, #8
        call    fsn_copy
        ld      hl, (fat_dir_ptr)
        ld      bc, #8
        add     hl, bc
        ld      a, (hl)
        cp      #0x20
        jr      z, fsn_done     ; расширения нет - и точки не надо
        ld      a, #0x2E
        ld      (de), a
        inc     de
        ld      b, #3
        call    fsn_copy
fsn_done:
        xor     a
        ld      (de), a
        ret

;; B символов из (HL) в (DE), хвостовые пробелы отбрасываются.
;;
;; Копируем всё подряд, а потом отматываем: считать "сколько было не
;; пробелов" нельзя - пробел может стоять и в середине.
fsn_copy:
        push    de
        ld      c, b
fsn_l:
        ld      a, (hl)
        inc     hl
        call    _fat_lower
        ld      (de), a
        inc     de
        djnz    fsn_l
        ld      b, c
fsn_trim:
        dec     de
        ld      a, (de)
        cp      #0x20
        jr      nz, fsn_keep
        djnz    fsn_trim
        pop     de              ; одни пробелы - вернуться в начало
        ret
fsn_keep:
        inc     de
        pop     hl              ; начало больше не нужно
        ret

;; -- Цепочка кластеров -> отрезки --
;;
;; Отрезок - "первый кластер плюс сколько подряд", 6 байт вместо четырёх
;; на каждый кластер. Нефрагментированный файл - одна запись.
;;
;; Раскладка в странице (окно 0xC000):
;;   +0   слово: сколько отрезков
;;   +2   отрезки по 6 байт: кластер (4), сколько подряд (2)
;;
;; Соседние кластеры лежат в секторе FAT соседними четвёрками байт: внутри
;; отрезка указатель едет на 4 байта вперёд, адрес сектора пересчитывается
;; только на его границе, раз на 128 кластеров.
;;
;; Вход:  _fat_clus = первый кластер файла, A = номер страницы.
;; Выход: CY=0, _fat_ext_count = сколько получилось отрезков.
;;        CY=1 - карта не ответила либо отрезков больше, чем влезает
;;        (что именно, говорит _fat_ext_overflow).
_fat_build_extents::
        ld      (fat_ext_page), a
        xor     a
        ld      (_fat_ext_overflow), a
        ld      hl, #0
        ld      (_fat_ext_count), hl

        ;; Запомнить, какая страница была включена, и вернуть её на место
        ;; при выходе: вызывающий про наши переключения знать не обязан.
        ld      a, (_fat_port_shadow)
        ld      (fat_ext_saved), a
        ld      a, (fat_ext_page)
        call    _fat_page_set

        ld      hl, #0xC002
        ld      (fat_ext_ptr), hl

        ;; Пустой файл: кластера нет, отрезков ноль.
        ld      a, (_fat_clus+3)
        or      a
        jr      nz, fbe_have
        ld      a, (_fat_clus+2)
        or      a
        jr      nz, fbe_have
        ld      a, (_fat_clus+1)
        or      a
        jr      nz, fbe_have
        ld      a, (_fat_clus)
        cp      #2
        jp      c, fbe_done

fbe_have:
        ;; Начали первый отрезок.
        ld      hl, #fat_ext_first
        ld      de, #_fat_clus
        call    copy32
        ld      hl, #1
        ld      (fat_ext_run), hl
        call    fbe_set_expect

fbe_sector:
        ;; Пересчитать адрес записи в FAT для текущего кластера. Сюда
        ;; попадаем только на разрыве отрезка или на границе сектора.
        call    fbe_load_fat
        jp      c, fbe_io_err

        ;; Забрать в регистры то, с чем работает горячий цикл.
        ld      de, (fat_ext_expect)
        ld      bc, (fat_ext_expect+2)
        ld      ix, (fat_ext_run)

;; -- Горячий цикл --
;;
;; Здесь тратится почти всё время открытия файла, поэтому всё, что нужно
;; циклу, лежит в регистрах: HL - место в секторе FAT, DE:BC - номер
;; кластера, при котором отрезок продолжается, IX - сколько кластеров в
;; отрезке уже набралось.
;;
;; Сравнение побайтовое, с выходом на первом несовпавшем: разбор причины
;; (конец цепочки или разрыв) стоит дорого, но случается раз на отрезок.
fbe_inner:
        ld      a, (hl)
        inc     hl
        cp      e
        jr      nz, fbe_mis1
        ld      a, (hl)
        inc     hl
        cp      d
        jr      nz, fbe_mis2
        ld      a, (hl)
        inc     hl
        cp      c
        jr      nz, fbe_mis3
        ld      a, (hl)
        inc     hl
        and     #0x0F           ; старшие четыре бита зарезервированы
        cp      b
        jr      nz, fbe_mis4

        ;; Отрезок продолжается. Адрес следующей записи пересчитывать не
        ;; надо: она лежит прямо за этой.
        inc     ix
        inc     de
        ld      a, d
        or      e
        jr      nz, fbe_no_carry
        inc     bc
fbe_no_carry:
        ld      a, l
        cp      #<(fat_fat_buf + 512)
        jr      nz, fbe_inner
        ld      a, h
        cp      #>(fat_fat_buf + 512)
        jr      nz, fbe_inner

        ;; Сектор кончился - сложить регистры обратно и взять следующий.
        call    fbe_spill
        jp      fbe_sector

;; Несовпадение. Отматываем HL на начало записи - каждая точка входа
;; знает, сколько байт успела съесть.
fbe_mis4:
        dec     hl
fbe_mis3:
        dec     hl
fbe_mis2:
        dec     hl
fbe_mis1:
        dec     hl
        call    fbe_spill

        ;; Прочитать запись целиком: в горячем цикле мы бросили сравнение
        ;; на первом несовпавшем байте и всех четырёх не видели.
        ld      de, #fat_ext_next
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        ld      a, (hl)
        and     #0x0F
        ld      (de), a

        ;; Конец цепочки?
        cp      #0x0F
        jr      nz, fbe_break
        ld      a, (fat_ext_next+2)
        cp      #0xFF
        jr      nz, fbe_break
        ld      a, (fat_ext_next+1)
        cp      #0xFF
        jr      nz, fbe_break
        ld      a, (fat_ext_next)
        cp      #0xF8
        jr      c, fbe_break
        call    fbe_close
        jr      c, fbe_overflow
        jp      fbe_done

fbe_break:
        ;; Отрезок кончился: закрыть и начать новый с того кластера, куда
        ;; цепочка увела.
        call    fbe_close
        jr      c, fbe_overflow
        ld      hl, #fat_ext_first
        ld      de, #fat_ext_next
        call    copy32
        ld      hl, #_fat_clus
        ld      de, #fat_ext_next
        call    copy32
        ld      hl, #1
        ld      (fat_ext_run), hl
        call    fbe_set_expect
        jp      fbe_sector

;; Сложить регистры горячего цикла обратно в память. HL не трогаем: он
;; ещё нужен вызывающему.
;;
;; Текущий кластер - это тот, которого мы ждали, минус один: пока отрезок
;; шёл, "ждём такой-то" уезжало на кластер вперёд вместе с указателем.
fbe_spill:
        ld      (fat_ext_expect), de
        ld      (fat_ext_expect+2), bc
        ld      (fat_ext_run), ix
        push    hl
        ld      hl, #_fat_clus
        ld      de, #fat_ext_expect
        call    copy32
        ld      hl, #_fat_clus
        call    fbe_dec32
        pop     hl
        ret

;; (HL) -= 1 для 32-битного числа.
fbe_dec32:
        ld      a, (hl)
        sub     #1
        ld      (hl), a
        ret     nc
        ld      b, #3
fbe_dec32_l:
        inc     hl
        ld      a, (hl)
        sbc     a, #0
        ld      (hl), a
        ret     nc
        djnz    fbe_dec32_l
        ret

fbe_done:
        call    fbe_write_count
        ld      a, (fat_ext_saved)
        call    _fat_page_set
        or      a
        ret

fbe_overflow:
        ld      a, #1
        ld      (_fat_ext_overflow), a
fbe_io_err:
        call    fbe_write_count
        ld      a, (fat_ext_saved)
        call    _fat_page_set
        scf
        ret

;; Записать в заголовок страницы, сколько отрезков вышло.
fbe_write_count:
        ld      hl, (_fat_ext_count)
        ld      (0xC000), hl
        ret

;; Закрыть текущий отрезок: дописать его в страницу. CY=1 - не влезло.
fbe_close:
        ld      hl, (_fat_ext_count)
        ld      de, #FAT_EXT_MAX
        or      a
        sbc     hl, de
        jr      c, fbe_close_ok
        scf                     ; отрезков больше, чем держит страница
        ret
fbe_close_ok:
        ld      hl, (fat_ext_ptr)
        ld      de, #fat_ext_first
        call    copy32
        ld      de, (fat_ext_run)
        ld      (hl), e
        inc     hl
        ld      (hl), d
        inc     hl
        ld      (fat_ext_ptr), hl
        ld      hl, (_fat_ext_count)
        inc     hl
        ld      (_fat_ext_count), hl
        or      a
        ret

;; fat_ext_expect = _fat_clus + 1 - номер кластера, при котором отрезок
;; продолжается.
fbe_set_expect:
        ld      hl, #fat_ext_expect
        ld      de, #_fat_clus
        call    copy32
        ld      hl, #fat_ext_expect
        ;; переходим к inc32 ниже

;; (HL) += 1 для 32-битного числа.
fbe_inc32:
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

;; Прочитать сектор FAT, где лежит запись для _fat_clus, и вернуть в HL
;; адрес самой записи. CY=1 - карта не ответила.
;;
;; Кэш тот же, что у _fat_next_clus: в секторе 128 записей, так что на
;; сплошном файле сюда заходят раз на 64 КБ данных.
fbe_load_fat:
        ld      hl, #fat_tmp
        ld      de, #_fat_clus+1
        ld      b, #3
fbe_cp3:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    fbe_cp3
        xor     a
        ld      (fat_tmp+3), a
        ld      hl, #fat_tmp
        call    shl32
        ld      a, (_fat_clus)
        rlca
        jr      nc, fbe_no_bit
        ld      hl, #fat_tmp
        inc     (hl)
fbe_no_bit:
        ld      hl, #fat_tmp
        ld      de, #_fat_start
        call    add32

        ld      a, (fat_fat_valid)
        or      a
        jr      z, fbe_read
        ld      hl, #fat_tmp
        ld      de, #fat_fat_lba
        ld      b, #4
fbe_same:
        ld      a, (de)
        cp      (hl)
        jr      nz, fbe_read
        inc     hl
        inc     de
        djnz    fbe_same
        jr      fbe_ptr
fbe_read:
        ld      hl, #fat_fat_lba
        ld      de, #fat_tmp
        call    copy32
        xor     a
        ld      (fat_fat_valid), a
        ld      hl, #fat_tmp
        call    load32_dehl
        ld      ix, #fat_fat_buf
        call    _sd_read_sector
        ret     c
        ld      a, #1
        ld      (fat_fat_valid), a
fbe_ptr:
        ld      a, (_fat_clus)
        and     #0x7F
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        ld      de, #fat_fat_buf
        add     hl, de
        or      a
        ret

;; Имена 8.3 на диске лежат заглавными и в той же CP866, что мы показываем.
;; Приводим к нижнему регистру, как это делает WC: иначе короткое имя
;; рядом с длинным стоит заглавными.
_fat_lower::
        cp      #0x80
        jr      nc, fat_lower_ru
        cp      #0x41
        ret     c
        cp      #0x5B
        ret     nc
        or      #0x20
        ret
fat_lower_ru:
        cp      #0xF0           ; Ё
        jr      nz, fat_lower_r1
        inc     a               ; ё
        ret
fat_lower_r1:
        cp      #0xA0
        ret     nc              ; строчные а-п и всё, что выше, уже строчные
        cp      #0x90
        jr      nc, fat_lower_r2
        or      #0x20           ; А-П -> а-п
        ret
fat_lower_r2:
        add     a, #0x50        ; Р-Я -> р-я
        ret
