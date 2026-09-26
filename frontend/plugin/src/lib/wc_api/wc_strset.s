        .module wc_strset
        .area _CODE
        .globl _wc_strset
_wc_strset::
        push    ix
        ld      ix, #0
        add     ix, sp

        ld      a, 4(ix)     ; c (из-за push ix)
        pop     ix

        ; записываем первый байт
        ld      (hl), a

        ld      b, d
        ld      c, e

        ; len == 1 -> всё
        dec     bc
        ld      a, b
        or      c
        jr      z, _wc_strset_done

        ; DE = HL + 1
        push    hl
        pop     de
        inc     de

        ; BC = len - 1 уже

        ldir

_wc_strset_done:
        ; очистка стека
        pop     bc
        inc     sp
        push    bc
        ret
