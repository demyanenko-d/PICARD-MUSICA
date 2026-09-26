        .module wc_mng8vpl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mng8vpl
_wc_mng8vpl::
        ex      af, af'
        push    ix
        ld      a, #0x51
        call    WC_ENTRY
        pop     ix
        ret
