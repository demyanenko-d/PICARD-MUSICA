;; dots.s - точечная строка инфо.
;;
;; Буфер: 7 строк точек по 10 байт, точка 0 - бит 7 байта 0, видны точки
;; 0..73 (биты 7..6 байта 9), остальные всегда нули. На экране точка -
;; три пикселя "##.", с x = 2; строка точек r - на y = 35 + 2r.
;;
;; Байт экрана - ровно три точки: 8 точек буфера ложатся в 3 байта экрана
;; A, B, C (24 пикселя), и у каждого свои три точки. A - хвост точки 7
;; прошлого байта буфера и точки 0, 1; B - точки 2, 3, 4; C - 5, 6 и голова
;; точки 7. Отсюда три таблицы по 8 байт, сдвиг на 2 пикселя в них уже
;; заложен. Таблицы и буфер - в одной странице 0xBB: выборка через BC.

        .module dots
        .globl  _bus_spectrum
        .globl  _bus_level

DOTS_PAD  .equ  0xBB00          ; ноль перед буфером: "прошлый байт" строки 0
DOTS_BUF  .equ  0xBB01          ; 70 байт, высокая память (см. crt0.s)
DOTS_TAB  .equ  0xBB50          ; таблицы A, B, C по 8 байт
_ui_hi    ==    0xB7C0          ; HiRam экрана (ui.c), до 0xBAFF
SCALE23   .equ  0xBF00          ; x * 23 / 256: уровень в пиксели (см. crt0.s)
DOTS_ROWS .equ  7
DOTS_Y0   .equ  35

        .area   _CODE

;; void dots_init(void) - таблицы на место, чистые буфер и HiRam с 0xBA20:
;; полосы спектра и VU до первого кадра не рисуют старое содержимое памяти.
_dots_init::
        ld      hl, #SCALE23    ; SCALE23[x] - старший байт x * 23
        ld      de, #0
init_scale:
        ld      (hl), d
        ld      a, e
        add     a, #23
        ld      e, a
        jr      nc, init_scale_c
        inc     d
init_scale_c:
        inc     l
        jr      nz, init_scale
        ld      hl, #HI_CLR     ; HiRam от спектра до конца: полосы, VU, максимумы
        ld      b, #HI_CLR_N
        xor     a
init_tail:
        ld      (hl), a
        inc     l
        djnz    init_tail
        ld      hl, #tab_a
        ld      de, #DOTS_TAB
        ld      bc, #24
        ldir
        ; дальше - dots_clear

;; void dots_clear(void)
_dots_clear::
        ld      hl, #DOTS_PAD
        ld      b, #DOTS_ROWS * 10 + 1
clear_loop:
        ld      (hl), #0
        inc     hl
        djnz    clear_loop
        ret

;; void dots_put(u8 col, u8 bits) - столбец col буфера: A = col 0..73,
;; L - биты 0..6 строк 0..6. Точки только добавляются: буфер чистый.
_dots_put::
        ld      c, a
        rrca
        rrca
        rrca
        and     #0x1F
        add     a, #DOTS_BUF & 0xFF
        ld      e, a
        ld      d, #DOTS_BUF >> 8
        ld      a, c
        and     #7
        inc     a
        ld      b, a
        ld      a, #1
put_mask:
        rrca                    ; 0x80 >> (col & 7)
        djnz    put_mask
        ld      c, a
        ld      b, #DOTS_ROWS
put_row:
        srl     l
        jr      nc, put_next
        ld      a, (de)
        or      c
        ld      (de), a
put_next:
        ld      a, e
        add     a, #10
        ld      e, a
        djnz    put_row
        ret

;; HL = адрес строки экрана y = A, знакоместо 0. Портит A, C.
row_addr:
        ld      c, a
        and     #0x07
        or      #0x40
        ld      h, a
        ld      a, c
        and     #0xC0
        rrca
        rrca
        rrca
        or      h
        ld      h, a
        ld      a, c
        and     #0x38
        rlca
        rlca
        ld      l, a
        ret

;; Строка экрана с HL пуста, рамка x = 0 на месте. Портит A, B, L.
row_blank:
        xor     a
        ld      b, #28
blank_loop:
        ld      (hl), a
        inc     l
        djnz    blank_loop
        ; дальше - row_frame

;; Рамка x = 0 строки, в которой стоит HL. Портит A, L.
row_frame:
        ld      a, l
        and     #0xE0
        ld      l, a
        set     7, (hl)
        ret

