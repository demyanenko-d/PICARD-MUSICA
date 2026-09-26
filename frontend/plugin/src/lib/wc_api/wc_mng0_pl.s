        .module wc_mng0_pl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mng0_pl
_wc_mng0_pl::
        ex      af, af'
        push    ix
        ld      a, #0x4E
        call    WC_ENTRY
        pop     ix
        ret
