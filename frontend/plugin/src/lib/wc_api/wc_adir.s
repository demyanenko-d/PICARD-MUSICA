        .module wc_adir
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_adir
_wc_adir::
        ex      af, af'
        push    ix
        ld      a, #0x38
        call    WC_ENTRY
        pop     ix
        ret
