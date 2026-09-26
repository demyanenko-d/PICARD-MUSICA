        .module wc_gvtl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gvtl
_wc_gvtl::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      c, 4(ix)                ; C = page
        ld      b, a                    ; B = plane
        ld      a, #0x46
        call    WC_ENTRY
        pop     ix
        ; callee cleans 1 byte (page)
        pop     hl
        inc     sp
        jp      (hl)
