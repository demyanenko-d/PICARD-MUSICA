        .module wc_rresb
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_rresb
_wc_rresb::
        push    ix
        push    hl
        pop     ix
        ld      a, #0x02
        call    WC_ENTRY
        pop     ix
        ret
