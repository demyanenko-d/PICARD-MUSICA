        .module wc_prwow
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_prwow
_wc_prwow::
        push    ix
        push    hl
        pop     ix
        ld      a, #0x01
        call    WC_ENTRY
        pop     ix
        ret
