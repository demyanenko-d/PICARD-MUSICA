        .module wc_resident_jump
        .area _CODE
        .globl _wc_resident_jump
_wc_resident_jump::
        ex      de, hl                  ; HL = addr (from DE = 2nd arg)
        ; A = page (1st arg, already in A)
        jp      0x6020                  ; resident jump - never returns!
