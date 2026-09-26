        .module wc_stream
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_stream
_wc_stream::
        push    ix
        ld      d, a
        ld      a, #0x39
        call    WC_ENTRY
        pop     ix
        ret