;; void dots_draw(u8 off) - буфер на экран, знакоместа 0..27 строк y = 35,
;; 37, .. 47: строка экрана r - строка буфера r + off - 7, вне 0..6 - пусто.
;; Въезд снизу: off 1..7.
_dots_draw::
        sub     #7
        ld      (draw_src), a
        ld      a, #DOTS_Y0
draw_row:
        push    af
        call    row_addr
        ld      a, (draw_src)
        cp      #DOTS_ROWS
        jr      c, draw_text
        call    row_blank
        jr      draw_row_end
draw_text:
        ld      e, a            ; DE = DOTS_BUF + 10 * строка
        add     a, a
        add     a, a
        add     a, e
        add     a, a
        add     a, #DOTS_BUF & 0xFF
        ld      e, a
        ld      d, #DOTS_BUF >> 8
        ld      b, #DOTS_TAB >> 8
draw_group:
        ; A: хвост точки 7 прошлого байта буфера, точки 0, 1. Прошлый байт
        ; строки 0 - DOTS_PAD, остальных - байт 9 строки выше: в обоих
        ; бит 0 нулевой.
        dec     e
        ld      a, (de)
        inc     e
        and     #1
        add     a, a
        add     a, a
        ld      c, a
        ld      a, (de)
        rlca
        rlca
        and     #3
        or      c
        add     a, #DOTS_TAB & 0xFF
        ld      c, a
        ld      a, (bc)
        ld      (hl), a
        inc     l
        ; знакоместо 27 - последнее: от байта 9 буфера только A
        ld      a, l
        and     #0x1F
        cp      #28
        jr      z, draw_text_end
        ; B: точки 2, 3, 4
        ld      a, (de)
        rrca
        rrca
        rrca
        and     #7
        add     a, #(DOTS_TAB + 8) & 0xFF
        ld      c, a
        ld      a, (bc)
        ld      (hl), a
        inc     l
        ; C: точки 5, 6 и голова 7
        ld      a, (de)
        and     #7
        add     a, #(DOTS_TAB + 16) & 0xFF
        ld      c, a
        ld      a, (bc)
        ld      (hl), a
        inc     l
        inc     e
        jr      draw_group
draw_text_end:
        call    row_frame
draw_row_end:
        ld      hl, #draw_src
        inc     (hl)
        pop     af
        add     a, #2
        cp      #DOTS_Y0 + 2 * DOTS_ROWS
        jr      c, draw_row
        ret

;; void dots_hscroll(u8 column) - строка на экране на точку влево, A: биты
;; 0..6 - строки 0..6 нового столбца, он встаёт точкой 73. Буфер не
;; трогается. Точка - 3 пикселя: сдвиг на 4 бита (RLD справа налево, A
;; несёт тетраду) и на бит назад (RR слева направо). Слева в бит рамки
;; x = 0 приходит мусор - рамка ставится заново.
_dots_hscroll::
        ld      e, a
        ld      a, #DOTS_Y0
hs_row:
        push    af
        call    row_addr
        ld      a, l
        or      #27
        ld      l, a            ; знакоместо 27
        ld      c, a
        xor     a               ; справа втекают нули
        ld      b, #7
hs_left4:
        rld
        dec     l
        rld
        dec     l
        rld
        dec     l
        rld
        dec     l
        djnz    hs_left4
        inc     l               ; знакоместо 0
        ld      b, #7
hs_right1:
        rr      (hl)
        inc     l
        rr      (hl)
        inc     l
        rr      (hl)
        inc     l
        rr      (hl)
        inc     l
        djnz    hs_right1
        ld      l, c
        ; точка 73 - пиксели x = 221, 222: биты 2, 1 знакоместа 27
        srl     e
        jr      nc, hs_nodot
        ld      a, (hl)
        or      #0x06
        ld      (hl), a
hs_nodot:
        call    row_frame
        pop     af
        add     a, #2
        cp      #DOTS_Y0 + 2 * DOTS_ROWS
        jr      c, hs_row
        ret

;; void dots_lift(void) - строка на экране на ряд точек вверх, снизу пусто.
;; Уход пункта: семь вызовов. Буфер не трогается.
_dots_lift::
        ld      a, #DOTS_Y0
lift_row:
        push    af
        add     a, #2
        call    row_addr        ; откуда - ряд ниже
        ex      de, hl
        pop     af
        push    af
        call    row_addr        ; куда
        ex      de, hl
        ld      bc, #28
        ldir
        pop     af
        add     a, #2
        cp      #DOTS_Y0 + 2 * (DOTS_ROWS - 1)
        jr      c, lift_row
        call    row_addr        ; нижний ряд
        jp      row_blank

