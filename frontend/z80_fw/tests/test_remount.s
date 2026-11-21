;; test_remount.s - повторное монтирование другой карты.
;;
;; Кэш сектора FAT обязан сбрасываться при монтировании: иначе у второй
;; карты с тем же началом FAT запись кластера берётся из сектора первой.
;;
;; Ход: карта, том, следующий кластер после 3; метка 1 (полигон меняет
;; запись FAT кластера 3); снова карта, том и следующий кластер после 3.
;;
;; Результат по 0x8F00:
;;   +0   0 успех, 1/3 карта не поднялась, 2/4 монтирование не удалось
;;   +1   CY первого _fat_next_clus
;;   +2   _fat_clus после него (4)
;;   +7   CY второго _fat_next_clus
;;   +8   _fat_clus после него (4)

        .module test_remount
        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_next_clus
        .globl  _fat_clus
        .globl  start

RESULT  .equ    0x8F00
DBGMARK .equ    0x33

        .area   _CODE

start::
        ld      sp, #0x8F00

        ld      a, #1
        call    tr_mount
        ld      hl, #RESULT+1
        call    tr_next

        ld      a, #1
        out     (DBGMARK), a

        ld      a, #3
        call    tr_mount
        ld      hl, #RESULT+7
        call    tr_next

        xor     a
        ld      (RESULT+0), a
        halt

;; Карта и том. A = код ошибки карты, A+1 - тома.
tr_mount:
        ld      (RESULT+0), a
        push    af
        call    _sd_init
        pop     bc
        jr      c, tr_fail
        inc     b
        ld      a, b
        ld      (RESULT+0), a
        call    _fat_mount
        ret     nc
tr_fail:
        halt

;; Следующий кластер после 3. HL -> место под CY и кластер.
tr_next:
        push    hl
        ld      hl, #_fat_clus
        ld      (hl), #3
        inc     hl
        xor     a
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (hl), a
        call    _fat_next_clus
        pop     hl
        ld      a, #0
        rla
        ld      (hl), a
        inc     hl
        ld      de, #_fat_clus
        ld      b, #4
tr_copy:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    tr_copy
        ret
