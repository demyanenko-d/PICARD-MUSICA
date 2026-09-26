        .module wc_mezz
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mezz
_wc_mezz::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        ld      ix, #0
        add     ix, sp
        ld      a, 6(ix)                ; A = msg_num
        ld      l, 7(ix)
        ld      h, 8(ix)                ; HL = str
        ld      d, 9(ix)                ; D = y
        ld      e, 10(ix)               ; E = x
        exx
        ex      af, af'                 ; A' = msg_num
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = win
        push    hl
        pop     ix                      ; IX = win
        exx                             ; HL=str, D=y, E=x
        ld      a, #0x0C
        call    WC_ENTRY
        ld      a, d                    ; return: next row
        pop     hl                      ; remove saved win
        pop     ix
        ; callee cleans 5 bytes
        pop     hl                      ; HL = return address
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        jp      (hl)
