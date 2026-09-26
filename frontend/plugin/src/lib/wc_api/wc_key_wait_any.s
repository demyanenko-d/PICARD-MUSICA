        .module wc_key_wait_any
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_wait_any
_wc_key_wait_any::
        push    ix
        ld      a, #0x2F
        call    WC_ENTRY
        pop     ix
        ret
