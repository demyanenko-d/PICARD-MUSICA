        .module wc_mkfile
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_mkfile
_wc_mkfile::
        push    ix
        ld      a, #0x48
        call    WC_ENTRY
        pop     ix
        ret
