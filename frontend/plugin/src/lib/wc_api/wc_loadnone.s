        .module wc_loadnone
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_loadnone
_wc_loadnone::
        push    ix
        ld      b, a
        ld      a, #0x3D
        call    WC_ENTRY
        pop     ix
        ret
