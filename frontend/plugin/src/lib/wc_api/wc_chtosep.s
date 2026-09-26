        .module wc_chtosep
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_chtosep
_wc_chtosep::
        push    ix
        ld      b, d
        ld      c, e                    ; BC = bufend (from DE)
        ex      de, hl                  ; DE = buf    (from HL)
        ld      a, #0x34
        call    WC_ENTRY
        pop     ix
        ret
