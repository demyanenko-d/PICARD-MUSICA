        .module wc_key_wait_release
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_wait_release
_wc_key_wait_release::
        push    ix
        ld      a, #0x2E
        call    WC_ENTRY
        pop     ix
        ret
