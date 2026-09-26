        .module wc_fentry
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_fentry
_wc_fentry::
        push    ix
        ; HL = name (sdcccall(1); WC нужен HL - уже на месте!)
        ld      a, #0x3B
        call    WC_ENTRY
        ld      a, #0
        jr      z, _wc_fentry_end
        inc     a
_wc_fentry_end:
        pop     ix
        ret
