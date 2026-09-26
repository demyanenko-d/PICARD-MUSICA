        .module wc_gvtm
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gvtm
_wc_gvtm::
        push    ix
        ld      c, a
        ld      a, #0x45
        call    WC_ENTRY
        pop     ix
        ret
