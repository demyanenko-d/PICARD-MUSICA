        .module wc_prsrw_attr
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_prsrw_attr
_wc_prsrw_attr::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        push    de                      ; save str (DE = 2nd arg)
        ld      ix, #0
        add     ix, sp

        ; -- вызов #03 (prsrw) --
        ld      b, 11(ix)               ; B = len_hi
        ld      c, 10(ix)               ; C = len_lo
        ld      d, 8(ix)                ; D = y
        ld      e, 9(ix)                ; E = x
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = str
        exx
        ld      l, 2(ix)
        ld      h, 3(ix)                ; HL = win
        ld      a, 12(ix)               ; A = attr (IX всё ещё -> фрейм)
        push    af                      ; сохраняем attr на стеке
        push    hl
        pop     ix                      ; IX = win
        exx                             ; BC=len, DE=y|x, HL=str; HL'=win
        ld      a, #0x03
        call    WC_ENTRY

        ; -- вызов #04 (priat) --
        pop     af                      ; A = attr
        ex      af, af'                 ; A' = attr
        exx                             ; HL = win (из HL'), BC=len, DE=y|x
        push    hl
        pop     ix                      ; IX = win
        exx                             ; restore BC=len, DE=y|x
        ld      a, #0x04
        call    WC_ENTRY

        pop     de                      ; remove saved str
        pop     hl                      ; remove saved win
        pop     ix                      ; restore caller IX
        ; callee cleans 5 bytes: y(1)+x(1)+len(2)+attr(1)
        pop     hl                      ; HL = return address
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        jp      (hl)
