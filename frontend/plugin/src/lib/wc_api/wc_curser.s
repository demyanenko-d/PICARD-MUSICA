        .module wc_curser
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_curser
_wc_curser::
        push    ix
        push    hl
        pop     ix
        ld      a, #0x07
        call    WC_ENTRY
        pop     ix
        ret
