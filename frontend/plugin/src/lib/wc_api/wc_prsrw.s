        .module wc_prsrw
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_prsrw
_wc_prsrw::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        push    de                      ; save str (DE = 2nd arg)
        ld      ix, #0
        add     ix, sp
        ld      b, 11(ix)               ; B = len_hi
        ld      c, 10(ix)               ; C = len_lo
        ld      d, 8(ix)                ; D = y
        ld      e, 9(ix)                ; E = x
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = str
        exx
        ld      l, 2(ix)
        ld      h, 3(ix)                ; HL = win
        push    hl
        pop     ix                      ; IX = win
        exx                             ; BC=len, DE=y|x, HL=str
        ld      a, #0x03
        call    WC_ENTRY
        pop     de                      ; remove saved str
        pop     hl                      ; remove saved win
        pop     ix                      ; restore caller IX
        ; callee cleans: y(1)+x(1)+len(2) = 4 bytes
        pop     hl                      ; HL = return address
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        jp      (hl)
