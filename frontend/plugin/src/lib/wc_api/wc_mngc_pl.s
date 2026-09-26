        .module wc_mngc_pl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mngc_pl
_wc_mngc_pl::
        ex      af, af'
        push    ix
        ld      a, #0x00
        call    WC_ENTRY
        pop     ix
        ret
