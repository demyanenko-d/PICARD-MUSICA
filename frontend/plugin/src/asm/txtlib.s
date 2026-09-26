;------------------------------------------------------------------------------
; txtlib.s - буферы строк на Z80, ABI под прототипы inc/txtlib.h.
;
; Буфер: buf[0] = ёмкость, buf[1] = длина, buf[2..] = символы.
;
; sdcccall(1): 1-й ptr/u16 -> HL, 2-й ptr/u16 -> DE, 1-й u8 -> A, 2-й u8
; при первом u8 -> L, остальное на стек (чистит вызванный).
; Возврат u8 -> A, u16 -> HL.
;------------------------------------------------------------------------------

        .module txtlib_mod

        .globl  _buf_append_str
        .globl  _buf_append_char
        .globl  _buf_append_u8_hex
        .globl  _buf_append_u16_dec
        .globl  _buf_append_u32_dec
        .globl  _buf_append_mmss
        .globl  _print_line

        .globl  _wc_prsrw_attr

        .area   _CODE

;----------------------------------------------------------------------
; Helper: nibble (0-15 in A) -> ASCII char ('0'-'F') in A
;----------------------------------------------------------------------
_nib2asc:
        cp      a, #10
        jr      c, 001$
        add     a, #7
001$:   add     a, #0x30
        ret

;======================================================================
; uint8_t buf_append_str(char *buf, const char *src)
;   HL=buf  DE=src  ->  A=added         (no stack params)
;======================================================================
_buf_append_str::
        ld      c, (hl)                 ; C = max
        inc     hl
        ld      b, (hl)                 ; B = curlen
        ld      a, c
        sub     a, b                    ; A = remaining
        jr      z, bas_z
        jr      c, bas_z
        push    hl                      ; save &buf[1]
        ld      c, a                    ; C = remaining
        inc     hl                      ; HL -> buf[2]
        ld      a, b
        add     a, l
        ld      l, a
        jr      nc, bas_nc
        inc     h
bas_nc: ld      b, #0                   ; B = added
bas_lp: ld      a, (de)
        or      a, a
        jr      z, bas_dn
        ld      (hl), a
        inc     hl
        inc     de
        inc     b
        dec     c
        jr      nz, bas_lp
bas_dn: pop     hl                      ; &buf[1]
        ld      a, (hl)
        add     a, b
        ld      (hl), a
        ld      a, b
        ret
bas_z:  xor     a, a
        ret

;======================================================================
; uint8_t buf_append_char(char *buf, char c)
;   HL=buf  stack: c(1)  ->  A=0|1      callee-cleans 1
;======================================================================
_buf_append_char::
        push    ix
        ld      ix, #0
        add     ix, sp
        ; 4(ix) = c
        ld      c, (hl)                 ; max
        inc     hl
        ld      a, (hl)                 ; curlen
        cp      a, c
        jr      nc, bac_f
        ld      e, a
        inc     a
        ld      (hl), a                 ; buf[1]++
        inc     hl                      ; -> buf[2]
        ld      a, e
        add     a, l
        ld      l, a
        jr      nc, bac_nc
        inc     h
bac_nc: ld      a, 4 (ix)
        ld      (hl), a
        ld      a, #1
        jr      bac_r
bac_f:  xor     a, a
bac_r:  pop     ix
        pop     hl
        inc     sp                      ; skip c (1 byte)
        jp      (hl)

;======================================================================
; uint8_t buf_append_u8_hex(char *buf, uint8_t val)
;   HL=buf  stack: val(1)  ->  A=0|2    callee-cleans 1
;======================================================================
_buf_append_u8_hex::
        push    ix
        ld      ix, #0
        add     ix, sp
        ; 4(ix) = val
        ld      c, (hl)                 ; max
        inc     hl
        ld      a, c
        sub     a, (hl)                 ; remaining
        cp      a, #2
        jr      c, bh8_f
        ld      b, (hl)                 ; curlen
        ld      a, b
        add     a, #2
        ld      (hl), a                 ; buf[1] += 2
        inc     hl
        ld      a, b
        add     a, l
        ld      l, a
        jr      nc, bh8_nc
        inc     h
bh8_nc: ld      a, 4 (ix)
        rrca
        rrca
        rrca
        rrca
        and     a, #0x0F
        call    _nib2asc
        ld      (hl), a
        inc     hl
        ld      a, 4 (ix)
        and     a, #0x0F
        call    _nib2asc
        ld      (hl), a
        ld      a, #2
        jr      bh8_r
bh8_f:  xor     a, a
bh8_r:  pop     ix
        pop     hl
        inc     sp
        jp      (hl)

