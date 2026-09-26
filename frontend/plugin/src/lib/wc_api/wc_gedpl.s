        .module wc_gedpl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gedpl
_wc_gedpl::
        push    ix
        ld      a, #0x0F
        call    WC_ENTRY
        pop     ix
        ret
