        .module wc_dmapl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_dmapl
_wc_dmapl::
        ex      af, af'
        push    ix
        ld      a, #0x0D
        call    WC_ENTRY
        pop     ix
        ret
