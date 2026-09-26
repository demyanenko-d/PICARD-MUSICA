        .module wc_mngv_pl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mngv_pl
_wc_mngv_pl::
        ex      af, af'
        push    ix
        ld      a, #0x40
        call    WC_ENTRY
        pop     ix
        ret
