;; dir.s - весь каталог разом в страницу.
;;
;; Навигатору нужен готовый список: диск на каждое нажатие не перечитать,
;; а имена переменной длины без таблицы не проиндексировать.
;;
;; Раскладка страницы (окно 0xC000..0xFFFF):
;;
;;   0xC000  слово: сколько записей
;;   0xC002  слово: докуда доросли данные
;;   0xC004  записи подряд, вверх:
;;              +0  атрибуты
;;              +1  первый кластер (4)
;;              +5  размер (4)
;;              +9  имя в CP866, ноль в конце
;;   0xFFFE  указатели на записи, по два байта, ВНИЗ: запись i лежит по
;;           адресу из слова 0xFFFE - i*2
;;
;; Данные растут снизу вверх, указатели сверху вниз. Встретились - каталог
;; в страницу не поместился, _fat_dir_full = 1.

        .module dir
        .globl  _fat_dir_load
        .globl  _fat_dir_get
        .globl  _fat_dir_count
        .globl  _fat_dir_full

        .globl  _fat_dir_next
        .globl  _fat_ent_name
        .globl  _fat_ent_attr
        .globl  _fat_ent_clus
        .globl  _fat_ent_size
        .globl  _fat_page_set
        .globl  _fat_page_dir
        .globl  _fat_port_shadow

DIR_COUNT .equ  0xC000
DIR_DATA  .equ  0xC002
DIR_FIRST .equ  0xC004
DIR_INDEX .equ  0xFFFE

        .area   _DATA

_fat_dir_count::.ds     2       ; сколько записей уложилось
_fat_dir_full:: .ds     1       ; 1 - каталог в страницу не поместился
fd_index:       .ds     2       ; куда класть следующий указатель
fd_saved:       .ds     1       ; какая страница была до нас
fd_len:         .ds     2

        .area   _CODE

;; Прочитать открытый каталог целиком в страницу.
;;
;; Каталог должен быть уже открыт (_fat_dir_open). Выход: CY=0;
;; _fat_dir_count - сколько записей, _fat_dir_full = 1 - влезло не всё.
_fat_dir_load::
        xor     a
        ld      (_fat_dir_full), a
        ld      hl, #0
        ld      (_fat_dir_count), hl

        ld      a, (_fat_port_shadow)
        and     #0x07
        ld      (fd_saved), a
        ld      a, (_fat_page_dir)
        call    _fat_page_set

        ld      hl, #DIR_FIRST
        ld      (DIR_DATA), hl
        ld      hl, #DIR_INDEX
        ld      (fd_index), hl
        ld      hl, #0
        ld      (DIR_COUNT), hl

fdl_next:
        call    _fat_dir_next
        jr      c, fdl_io
        or      a
        jr      z, fdl_done

        ;; Метку тома пропускаем, "." и ".." оставляем навигатору.
        ld      a, (_fat_ent_attr)
        and     #0x08
        jr      nz, fdl_next

        call    fdl_put
        jr      nc, fdl_next
        ;; Не влезло - дальше складывать некуда, но то, что уже собрано,
        ;; годится.
        ld      a, #1
        ld      (_fat_dir_full), a

fdl_done:
        ld      hl, (_fat_dir_count)
        ld      (DIR_COUNT), hl
        ld      a, (fd_saved)
        call    _fat_page_set
        or      a
        ret

fdl_io:
        ld      a, (fd_saved)
        call    _fat_page_set
        scf
        ret

;; Уложить очередную запись. CY=1 - места больше нет.
fdl_put:
        ;; Длина имени с нулём.
        ld      hl, #_fat_ent_name
        ld      bc, #0
fdp_len:
        ld      a, (hl)
        inc     hl
        inc     bc
        or      a
        jr      nz, fdp_len
        ld      (fd_len), bc

        ;; Хватит ли места: данные плюс запись плюс два байта указателя не
        ;; должны дойти до указателей.
        ld      hl, (DIR_DATA)
        ld      bc, (fd_len)
        add     hl, bc
        ld      bc, #9+2
        add     hl, bc
        ld      de, (fd_index)
        or      a
        sbc     hl, de
        jr      nc, fdp_full

        ;; Заголовок записи.
        ld      hl, (DIR_DATA)
        ld      de, (fd_index)
        ld      a, l            ; указатель на неё; через (DE) ходит только A
        ld      (de), a
        inc     de
        ld      a, h
        ld      (de), a
        ld      de, (fd_index)
        dec     de
        dec     de
        ld      (fd_index), de

        ld      a, (_fat_ent_attr)
        ld      (hl), a
        inc     hl
        ld      de, #_fat_ent_clus
        call    fdp_copy4
        ld      de, #_fat_ent_size
        call    fdp_copy4

        ld      de, #_fat_ent_name
        ld      bc, (fd_len)
        ex      de, hl
        ldir                    ; имя вместе с нулём
        ex      de, hl
        ld      (DIR_DATA), hl

        ld      hl, (_fat_dir_count)
        inc     hl
        ld      (_fat_dir_count), hl
        or      a
        ret

fdp_full:
        scf
        ret

fdp_copy4:
        ld      b, #4
fdp_c4:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    fdp_c4
        ret

;; Достать запись номер HL в _fat_ent_*.
;;
;; Через то же место, куда кладёт обход каталога: вид записи один и тот же
;; у навигатора и у поиска по имени.
;;
;; CY=1 - такой записи нет.
_fat_dir_get::
        ld      de, (_fat_dir_count)
        push    hl
        or      a
        sbc     hl, de
        pop     hl
        jr      c, fdg_ok
        scf
        ret
fdg_ok:
        ld      a, (_fat_port_shadow)
        and     #0x07
        ld      (fd_saved), a
        ld      a, (_fat_page_dir)
        call    _fat_page_set

        ;; Указатель на запись лежит по 0xFFFE - i*2.
        add     hl, hl
        ex      de, hl
        ld      hl, #DIR_INDEX
        or      a
        sbc     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ex      de, hl          ; HL -> сама запись

        ld      a, (hl)
        ld      (_fat_ent_attr), a
        inc     hl
        ld      de, #_fat_ent_clus
        call    fdg_copy4
        ld      de, #_fat_ent_size
        call    fdg_copy4

        ld      de, #_fat_ent_name
fdg_name:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        or      a
        jr      nz, fdg_name

        ld      a, (fd_saved)
        call    _fat_page_set
        or      a
        ret

;; (DE) <- (HL), 4 байта.
fdg_copy4:
        ld      b, #4
fdg_c4:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        djnz    fdg_c4
        ret
