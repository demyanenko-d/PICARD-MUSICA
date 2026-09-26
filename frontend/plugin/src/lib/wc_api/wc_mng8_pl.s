        .module wc_mng8_pl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mng8_pl
_wc_mng8_pl::
        ex      af, af'
        push    ix
        ld      a, #0x4F
        call    WC_ENTRY
        pop     ix
        ret
