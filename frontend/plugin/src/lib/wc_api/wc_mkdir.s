        .module wc_mkdir
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mkdir
_wc_mkdir::
        push    ix
        ld      a, #0x49
        call    WC_ENTRY
        pop     ix
        ret
