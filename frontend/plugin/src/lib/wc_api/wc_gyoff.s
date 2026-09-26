        .module wc_gyoff
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gyoff
_wc_gyoff::
        push    ix
        ld      a, #0x43
        call    WC_ENTRY
        pop     ix
        ret
