        .module wc_load512
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_load512
_wc_load512::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      b, 4(ix)                ; B = blocks (1 byte on stack)
        ; HL = dest (sdcccall(1) first uint16 -> HL)
        ld      a, #0x30
        call    WC_ENTRY
        pop     ix
        pop     hl                      ; HL = return address
        inc     sp                      ; discard 1-byte 'blocks' arg
        jp      (hl)                    ; return (callee cleans stack arg)