;; -- Спектр --
;;
;; Поля HiRam (ui.c, смещения сверяет static_assert там же) и хвост HiRam за
;; структурой: максимумы с прошлого кадра и переменные spec_tick.
SPEC_HA   .equ  _ui_hi + 608    ; spec_h[16]: высота полосы
SPEC_PA   .equ  _ui_hi + 624    ; spec_peak[16]: строка пика + 1
SPEC_DA   .equ  _ui_hi + 640    ; spec_hold[16]: сколько кадров пик стоит
SPEC_TOP  .equ  9
SPEC_ROWS .equ  23
_bus_spec_max  ==  0xBAE0       ; 16 байт, пишет bus_client
_bus_level_max ==  0xBAF0       ; 2 байта
ST_ODD    .equ  0xBAF2
HI_CLR    .equ  SPEC_HA         ; чистит dots_init, до конца страницы 0xBA
HI_CLR_N  .equ  0xBB00 - HI_CLR

;; void spec_tick(void) - кадр спектра из прерывания. Полоса i: x = 2 + 4i,
;; y = 9..31 снизу вверх. Подъём сразу, спад пиксель за кадр; пик стоит 25
;; кадров и падает пиксель за 2 кадра. Значение полосы - максимум, пришедший
;; с прошлого кадра (bus_spec_max): кадр с платы, затёртый следующим до
;; отрисовки, не теряется. После него в максимум ложится последнее
;; пришедшее (bus_spectrum) - спектр без перемен плата не шлёт; копия одним
;; LDIR после полос: хук идёт при запрещённых прерываниях, bus_client между
;; полосами ничего не пишет.
;;
;; Всё состояние полос - в странице 0xBA: spec_h 0xBA20 + i, spec_peak
;; 0xBA30 + i, spec_hold 0xBA40 + i (смещения держит static_assert в ui.c),
;; максимумы 0xBAE0 + i. HL - на полосу, переход между массивами - сменой
;; бит L; BC - на максимум. E - высота, D - пик. Строки рисуются снизу
;; вверх: адрес нижней - из таблицы spec_rows, дальше шаг на строку выше без
;; пересчёта.
_spec_tick::
        ld      hl, #ST_ODD
        ld      a, (hl)
        xor     #1
        ld      (hl), a
        ; Тишина: максимумы и пики нулевые - полос на экране нет и не будет
        ; (пик не ниже высоты). Так на загрузке трека: 16 полос впустую - около
        ; 2500 тактов. Удержание пика не трогается: в тишине оно и так 25.
        ld      hl, #_bus_spec_max
        xor     a
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        jr      nz, st_go
        ld      l, #<SPEC_PA
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        inc     l
        or      (hl)
        ret     z
st_go:
        ld      hl, #SPEC_HA
        ld      bc, #_bus_spec_max
st_band:
        ld      a, (bc)         ; v = максимум * 23 / 256
        ld      e, a
        ld      d, #SCALE23 >> 8
        ld      a, (de)
        ld      e, a
        ; высота: прошлая минус пиксель, но не ниже v
        ld      a, (hl)
        or      a
        jr      z, st_h0
        dec     a
st_h0:
        cp      e
        jr      nc, st_h1
        ld      a, e
st_h1:
        ld      e, a
        ; пик: не ниже высоты; выше - стоит, потом падает через кадр
        set     4, l            ; spec_peak
        ld      d, (hl)
        cp      d
        ld      a, l
        jr      c, st_below
        ld      d, e
        xor     #0x70           ; 0x3i -> 0x4i: spec_hold
        ld      l, a
        ld      (hl), #25
        jr      st_peak
st_below:
        xor     #0x70
        ld      l, a
        ld      a, (hl)
        or      a
        jr      z, st_fall
        dec     (hl)
        jr      st_peak
st_fall:
        ld      a, (ST_ODD)
        or      a
        jr      z, st_peak
        dec     d
st_peak:
        ld      a, l
        xor     #0x60           ; 0x4i -> 0x2i: spec_h
        ld      l, a
        ld      a, (hl)
        cp      e
        jr      nz, st_draw
        set     4, l
        ld      a, (hl)
        res     4, l
        cp      d
        jr      z, st_next
