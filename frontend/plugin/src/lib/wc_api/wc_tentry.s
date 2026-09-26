        .module wc_tentry
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_tentry
_wc_tentry::
        push    ix
        ex      de, hl                  ; DE = addr (SDCC HL -> WC DE)
        ld      a, #0x33
        call    WC_ENTRY
        pop     ix
        ret
