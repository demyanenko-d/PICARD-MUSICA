        .module wc_mng0vpl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mng0vpl
_wc_mng0vpl::
        ex      af, af'
        push    ix
        ld      a, #0x50
        call    WC_ENTRY
        pop     ix
        ret
