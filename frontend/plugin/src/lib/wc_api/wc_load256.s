        .module wc_load256
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_load256
_wc_load256::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      b, 4(ix)                ; B = blocks (1 byte on stack)
        ; HL = dest (already in HL)
        ld      a, #0x3C
        call    WC_ENTRY
        pop     ix
        pop     hl                      ; HL = return address
        inc     sp                      ; discard 1-byte 'blocks' arg
        jp      (hl)                    ; return (callee cleans stack arg)