;----------------------------------------------------------------------
; Shared helper: emit one decimal digit.
;   A  = digit (0-9)
;   B  = "added" counter (0 = not-yet-started -> suppress leading 0)
;   C  = remaining buffer space
;   IY = destination pointer (advanced on write)
;----------------------------------------------------------------------
_emit_digit:
        push    af
        or      a, b                    ; z iff digit==0 AND not started
        jr      z, ed_skip
        ld      a, c
        or      a, a
        jr      z, ed_skip              ; no space
        dec     c
        pop     af
        add     a, #0x30
        ld      0 (iy), a
        inc     iy
        inc     b
        ret
ed_skip:
        pop     af
        ret

;----------------------------------------------------------------------
; Shared helper: extract one 32-bit decimal digit via repeated
; subtraction of a 4-byte constant taken from inline data after CALL.
;
; Usage:   call _d32_digit
;          .db  lo, b1, b2, hi   ; 32-bit subtrahend (LE)
;
; In:  DEHL = 32-bit value, -3(ix) = scratch,
;      B = started/added, C = remaining, IY = dest ptr
; Out: DEHL = remainder, digit emitted via _emit_digit
;----------------------------------------------------------------------
_d32_digit:
        ex      (sp), hl                ; HL = inline-data ptr, (SP)=val_lo
        ld      a, (hl)
        ld      (d32c0 + 1), a
        inc     hl
        ld      a, (hl)
        ld      (d32c1 + 1), a
        inc     hl
        ld      a, (hl)
        ld      (d32c2 + 1), a
        inc     hl
        ld      a, (hl)
        ld      (d32c3 + 1), a
        inc     hl                       ; HL past data = true ret addr
        ex      (sp), hl                ; (SP)=true ret, HL=val_lo restored
        ld      -3 (ix), #0xFF
d32d_lp:inc     -3 (ix)
        push    de
        push    hl
        ld      a, l
d32c0:  sub     a, #0x00                ; patched: subtrahend byte 0
        ld      l, a
        ld      a, h
d32c1:  sbc     a, #0x00                ; patched: byte 1
        ld      h, a
        ld      a, e
d32c2:  sbc     a, #0x00                ; patched: byte 2
        ld      e, a
        ld      a, d
d32c3:  sbc     a, #0x00                ; patched: byte 3
        ld      d, a
        jr      c, d32d_un
        pop     af
        pop     af
        jr      d32d_lp
d32d_un:pop     hl
        pop     de
        ld      a, -3 (ix)
        jp      _emit_digit             ; tail-call, ret goes past inline data

;----------------------------------------------------------------------
; Shared 16-bit decimal division: 10000/1000/100/10/ones.
; Entry _div_10k_ones: full 16-bit decimal (10000..1).
; Entry _div_1k_ones:  skip 10000, do 1000..1.
; In:  DE=value, B=added counter, C=remaining, IY=dst.
; Out: B=updated, DE=remainder. A,HL clobbered.
;----------------------------------------------------------------------
_div_10k_ones:
        ;-- 10000 (neg 0xD8F0) --
        ld      a, #0xFF
_d10k:  inc     a
        ld      hl, #0xD8F0
        add     hl, de
        jr      nc, _d10ke
        ex      de, hl
        jr      _d10k
_d10ke: call    _emit_digit
_div_1k_ones:
        ;-- 1000 (neg 0xFC18) --
        ld      a, #0xFF
_d1k:   inc     a
        ld      hl, #0xFC18
        add     hl, de
        jr      nc, _d1ke
        ex      de, hl
        jr      _d1k
_d1ke:  call    _emit_digit
        ;-- 100 (neg 0xFF9C) --
        ld      a, #0xFF
_d100:  inc     a
        ld      hl, #0xFF9C
        add     hl, de
        jr      nc, _d100e
        ex      de, hl
        jr      _d100
_d100e: call    _emit_digit
        ;-- 10 (neg 0xFFF6) --
        ld      a, #0xFF
_d10:   inc     a
        ld      hl, #0xFFF6
        add     hl, de
        jr      nc, _d10e
        ex      de, hl
        jr      _d10
_d10e:  call    _emit_digit
        ;-- ones (always emit) --
        ld      a, c
        or      a, a
        ret     z
        ld      a, e
        add     a, #0x30
        ld      0 (iy), a
        inc     iy
        inc     b
        ret

