        .module wc_scrlwow
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_scrlwow
_wc_scrlwow::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        ld      ix, #0
        add     ix, sp
        ld      a, 10(ix)               ; A = flags
        ld      b, 8(ix)                ; B = h
        ld      c, 9(ix)                ; C = w
        ld      d, 6(ix)                ; D = y
        ld      e, 7(ix)                ; E = x
        exx
        ex      af, af'                 ; A' = flags
        ld      l, 0(ix)
        ld      h, 1(ix)
        push    hl
        pop     ix                      ; IX = win
        exx                             ; BC=h/w, DE=y/x
        ld      a, #0x54
        call    WC_ENTRY
        pop     hl                      ; remove saved win
        pop     ix
        ; callee cleans 5 bytes
        pop     hl
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        jp      (hl)
