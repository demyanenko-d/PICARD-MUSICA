        .module wc_delfl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_delfl
_wc_delfl::
        push    ix
        ld      a, #0x4B
        call    WC_ENTRY
        pop     ix
        ret