;======================================================================
; uint8_t buf_append_u16_dec(char *buf, uint16_t val)
;   HL=buf  DE=val  ->  A=added         (no stack params)
;======================================================================
_buf_append_u16_dec::
        push    iy
        ld      c, (hl)
        inc     hl
        ld      a, c
        sub     a, (hl)
        jr      z, d16_z
        jr      c, d16_z
        ld      c, a                    ; C = remaining
        ld      b, (hl)                 ; B = curlen (temp)
        push    hl                      ; save &buf[1]
        inc     hl
        ld      a, b
        add     a, l
        ld      l, a
        jr      nc, d16_nc
        inc     h
d16_nc: push    hl
        pop     iy                      ; IY = dst
        ld      b, #0                   ; B = added/started
        call    _div_10k_ones
        pop     hl                      ; &buf[1]
        ld      a, (hl)
        add     a, b
        ld      (hl), a
        ld      a, b
        pop     iy
        ret
d16_z:  xor     a, a
        pop     iy
        ret

;======================================================================
; uint8_t buf_append_u32_dec(char *buf, uint32_t val)
;   HL=buf  stack: val(4)  ->  A=added  callee-cleans 4
;   4(ix)=byte0(LSB)  5(ix)=byte1  6(ix)=byte2  7(ix)=byte3(MSB)
;======================================================================
_buf_append_u32_dec::
        push    ix
        ld      ix, #0
        add     ix, sp
        push    iy                      ; -2,-1(ix)
        push    af                      ; -4,-3(ix) scratch
        push    af                      ; -6,-5(ix) scratch
        ; -3(ix) used as digit counter per power
        ld      c, (hl)
        inc     hl
        ld      a, c
        sub     a, (hl)
        jp      z, d32_z
        jp      c, d32_z
        ld      c, a                    ; C = remaining
        ld      b, (hl)                 ; B = curlen (temp)
        push    hl                      ; save &buf[1] -> -8,-7(ix)
        inc     hl
        ld      a, b
        add     a, l
        ld      l, a
        jr      nc, d32_nc
        inc     h
d32_nc: push    hl
        pop     iy                      ; IY = dst
        ; Load 32-bit val into DEHL
        ld      l, 4 (ix)
        ld      h, 5 (ix)
        ld      e, 6 (ix)
        ld      d, 7 (ix)
        ld      b, #0                   ; B = added/started
        ;-- 1,000,000 / 100,000 / 10,000 via shared subroutine --
        call    _d32_digit
        .db     0x40, 0x42, 0x0F, 0x00  ; 1,000,000
        call    _d32_digit
        .db     0xA0, 0x86, 0x01, 0x00  ; 100,000
        call    _d32_digit
        .db     0x10, 0x27, 0x00, 0x00  ; 10,000
        ;-- remainder < 10000, fits in HL -> move to DE --
        ex      de, hl
        call    _div_1k_ones
        pop     hl                      ; &buf[1]
        ld      a, (hl)
        add     a, b
        ld      (hl), a
        ld      a, b                    ; return value
d32_exit:
        ; clean locals, restore, callee-clean 4
        pop     de                      ; eat -6,-5
        pop     de                      ; eat -4,-3
        pop     iy
        pop     ix
        pop     hl                      ; ret addr
        pop     de                      ; skip val lo
        pop     de                      ; skip val hi
        jp      (hl)
d32_z:  xor     a, a
        jr      d32_exit

;======================================================================
; void print_line(wc_window_t *win, uint8_t y, const char *buf,
;                 uint8_t attr)
;   HL=win  stack: y(1) buf(2) attr(1)  callee-cleans 4
;   Prints buf content + spaces padding to fill content width.
;======================================================================
_print_line::
        push    ix
        ld      ix, #0
        add     ix, sp
        ; 4(ix)=y  5,6(ix)=buf  7(ix)=attr
        push    af                      ; -2,-1(ix): cw at -1, slen at -2
        push    hl                      ; -4,-3(ix): win
        ;-- cw = get_content_width(win) --
        ld      de, #4
        add     hl, de
        ld      a, (hl)
        or      a, a
        jr      nz, pl_nz
        ld      a, #96
pl_nz:  sub     a, #4
        ld      -1 (ix), a
        ;-- slen = buf[1] --
        ld      l, 5 (ix)
        ld      h, 6 (ix)
        inc     hl
        ld      a, (hl)
        ld      -2 (ix), a
        ;-- print_len = min(slen, cw) --
        cp      a, -1 (ix)
        jr      c, pl_min
        ld      a, -1 (ix)