st_draw:
        ; Меняются только строки между прошлой и новой высотой и две строки
        ; пика, прошлого и нового; остальная полоса прежняя.
        push    bc
        push    hl
        ld      a, l
        and     #0x0F
        srl     a               ; знакоместо i / 2; CY - нечётная полоса
        ld      (st_col + 1), a
        ld      c, #0x30
        jr      nc, st_d1
        ld      c, #0x03
st_d1:
        set     4, l
        ld      a, (hl)
        ld      (st_od + 1), a  ; прошлый пик
        res     4, l
        ld      a, (hl)         ; прошлая высота
        cp      e
        jr      z, st_pk
        jr      c, st_d2
        ld      b, e            ; спад: строки [новая, прошлая)
        jr      st_d3
st_d2:
        ld      b, a            ; подъём: строки [прошлая, новая)
        ld      a, e
st_d3:
        ld      (st_hi + 1), a
        call    st_addr
st_row:
        ; точка есть: строка ниже высоты или строка пика
        ld      a, b
        cp      e
        jr      c, st_set
        inc     a
        cp      d
        jr      z, st_set
        ld      a, c
        cpl
        and     (hl)
        ld      (hl), a
        jr      st_up
st_set:
        ld      a, c
        or      (hl)
        ld      (hl), a
st_up:
        inc     b
        ld      a, b
st_hi:
        cp      #0              ; верх отрезка, вписан выше
        jr      z, st_pk
        ld      a, h
        dec     h               ; строка экрана выше
        and     #7
        jr      nz, st_row
        ld      a, l            ; через границу знакоместа
        sub     #32
        ld      l, a
        jr      c, st_row       ; и трети экрана
        ld      a, h
        add     a, #8
        ld      h, a
        jr      st_row
st_pk:
        ; Пик не ниже высоты. Прошлая строка пика выше новой высоты - гаснет,
        ; ниже или на ней - уже точка полосы. Новая строка пика выше полосы -
        ; горит; на верху полосы (подъём, D = E) её нарисовал отрезок.
st_od:
        ld      a, #0           ; прошлый пик, вписан выше
        cp      d
        jr      z, st_store
        cp      e
        jr      z, st_pk2
        jr      c, st_pk2
        dec     a
        ld      b, a
        call    st_addr
        ld      a, c
        cpl
        and     (hl)
        ld      (hl), a
st_pk2:
        ld      a, d
        cp      e
        jr      z, st_store
        jr      c, st_store
        dec     a
        ld      b, a
        call    st_addr
        ld      a, c
        or      (hl)
        ld      (hl), a
st_store:
        pop     hl
        pop     bc
        ld      (hl), e
        set     4, l
        ld      (hl), d
        res     4, l
st_next:
        inc     c
        inc     l
        bit     4, l            ; 0x30 - полос больше нет
        jp      z, st_band
        ld      hl, #_bus_spectrum
        ld      de, #_bus_spec_max
        ld      bc, #16
        ldir
        ret

;; HL = адрес строки полосы B на экране; знакоместо полосы вписано в OR.
st_addr:
        ld      a, b
        add     a, a
        add     a, #<spec_rows
        ld      l, a
        ld      a, #>spec_rows
        adc     a, #0
        ld      h, a
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
st_col:
        or      #0
        ld      l, a
        ret

;; Адрес строки экрана полосы b (y = 31 - b), знакоместо 0.
spec_rows:
        .dw     0x4760          ; строка 0, y = 31
        .dw     0x4660          ; строка 1, y = 30
        .dw     0x4560          ; строка 2, y = 29
        .dw     0x4460          ; строка 3, y = 28
        .dw     0x4360          ; строка 4, y = 27
        .dw     0x4260          ; строка 5, y = 26
        .dw     0x4160          ; строка 6, y = 25
        .dw     0x4060          ; строка 7, y = 24
        .dw     0x4740          ; строка 8, y = 23
        .dw     0x4640          ; строка 9, y = 22
        .dw     0x4540          ; строка 10, y = 21
        .dw     0x4440          ; строка 11, y = 20
        .dw     0x4340          ; строка 12, y = 19
        .dw     0x4240          ; строка 13, y = 18
        .dw     0x4140          ; строка 14, y = 17
        .dw     0x4040          ; строка 15, y = 16
        .dw     0x4720          ; строка 16, y = 15
        .dw     0x4620          ; строка 17, y = 14
        .dw     0x4520          ; строка 18, y = 13
        .dw     0x4420          ; строка 19, y = 12
        .dw     0x4320          ; строка 20, y = 11
        .dw     0x4220          ; строка 21, y = 10
        .dw     0x4120          ; строка 22, y = 9

