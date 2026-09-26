        .module wc_gipagpl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gipagpl
_wc_gipagpl::
        push    ix
        ld      a, #0x32
        call    WC_ENTRY
        pop     ix
        ret
