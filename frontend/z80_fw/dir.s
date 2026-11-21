;; dir.s - весь каталог разом в страницу.
;;
;; Навигатору нужен готовый список: диск на каждое нажатие не перечитать,
;; а имена переменной длины без таблицы не проиндексировать.
;;
;; Раскладка страницы (окно 0xC000..0xFFFF):
;;
;;   0xC000  слово: сколько записей
;;   0xC002  слово: докуда доросли данные
;;   0xC004  записи подряд, вверх, в том же виде, что _fat_ent_* в памяти:
;;              +0  атрибуты
;;              +1  первый кластер (4)
;;              +5  размер (4)
;;              +9  имя в CP866, ноль в конце
;;   0xFFFE  указатели на записи, по два байта, ВНИЗ: запись i лежит по
;;           адресу из слова 0xFFFE - i*2
;;
;; Данные растут снизу вверх, указатели сверху вниз. Встретились - каталог
;; в страницу не поместился, _fat_dir_full = 1.
;;
;; Перед каждой записью зовётся подпрограмма из _fat_dir_hook, если там не
;; ноль (большой каталог читается секундами). Ей нельзя трогать карту и
;; окно 0xC000; регистры она портит любые, кроме IX.

        .module dir
        .globl  _fat_dir_load
        .globl  _fat_dir_hook
        .globl  _fat_dir_get
        .globl  _fat_dir_count
        .globl  _fat_dir_full

        .globl  _fat_dir_next
        .globl  _fat_ent_name
        .globl  _fat_ent_attr
        .globl  _fat_ent_clus
        .globl  _fat_ent_size
        .globl  _fat_page_dir
        .globl  fat_page_enter
        .globl  fat_page_leave

DIR_COUNT .equ  0xC000
DIR_DATA  .equ  0xC002
DIR_FIRST .equ  0xC004
DIR_INDEX .equ  0xFFFE

        .area   _DATA

_fat_dir_count::.ds     2       ; сколько записей уложилось
_fat_dir_full:: .ds     1       ; 1 - каталог в страницу не поместился
_fat_dir_hook:: .ds     2       ; 0 или подпрограмма приложения перед записью
fd_index:       .ds     2       ; куда класть следующий указатель
fd_len:         .ds     2       ; длина записи: 9 и имя с нулём

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

        ld      a, (_fat_page_dir)
        call    fat_page_enter

        ld      hl, #DIR_FIRST
        ld      (DIR_DATA), hl
        ld      hl, #DIR_INDEX
        ld      (fd_index), hl
        ld      hl, #0
        ld      (DIR_COUNT), hl

fdl_next:
        ld      hl, (_fat_dir_hook)
        ld      a, h
        or      l
        call    nz, fdl_hook
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
        call    fat_page_leave
        or      a
        ret

fdl_io:
        call    fat_page_leave
        scf
        ret

fdl_hook:
        jp      (hl)

;; Уложить очередную запись. CY=1 - места больше нет.
fdl_put:
        ;; Длина записи: 9 байт полей и имя с нулём.
        ld      hl, #_fat_ent_name
        ld      bc, #9
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
        add     hl, bc
        inc     hl
        inc     hl
        ld      de, (fd_index)
        or      a
        sbc     hl, de
        jr      nc, fdp_full

        ;; Указатель на запись.
        ld      de, (DIR_DATA)
        ld      hl, (fd_index)
        ld      (hl), e
        inc     hl
        ld      (hl), d
        dec     hl
        dec     hl
        dec     hl
        ld      (fd_index), hl

        ;; Сама запись: поля и имя.
        ld      hl, #_fat_ent_attr
        ld      bc, (fd_len)
        ldir
        ld      (DIR_DATA), de

        ld      hl, (_fat_dir_count)
        inc     hl
        ld      (_fat_dir_count), hl
        or      a
        ret

fdp_full:
        scf
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
        ld      a, (_fat_page_dir)
        call    fat_page_enter

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

        ;; Поля, затем имя до нуля.
        ld      de, #_fat_ent_attr
        ld      bc, #9
        ldir
fdg_name:
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        or      a
        jr      nz, fdg_name

        call    fat_page_leave
        or      a
        ret
