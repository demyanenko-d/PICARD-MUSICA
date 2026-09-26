        .module wc_int_pl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_int_pl
_wc_int_pl::
        ex      af, af'
        push    ix
        ld      a, #0x56
        call    WC_ENTRY
        pop     ix
        ret
