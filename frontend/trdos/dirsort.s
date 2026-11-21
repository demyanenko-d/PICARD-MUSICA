;; dirsort.s - сортировка таблицы каталога: каталоги вперёд, дальше по
;; имени без учёта регистра (латиница и CP866).
;;
;; Таблица - в странице каталога (её включает вызывающий): указатель записи
;; i лежит по 0xFFFE - 2i, запись - атрибут, кластер 4, размер 4, имя до
;; нуля. Переставляются только указатели. Шелл с шагами Сьюры: шаг не меньше
;; числа записей даёт пустой проход, поэтому идут все шаги таблицы.
;;
;; Раз в кадр (по _fw_ticks) - _dir_sort_tick приложения: сотни записей
;; сортируются секунду и дольше, плату надо обслуживать изнутри. Он вправе
;; переключать страницы, но обязан вернуть страницу каталога.

        .module dirsort
        .globl  _fw_ticks
        .globl  _dir_sort_tick

ATTR_DIR  .equ  0x10
NAME_OFS  .equ  9

        .area   _DATA
ds_gapb:  .ds   2               ; шаг в байтах таблицы
ds_pend:  .ds   2               ; адрес указателя записи n - 1
ds_pi:    .ds   2
ds_pjg:   .ds   2
ds_tmp:   .ds   2
ds_gap:   .ds   2               ; где мы в таблице шагов
ds_tick:  .ds   1               ; кадр последнего _dir_sort_tick

        .area   _CODE

;; void dir_sort_ptrs(u16 n) - HL = число записей, n >= 2.
_dir_sort_ptrs::
        push    ix
        add     hl, hl
        ex      de, hl
        ld      hl, #0
        or      a
        sbc     hl, de          ; 0x10000 - 2n = адрес указателя n - 1
        ld      (ds_pend), hl
        ld      hl, #gaps
ds_pass:
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        inc     hl
        ld      a, d
        or      e
        jr      z, ds_done
        ld      (ds_gap), hl
        ex      de, hl
        add     hl, hl
        ld      (ds_gapb), hl
        ex      de, hl
        ld      hl, #0xFFFE
        or      a
        sbc     hl, de          ; указатель записи i = шаг
ds_i:
        ld      (ds_pi), hl
        ld      a, (_fw_ticks)
        ld      hl, #ds_tick
        cp      (hl)
        jr      z, ds_i_cmp
        ld      (hl), a
        call    _dir_sort_tick
ds_i_cmp:
        ld      hl, (ds_pi)
        ld      de, (ds_pend)
        or      a
        sbc     hl, de
        jr      c, ds_next_gap  ; i >= n
        ld      hl, (ds_pi)
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ld      (ds_tmp), de
        ld      ix, (ds_pi)
ds_j:
        push    ix              ; j - шаг: адрес выше на шаг
        pop     hl
        ld      bc, (ds_gapb)
        add     hl, bc
        jr      c, ds_place     ; j < шага
        ld      (ds_pjg), hl
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ex      de, hl
        ld      de, (ds_tmp)
        call    ent_after
        jr      nc, ds_place
        ld      hl, (ds_pjg)
        ld      a, (hl)
        ld      0(ix), a
        inc     hl
        ld      a, (hl)
        ld      1(ix), a
        ld      ix, (ds_pjg)
        jr      ds_j
ds_place:
        ld      de, (ds_tmp)
        ld      0(ix), e
        ld      1(ix), d
        ld      hl, (ds_pi)
        dec     hl
        dec     hl
        jr      ds_i
ds_next_gap:
        ld      hl, (ds_gap)
        jp      ds_pass
ds_done:
        pop     ix
        ret

gaps:   .dw     701, 301, 132, 57, 23, 10, 4, 1, 0

;; HL = запись a, DE = запись b. CY = 1 - a должна идти после b.
ent_after:
        ld      a, (de)
        xor     (hl)
        and     #ATTR_DIR
        jr      z, ea_same
        ld      a, (hl)
        and     #ATTR_DIR       ; a - каталог: раньше b, CY = 0
        ret     nz
        scf
        ret
ea_same:
        ld      bc, #NAME_OFS
        add     hl, bc
        ex      de, hl
        add     hl, bc
        ex      de, hl
        ;; Имя с точки ("." и "..") - раньше любого: знаки ниже точки
        ;; ($, !, #) иначе ставят каталог над ними.
        ld      a, (de)
        cp      #'.'
        jr      z, ea_b_dot
        ld      a, (hl)
        cp      #'.'
        jr      nz, ea_loop
        or      a               ; a с точки, b нет: a раньше, CY = 0
        ret
ea_b_dot:
        ld      a, (hl)
        cp      #'.'
        jr      z, ea_loop      ; оба с точки: "." раньше ".."
        scf                     ; b с точки, a нет: a после
        ret
ea_loop:
        ld      a, (de)
        cp      (hl)
        jr      nz, ea_differ
        or      a               ; совпали целиком: CY = 0
        ret     z
        inc     hl
        inc     de
        jr      ea_loop
ea_differ:
        call    fold
        ld      c, a            ; C = b
        ld      a, (hl)
        call    fold            ; A = a
        cp      c
        jr      nz, ea_decided
        inc     hl              ; различались только регистром
        inc     de
        jr      ea_loop
ea_decided:
        ccf                     ; a > b - после
        ret

;; Регистр вверх: a-z, а-п (0xA0..0xAF) на 0x20; р-я (0xE0..0xEF) на 0x50.
;; Имена приходят и длинные (регистр как у пользователя), и 8.3 (всегда
;; заглавные): без приведения список разъехался бы на две группы.
fold:
        cp      #0x61
        ret     c
        cp      #0x7B
        jr      c, fold_20
        cp      #0xA0
        ret     c
        cp      #0xB0
        jr      c, fold_20
        cp      #0xE0
        ret     c
        cp      #0xF0
        ret     nc
        sub     #0x50
        ret
fold_20:
        sub     #0x20
        ret
