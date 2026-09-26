        .module wc_cursor
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_cursor
_wc_cursor::
        push    ix
        push    hl
        pop     ix
        ld      a, #0x06
        call    WC_ENTRY
        pop     ix
        ret
