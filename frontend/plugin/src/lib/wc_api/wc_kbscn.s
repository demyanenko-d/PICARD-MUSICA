        .module wc_kbscn
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_kbscn
_wc_kbscn::
        ex      af, af'                 ; A' = mode
        push    ix
        ld      a, #0x2A
        call    WC_ENTRY
        pop     ix
        ret
