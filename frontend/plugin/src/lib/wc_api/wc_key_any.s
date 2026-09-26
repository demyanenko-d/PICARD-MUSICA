        .module wc_key_any
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_any
_wc_key_any::
        push    ix
        ld      a, #0x2D
        call    WC_ENTRY
        pop     ix
        ; WC returns the key in Z (NZ = pressed), A is garbage: Z -> 0/1 in A.
        ld      a, #0x00
        ret     z
        inc     a
        ret
