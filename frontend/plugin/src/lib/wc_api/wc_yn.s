        .module wc_yn
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_yn
_wc_yn::
        ex      af, af'
        push    ix
        ld      a, #0x08
        call    WC_ENTRY
        ld      a, #0
        jr      nz, _wc_yn_end
        inc     a
_wc_yn_end:
        pop     ix
        ret