;; -- VU --
;;
;; Две полосы в рамке, строки y = 19, 20: левый канал от x = VU_L влево,
;; правый от x = VU_R вправо, по 23 пикселя. Подъём сразу, спад пиксель за
;; кадр. Значение - максимум, пришедший с прошлого кадра (bus_level_max),
;; после него там последнее пришедшее, как у спектра.
VU_HA     .equ  _ui_hi + 656    ; vu[2]: длина полосы
VU_L      .equ  102
VU_R      .equ  105
VU_ROW_H  .equ  0x43            ; строка y = 19: H, L - знакоместо 0
VU_ROW_L  .equ  0x40

;; void vu_tick(void) - кадр VU из прерывания.
_vu_tick::
        ld      hl, #_bus_level
        ld      de, #_bus_level_max
        ld      ix, #VU_HA
        call    vu_len
        jr      z, vu_right
        call    vu_order
        push    af
        ld      a, d
        sub     e
        ld      c, a
        ld      a, #VU_L + 1
        sub     d
        ld      b, a
        pop     af
        call    vu_span
vu_right:
        ld      hl, #_bus_level + 1
        ld      de, #_bus_level_max + 1
        ld      ix, #VU_HA + 1
        call    vu_len
        ret     z
        call    vu_order
        push    af
        ld      a, d
        sub     e
        ld      c, a
        ld      a, #VU_R
        add     a, e
        ld      b, a
        pop     af
        jp      vu_span

;; Новая длина полосы канала: HL - bus_level[ch], DE - bus_level_max[ch],
;; IX - vu[ch]. Выход: B - прошлая длина, C - новая, Z - без перемен.
vu_len:
        ld      a, (de)
        ld      c, a
        ld      a, (hl)
        ld      (de), a
        ld      hl, #SCALE23
        ld      b, #0
        add     hl, bc
        ld      c, (hl)         ; v
        ld      b, 0(ix)        ; прошлая
        ld      a, b
        or      a
        jr      z, vl_0
        dec     a
vl_0:
        cp      c
        jr      nc, vl_1
        ld      a, c
vl_1:
        ld      c, a
        cp      b
        ret     z
        ld      0(ix), c
        ret

;; D = большая из длин B и C, E = меньшая; A = 0xFF - полоса растёт, 0 -
;; спадает.
vu_order:
        ld      a, c
        cp      b
        jr      c, vo_down
        ld      d, c
        ld      e, b
        ld      a, #0xFF
        ret
vo_down:
        ld      d, b
        ld      e, c
        xor     a
        ret

;; C пикселей с x = B на строках 19 и 20: A = 0xFF - ставить, 0 - снимать.
;; Адрес и маска - один раз, дальше маска шагает вправо RRC: около 110
;; тактов на пиксель вместо 220 с пересчётом. Портит B.
vu_span:
        ld      e, a
        ld      a, b
        rrca
        rrca
        rrca
        and     #0x1F
        or      #VU_ROW_L
        ld      l, a
        ld      h, #VU_ROW_H
        ld      a, b
        and     #7
        ld      d, #0x80
        jr      z, vs_px
vs_shift:
        srl     d
        dec     a
        jr      nz, vs_shift
vs_px:
        ld      a, e
        or      a
        ld      a, d
        jr      z, vs_clear
        or      (hl)
        ld      (hl), a
        inc     h
        ld      a, d
        or      (hl)
        ld      (hl), a
        jr      vs_next
vs_clear:
        cpl
        ld      b, a
        and     (hl)
        ld      (hl), a
        inc     h
        ld      a, b
        and     (hl)
        ld      (hl), a
vs_next:
        dec     h
        rrc     d               ; 0x01 -> 0x80 с переносом: следующее знакоместо
        jr      nc, vs_same
        inc     l
vs_same:
        dec     c
        jr      nz, vs_px
        ret

        .area   _DATA
draw_src: .ds   1               ; строка буфера для строки экрана

        .area   _CODE

tab_a:  .db 0x00,0x06,0x30,0x36,0x80,0x86,0xB0,0xB6
tab_b:  .db 0x00,0x03,0x18,0x1B,0xC0,0xC3,0xD8,0xDB
tab_c:  .db 0x00,0x01,0x0C,0x0D,0x60,0x61,0x6C,0x6D
