        .module wc_findnext
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_findnext
_wc_findnext::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      a, 4(ix)                ; A = flags (from stack)
        ex      af, af'                 ; A' = flags
        ex      de, hl                  ; DE = entry_buf (SDCC HL -> WC DE)
        ld      a, #0x3A
        call    WC_ENTRY
        ld      a, #0
        jr      z, _wc_findnext_end
        inc     a
_wc_findnext_end:
        pop     ix
        ; callee cleans 1 byte (flags)
        pop     hl
        inc     sp
        jp      (hl)