pl_min: ld      c, a                    ; C = print_len
        ;-- 1st wc_prsrw_attr(win, data, y, 2, print_len, attr) --
        ld      a, 7 (ix)
        push    af
        inc     sp
        ld      b, #0
        push    bc
        ld      h, #2
        ld      l, 4 (ix)
        push    hl
        ; DE = buf + 2
        ld      a, 5 (ix)
        add     a, #2
        ld      e, a
        ld      a, 6 (ix)
        adc     a, #0
        ld      d, a
        ; HL = win
        ld      l, -4 (ix)
        ld      h, -3 (ix)
        call    _wc_prsrw_attr
        ;-- padding? --
        ld      a, -2 (ix)
        cp      a, -1 (ix)
        jr      nc, pl_done
        ; pad = cw − slen
        ld      a, -1 (ix)
        sub     a, -2 (ix)
        ld      c, a
        ; Fill buf[2..2+pad-1] with 0x20 (runtime space fill)
        ld      l, 5 (ix)
        ld      h, 6 (ix)
        inc     hl
        inc     hl                      ; HL = &buf[2]
        ld      b, c                    ; B = pad count
        ld      a, #0x20
pf_fill:ld      (hl), a
        inc     hl
        djnz    pf_fill                 ; B=0 after loop
        ;-- 2nd wc_prsrw_attr(win, &buf[2], y, 2+slen, pad, attr) --
        ld      a, 7 (ix)
        push    af
        inc     sp
        push    bc                      ; len=pad (B=0, C=pad)
        ld      a, -2 (ix)
        add     a, #2
        ld      h, a                    ; x = 2 + slen
        ld      l, 4 (ix)              ; y
        push    hl
        ld      e, 5 (ix)
        ld      d, 6 (ix)
        inc     de
        inc     de                      ; DE = &buf[2]
        ld      l, -4 (ix)
        ld      h, -3 (ix)
        call    _wc_prsrw_attr
pl_done:
        ld      sp, ix
        pop     ix
        pop     hl
        pop     af
        pop     af                      ; callee-clean 4
        jp      (hl)

;----------------------------------------------------------------------
; buf_append_mmss(buf, sec) - append "MM:SS" (5 chars) to buffer
;
; sdcccall(1):  HL = buf,  DE = sec (uint16_t, 0..5999)
; No division - subtraction loops only (~30 bytes code).
;
; IX обязан вернуться в целости: SDCC держит в нём кадр вызывающей
; функции (___sdcc_enter_ix). Остальные подпрограммы файла его сохраняют.
;----------------------------------------------------------------------
_buf_append_mmss:
        push    ix              ; кадр вызывающего - вернуть в целости
        push    hl
        pop     ix              ; IX = buf
        ; DE = total seconds.  Divide by 60 via subtraction -> B=min, E=sec
        ld      b, #0
mmss_d60:
        ld      a, e
        sub     a, #60
        ld      c, a
        ld      a, d
        sbc     a, #0
        jr      c, mmss_d60_done
        ld      d, a
        ld      e, c
        inc     b
        jr      mmss_d60
mmss_d60_done:
        ; B = minutes, E = seconds
        push    de              ; save seconds (mmss_2dig clobbers DE)
        ld      a, b
        call    mmss_2dig       ; appends 2 digits of A
        ; Append ':'
        ld      a, 1 (ix)
        ld      c, a
        ld      b, #0
        inc     a
        ld      1 (ix), a
        push    ix
        pop     hl
        add     hl, bc
        inc     hl              ; skip buf[0] (max)
        inc     hl              ; skip buf[1] (len)
        ld      (hl), #0x3A     ; ':'
        ; Append seconds
        pop     de              ; restore E = seconds
        ld      a, e
        call    mmss_2dig
        pop     ix              ; вернуть кадр вызывающего (см. выше)
        ret

; Subroutine: append 2 ASCII digits of A (0..99) to buf at IX
; Destroys A, B, C, DE, HL
mmss_2dig:
        ld      c, #0x30        ; tens digit = '0'
mmss_t10:
        sub     a, #10
        jr      c, mmss_t10_done
        inc     c
        jr      mmss_t10
mmss_t10_done:
        add     a, #10          ; undo last sub
        add     a, #0x30        ; units -> ASCII
        ld      b, a            ; save units
        ; Append tens (C) then units (B)
        ld      a, 1 (ix)       ; len
        ld      l, a
        ld      h, #0
        add     a, #2
        ld      1 (ix), a       ; len += 2
        push    ix
        pop     de              ; DE = buf base
        add     hl, de
        inc     hl              ; skip buf[0] (max)
        inc     hl              ; skip buf[1] (len)
        ld      (hl), c         ; tens
        inc     hl
        ld      (hl), b         ; units
        ret
