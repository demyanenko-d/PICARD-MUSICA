        .module wc_istr
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_istr
_wc_istr::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      a, 4(ix)                ; A = mode (from stack)
        ex      af, af'                 ; A' = mode
        push    hl
        pop     ix                      ; IX = win (HL = 1st arg)
        ld      a, #0x09
        call    WC_ENTRY
        pop     ix
        ; callee cleans 1 byte (mode)
        pop     hl
        inc     sp
        jp      (hl)
