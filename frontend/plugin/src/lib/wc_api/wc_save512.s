        .module wc_save512
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_save512
_wc_save512::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      b, 4(ix)
        ; HL = src (DON'T ex de, hl!)
        ld      a, #0x31
        call    WC_ENTRY
        pop     ix
        pop     hl
        inc     sp
        jp      (hl)
