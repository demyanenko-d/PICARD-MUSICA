        .module wc_nork
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_nork
_wc_nork::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      a, 4(ix)                ; A = val (from stack)
        ex      af, af'                 ; A' = val
        ; HL = addr (1st arg, already in HL)
        ld      a, #0x0A
        call    WC_ENTRY
        pop     ix
        ; callee cleans 1 byte (val)
        pop     hl
        inc     sp
        jp      (hl)
