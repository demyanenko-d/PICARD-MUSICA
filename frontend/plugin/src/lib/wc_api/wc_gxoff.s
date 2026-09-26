        .module wc_gxoff
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gxoff
_wc_gxoff::
        push    ix
        ld      a, #0x44
        call    WC_ENTRY
        pop     ix
        ret
