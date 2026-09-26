        .module wc_gdir
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gdir
_wc_gdir::
        push    ix
        ld      a, #0x3F
        call    WC_ENTRY
        pop     ix
        ret
