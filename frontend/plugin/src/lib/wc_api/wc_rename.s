        .module wc_rename
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_rename
_wc_rename::
        push    ix
        ld      a, #0x4A
        call    WC_ENTRY
        pop     ix
        ret
