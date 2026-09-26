        .module wc_gfile
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gfile
_wc_gfile::
        push    ix
        ld      a, #0x3E
        call    WC_ENTRY
        pop     ix
        ret
