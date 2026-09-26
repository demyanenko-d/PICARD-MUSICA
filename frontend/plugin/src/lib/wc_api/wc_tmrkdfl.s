        .module wc_tmrkdfl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_tmrkdfl
_wc_tmrkdfl::
        push    ix
        push    hl                      ; save panel (HL = 1st arg)
        push    de                      ; save filenum (DE = 2nd arg)
        ld      ix, #0
        add     ix, sp
        ld      e, 8(ix)
        ld      d, 9(ix)                ; DE = namebuf (from stack)
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = filenum (from saved DE)
        exx
        ld      l, 2(ix)
        ld      h, 3(ix)                ; HL = panel (from saved HL)
        push    hl
        pop     ix                      ; IX = panel
        exx                             ; HL=filenum, DE=namebuf
        ld      a, #0x36
        call    WC_ENTRY
        pop     de                      ; remove saved filenum
        pop     hl                      ; remove saved panel
        pop     ix
        ; callee cleans 2 bytes (namebuf)
        pop     hl
        inc     sp
        inc     sp
        jp      (hl)
