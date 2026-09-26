        .module wc_txtpr
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_txtpr
_wc_txtpr::
        push    ix              ; (1) save caller IX
        push    hl              ; (2) save win ptr  (HL = 1st arg)
        push    de              ; (3) save str ptr  (DE = 2nd arg)
        ld      ix, #0
        add     ix, sp
        ld      d, 9(ix)        ; D = y
        ld      e, 8(ix)        ; E = x
        ld      l, 0(ix)
        ld      h, 1(ix)        ; HL = str ptr
        exx                     ; shadow: HL'=str, D'=y, E'=x
        ld      l, 2(ix)
        ld      h, 3(ix)        ; HL = win ptr
        push    hl
        pop     ix              ; IX = win ptr
        exx                     ; restore HL=str, D=y, E=x
        ld      a, #0x0B
        call    WC_ENTRY
        ld      a, d            ; A = return: next Y
        pop     de              ; (3) restore DE
        pop     hl              ; (2) restore HL
        pop     ix              ; (1) restore IX
        pop     hl              ; HL = return address
        inc     sp              ; } discard caller's push bc (2 bytes: y, x)
        inc     sp              ; }
        jp      (hl)            ; return (callee cleans stack args)
