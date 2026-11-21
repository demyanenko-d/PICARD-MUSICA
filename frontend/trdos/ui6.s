;; ui6.s - текст шрифтом 6x8 и адрес строки экрана.
;;
;; Шрифт (_fw_font) разложен по строкам: страница 256 байт на строку глифа
;; 1..7, код символа - младший байт адреса, глиф в битах 7..3. Строка 0 у
;; шрифта пустая и не хранится. Следующая строка глифа - inc старшего
;; байта, как и следующая строка точек экрана.

        .module ui6
        .globl  _fw_font

FONT_LINES .equ 7
LINE_COL   .equ 1               ; ui_line6: знакоместа 1..30
LINE_BYTES .equ 30

        .area   _DATA
_ui6_x::        .ds 1           ; пиксель по горизонтали, сдвигается на 6 за символ
_ui6_row::      .ds 1           ; строка знакомест 0..23
_ui6_max::      .ds 1           ; сколько символов ещё можно
_ui6_buf::      .ds 40          ; ui_line6: коды символов
line_groups:    .ds 1
line_tail:      .ds 1
line_hi:        .ds 1

        .area   _CODE

;; u8 *scr(u8 col, u8 y) - A = знакоместо, L = строка пикселей; адрес в DE.
_scr::
        ld      e, a
        ld      a, l
        and     #0x38
        rlca
        rlca
        or      e
        ld      e, a
        ld      a, l
        and     #0xC0
        rrca
        rrca
        rrca
        ld      d, a
        ld      a, l
        and     #0x07
        or      d
        or      #0x40
        ld      d, a
        ret

;; void ui_puts6(const char *s) - HL = строка. До нуля или ui6_max символов
;; с пикселя ui6_x. Рисует поверх (OR): поле под текстом очищает
;; вызывающий. Сдвиг глифа одинаков для всех строк символа, поэтому
;; подпрограмма сдвига выбирается один раз на символ: до 4 - вправо,
;; 5..7 - влево на 8 - сдвиг.
_ui_puts6::
puts_next:
        ld      a, (_ui6_max)
        or      a
        ret     z
        dec     a
        ld      (_ui6_max), a
        ld      a, (hl)
        or      a
        ret     z
        inc     hl
        push    hl

        ; DE = строка 1 глифа
        ld      e, a
        ld      d, #>_fw_font

        ; подпрограмма сдвига для x & 7
        ld      a, (_ui6_x)
        and     #7
        add     a, a
        ld      l, a
        ld      h, #0
        ld      bc, #sh_tab
        add     hl, bc
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ld      (puts_call + 1), hl

        ; HL = адрес строки 1 знакоместа: 0x41 | row & 0x18, (row & 7) << 5 | x >> 3
        ld      a, (_ui6_row)
        ld      c, a
        and     #0x18
        or      #0x41
        ld      h, a
        ld      a, c
        and     #7
        rrca
        rrca
        rrca
        ld      l, a
        ld      a, (_ui6_x)
        rrca
        rrca
        rrca
        and     #0x1f
        or      l
        ld      l, a

        ld      b, #FONT_LINES
puts_line:
        ld      a, (de)
        inc     d
        or      a
        jr      z, puts_skip
        ld      c, #0
puts_call:
        call    0
puts_skip:
        inc     h
        djnz    puts_line

        ld      a, (_ui6_x)
        add     a, #6
        ld      (_ui6_x), a
        pop     hl
        jr      puts_next

;; Сдвиг глифа A и OR в (HL), (HL+1). Вправо: A - старший байт, C - младший.
sh4:    srl     a
        rr      c
sh3:    srl     a
        rr      c
sh2:    srl     a
        rr      c
sh1:    srl     a
        rr      c
sh0:    or      (hl)
        ld      (hl), a
        inc     l
        ld      a, c
        or      (hl)
        ld      (hl), a
        dec     l
        ret
