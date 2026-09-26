        .module wc_resident_call
        .area _CODE
        .globl _wc_resident_call
_wc_resident_call::
        ex      de, hl                  ; HL = addr (from DE = 2nd arg)
        ; A = page (1st arg, already in A)
        call    0x6028                  ; resident call - returns here
        ret
