        .module wc_gvmod
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gvmod
_wc_gvmod::
        ex      af, af'
        push    ix
        ld      a, #0x42
        call    WC_ENTRY
        pop     ix
        ret
