        .module wc_mngcvpl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mngcvpl
_wc_mngcvpl::
        ex      af, af'
        push    ix
        ld      a, #0x41
        call    WC_ENTRY
        pop     ix
        ret
