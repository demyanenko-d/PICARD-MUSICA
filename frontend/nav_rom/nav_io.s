;; nav_io.s - окно ОЗУ навигатора, 7FFD и время.
;;
;; Пока в окне N1, стек и данные навигатора (они в N0) не видны: копирование
;; экрана идёт без call и push.

        .module nav_io
        .globl  _nav_screen_save
        .globl  _nav_screen_restore
        .globl  _nav_out_7ffd
        .globl  _nav_wait_ms

        .include "obj/nav_page.inc"

PAGE    .equ    0x2000
SCREEN  .equ    0x4000
SCR_LEN .equ    6912

        .area   _CODE

;; u8 nav_screen_save(void): экран 5 - в N1. 0 - сохранён; 1 - окно не
;; переключилось, экран не тронут.
_nav_screen_save::
        ld      a, #1
        out     (NP_BANK_PORT), a
        ld      bc, #0
save_wait:
        ld      a, (PAGE + NP_BANK)
        dec     a
        jr      z, save_copy
        dec     bc
        ld      a, b
        or      a, c
        jr      nz, save_wait
        xor     a
        out     (NP_BANK_PORT), a
        inc     a
        ret
save_copy:
        ld      hl, #SCREEN
        ld      de, #PAGE + NP_SAVE_SCREEN
        ld      bc, #SCR_LEN
        ldir
        jr      to_n0

;; void nav_screen_restore(void): экран 5 - из N1.
_nav_screen_restore::
        ld      a, #1
        out     (NP_BANK_PORT), a
restore_wait:
        ld      a, (PAGE + NP_BANK)
        dec     a
        jr      nz, restore_wait
        ld      hl, #PAGE + NP_SAVE_SCREEN
        ld      de, #SCREEN
        ld      bc, #SCR_LEN
        ldir
to_n0:
        xor     a
        out     (NP_BANK_PORT), a
n0_wait:
        ld      a, (PAGE + NP_BANK)
        or      a
        jr      nz, n0_wait
        ret

;; void nav_out_7ffd(u8 v)
_nav_out_7ffd::
        ld      bc, #0x7FFD
        out     (c), a
        ret

;; void nav_wait_ms(u8 n): n по 3328 тактов, около миллисекунды при 3.5 МГц.
_nav_wait_ms::
        ld      c, a
wait_ms:
        ld      b, #0
wait_inner:
        djnz    wait_inner
        dec     c
        jr      nz, wait_ms
        ret
