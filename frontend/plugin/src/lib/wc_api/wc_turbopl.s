        .module wc_turbopl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_turbopl
_wc_turbopl::
        push    ix
        ld      c, l                    ; C = param (2-й аргумент)
        ld      b, a                    ; B = mode (1-й аргумент)
        ld      a, #0x0E
        call    WC_ENTRY
        pop     ix
        ret
