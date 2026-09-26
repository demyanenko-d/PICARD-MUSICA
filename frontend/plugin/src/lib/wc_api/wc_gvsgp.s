        .module wc_gvsgp
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gvsgp
_wc_gvsgp::
        push    ix
        ld      c, a
        ld      a, #0x47
        call    WC_ENTRY
        pop     ix
        ret