;; Влево на 8 - сдвиг: C - старший байт, A - младший.
sh5:    sla     a
        rl      c
sh6:    sla     a
        rl      c
sh7:    sla     a
        rl      c
        inc     l
        or      (hl)
        ld      (hl), a
        dec     l
        ld      a, c
        or      (hl)
        ld      (hl), a
        ret

sh_tab: .dw     sh0, sh1, sh2, sh3, sh4, sh5, sh6, sh7

;; void ui_line6(u8 row, u8 n) - A = строка знакомест, L = сколько символов
;; в ui6_buf (до 40). Строки точек 1..7 знакомест 1..30, текст с x = 8.
;;
;; Каждый байт пишется один раз, сразу итоговым: старое содержимое не
;; стирается заранее и пустая строка на экране не появляется. Четыре
;; символа - ровно три байта: символ 0 с бита 7 байта 0, символ 1 с бита 1
;; (биты 1..0 байта 0 и 7..5 байта 1), символ 2 с бита 3 (3..0 байта 1 и
;; бит 7 байта 2), символ 3 с бита 5. Сдвиг - циклический, биты 2..0 глифа
;; нулевые, поэтому маска нужна только на разрезанных символах.
_ui_line6::
        push    ix
        ld      c, a

        ; добить пробелами до кратного 4: группа - всегда 4 символа
        ld      e, l
        ld      d, #0
        ld      hl, #_ui6_buf
        add     hl, de
line_pad:
        ld      a, e
        and     #3
        jr      z, line_padded
        ld      (hl), #0x20
        inc     hl
        inc     e
        jr      line_pad
line_padded:
        ld      a, e
        rrca
        rrca
        ld      (line_groups), a        ; групп 0..10
        ld      b, a
        add     a, a
        add     a, b
        neg
        add     a, #LINE_BYTES
        ld      (line_tail), a          ; 30 - 3 * групп: нулевые байты

        ; DE = строка точек 1, знакоместо 1
        ld      a, c
        and     #0x18
        or      #0x41
        ld      d, a
        ld      (line_hi), a
        ld      a, c
        and     #7
        rrca
        rrca
        rrca
        or      #LINE_COL
        ld      e, a

        ld      ix, #_ui6_buf
        ld      a, (line_groups)
        or      a
        jr      z, line_tail_start
line_group:
        ld      h, #>_fw_font
        ld      a, (line_hi)
        ld      d, a
line_glyph:
        ld      l, 0(ix)
        ld      c, (hl)
        ld      l, 1(ix)
        ld      a, (hl)
        rlca
        rlca
        ld      b, a
        and     #0x03
        or      c
        ld      (de), a
        inc     e
        ld      a, b
        and     #0xE0
        ld      c, a
        ld      l, 2(ix)
        ld      a, (hl)
        rrca
        rrca
        rrca
        rrca
        ld      b, a
        and     #0x0F
        or      c
        ld      (de), a
        inc     e
        ld      a, b
        and     #0x80
        ld      c, a
        ld      l, 3(ix)
        ld      a, (hl)
        rrca
        rrca
        or      c
        ld      (de), a
        dec     e
        dec     e
        inc     h
        inc     d
        ld      a, d
        and     #7
        jr      nz, line_glyph          ; после строки 7 - снова 0

        inc     e
        inc     e
        inc     e
        ld      bc, #4
        add     ix, bc
        ld      hl, #line_groups
        dec     (hl)
        jr      nz, line_group

line_tail_start:
        ld      a, (line_tail)
        or      a
        jr      z, line_done
        ld      c, a
        ld      a, (line_hi)
        ld      d, a
line_tail_row:
        ld      b, c
        ld      l, e
        xor     a
line_tail_byte:
        ld      (de), a
        inc     e
        djnz    line_tail_byte
        ld      e, l
        inc     d
        ld      a, d
        and     #7
        jr      nz, line_tail_row
line_done:
        pop     ix
        ret
