;; kbd.s - клавиатура Спектрума.
;;
;; Матрица 8x5: старший байт адреса порта 0xFE выбирает полуряд нулём в
;; соответствующем бите, младшие пять бит ответа - клавиши, и сброшенный
;; бит значит "нажата".
;;
;; Опрашивает обработчик прерывания, пятьдесят раз в секунду; приложение
;; берёт готовые коды из кольца. Дребезг гасится здесь же.
;;
;; Коды те же, что у ПЗУ Спектрума: 8 влево, 9 вправо, 10 вниз, 11 вверх.

        .module kbd
        .globl  _fw_kbd_scan
        .globl  _fw_kbd_get
        .globl  _fw_kbd_wait
        .globl  _fw_kbd_flush
        .globl  _fw_kbd_matrix

KBD_RING .equ   8               ; сколько нажатий переживёт незанятое приложение

;; Автоповтор удержанной стрелки: кадров до первой добавки и между
;; следующими. Опрос идёт пятьдесят раз в секунду, то есть полсекунды
;; ожидания и шестьдесят миллисекунд шага.
KBD_REP_DELAY .equ 25
KBD_REP_RATE  .equ 3

        .area   _DATA

_fw_kbd_matrix::.ds     8       ; как есть с порта: 0 в бите = нажата
kbd_prev:       .ds     8       ; то же в прошлый раз - ловим нажатие, не удержание
kbd_ring:       .ds     KBD_RING
kbd_head:       .ds     1       ; куда класть
kbd_tail:       .ds     1       ; откуда брать

;; Клавиша, которую держат: код, её место в матрице и счётчик кадров.
;; Нулевой код - повторять нечего.
kbd_rep_code:   .ds     1
kbd_rep_row:    .ds     1
kbd_rep_bit:    .ds     1
kbd_rep_timer:  .ds     1

        .area   _CODE

;; Опросить клавиатуру и сложить новые нажатия в кольцо.
;;
;; Зовётся из прерывания, поэтому не трогает ничего, кроме своих данных, и
;; не ждёт никаких событий.
_fw_kbd_scan::
        ;; Восемь полурядов. Выбор - нулём в бите старшего байта адреса.
        ld      hl, #_fw_kbd_matrix
        ld      b, #0xFE        ; полуряд 0: старший байт 0xFE
        ld      c, #0xFE        ; младший байт адреса порта
        ld      d, #8
kbd_row:
        in      a, (c)
        and     #0x1F
        ld      (hl), a
        inc     hl
        ;; Сдвиг влево с втягиванием единицы: 0xFE -> 0xFD -> 0xFB ...
        ld      a, b
        rlca
        or      #0x01
        ld      b, a
        dec     d
        jr      nz, kbd_row

        ;; Модификаторы: CAPS SHIFT - полуряд 0 бит 0, SYMBOL SHIFT -
        ;; полуряд 7 бит 1.
        ld      a, (_fw_kbd_matrix+0)
        cpl
        and     #0x01
        ld      (kbd_caps), a

        ;; Перебрать все сорок позиций и найти те, которых в прошлый раз
        ;; не было.
        ld      hl, #_fw_kbd_matrix
        ld      de, #kbd_prev
        ld      b, #0           ; номер полуряда
kbd_scan_row:
        ld      a, (de)
        ld      c, a            ; было
        ld      a, (hl)         ; стало
        ld      (de), a         ; запомнить на следующий раз
        ;; Нажатыми стали те биты, что были единицей, а стали нулём.
        cpl
        and     c
        jr      z, kbd_scan_next
        ld      c, a            ; C - свежие нажатия этого полуряда
        push    hl
        push    de
        call    kbd_emit_row
        pop     de
        pop     hl
kbd_scan_next:
        inc     hl
        inc     de
        inc     b
        ld      a, b
        cp      #8
        jr      nz, kbd_scan_row

        ;; Удержание: свежих нажатий у него нет, поэтому код добавляется в
        ;; кольцо здесь, пока клавиша не отпущена.
        ld      a, (kbd_rep_code)
        or      a
        ret     z
        ;; Её полуряд из только что снятой матрицы.
        ld      a, (kbd_rep_row)
        ld      l, a
        ld      h, #0
        ld      de, #_fw_kbd_matrix
        add     hl, de
        ld      a, (kbd_rep_bit)
        ld      b, a
        ld      a, (hl)
        inc     b
kbd_rep_test:
        dec     b
        jr      z, kbd_rep_ready
        rrca
        jr      kbd_rep_test
kbd_rep_ready:
        rrca                    ; нужный бит ушёл в перенос
        jr      c, kbd_rep_off  ; единица - клавишу отпустили
        ld      hl, #kbd_rep_timer
        dec     (hl)
        ret     nz
        ld      (hl), #KBD_REP_RATE
        ld      a, (kbd_rep_code)
        jp      kbd_push
kbd_rep_off:
        xor     a
        ld      (kbd_rep_code), a
        ret

;; Разобрать свежие нажатия одного полуряда: B - номер полуряда, C - маска.
kbd_emit_row:
        ld      d, #0           ; номер бита
kbd_emit_bit:
        srl     c
        jr      nc, kbd_emit_skip

        ;; Код клавиши лежит в таблице по смещению полуряд*5 + бит.
        push    bc
        push    de
        ;; Место в матрице - до таблицы: её адрес занимает D.
        ld      a, b
        ld      (kbd_rep_row), a
        ld      a, d
        ld      (kbd_rep_bit), a
        ld      a, b
        add     a, a
        add     a, a
        add     a, b            ; полуряд*5
        add     a, d
        ld      l, a
        ld      h, #0
        ld      de, #kbd_table
        add     hl, de
        ld      a, (hl)
        or      a
        jr      z, kbd_emit_shift       ; сами шифты клавишами не считаем
        call    kbd_apply_caps
        call    kbd_arm_repeat
        call    kbd_push
kbd_emit_done:
        pop     de
        pop     bc

kbd_emit_skip:
        inc     d
        ld      a, d
        cp      #5
        jr      nz, kbd_emit_bit
        ret

;; Шифт нажали свежим: место в матрице под ним уже переписано, поэтому
;; повтор снимается - следующая клавиша взведёт его заново.
kbd_emit_shift:
        xor     a
        ld      (kbd_rep_code), a
        jr      kbd_emit_done

;; Взвести автоповтор на клавишу с кодом A; A сохраняется.
;;
;; Повторяются только стрелки: удержанный Enter запускал бы трек снова и
;; снова, а пробел - листал треки. Любая другая клавиша повтор снимает.
kbd_arm_repeat:
        push    af
        xor     a
        ld      (kbd_rep_code), a
        pop     af
        cp      #8              ; 8..11 - влево, вправо, вниз, вверх
        ret     c
        push    af
        cp      #12
        jr      nc, kbd_arm_done
        pop     af
        ld      (kbd_rep_code), a
        push    af
        ld      a, #KBD_REP_DELAY
        ld      (kbd_rep_timer), a
kbd_arm_done:
        pop     af
        ret

;; Учесть CAPS SHIFT: стрелки, Esc, забой и заглавные буквы.
;;
;; Стрелок на клавиатуре нет физически: все программы читают их как
;; CAPS SHIFT с цифрами.
kbd_apply_caps:
        ld      c, a
        ld      a, (kbd_caps)
        or      a
        ld      a, c
        ret     z

        cp      #0x35           ; "5" -> влево
        jr      nz, kbd_c6
        ld      a, #8
        ret
kbd_c6: cp      #0x36           ; "6" -> вниз
        jr      nz, kbd_c7
        ld      a, #10
        ret
kbd_c7: cp      #0x37           ; "7" -> вверх
        jr      nz, kbd_c8
        ld      a, #11
        ret
kbd_c8: cp      #0x38           ; "8" -> вправо
        jr      nz, kbd_c0
        ld      a, #9
        ret
kbd_c0: cp      #0x30           ; "0" -> забой
        jr      nz, kbd_csp
        ld      a, #12
        ret
kbd_csp:
        cp      #0x20           ; пробел -> Esc
        jr      nz, kbd_cup
        ld      a, #27
        ret
kbd_cup:
        ;; Буквы с CAPS SHIFT - заглавные.
        cp      #0x61
        ret     c
        cp      #0x7B
        ret     nc
        and     #0xDF
        ret

;; Положить код A в кольцо. Полное кольцо роняет самое старое нажатие:
;; если приложение задумалось, свежая клавиша нужнее давней.
kbd_push:
        ld      c, a
        ld      a, (kbd_head)
        ld      e, a
        inc     a
        and     #KBD_RING-1
        ld      hl, #kbd_tail
        cp      (hl)
        jr      nz, kbd_push_ok
        ;; Догнали хвост - подвинем его.
        ld      a, (kbd_tail)
        inc     a
        and     #KBD_RING-1
        ld      (kbd_tail), a
        ld      a, (kbd_head)
        inc     a
        and     #KBD_RING-1
kbd_push_ok:
        ld      (kbd_head), a
        ld      d, #0
        ld      hl, #kbd_ring
        add     hl, de
        ld      (hl), c
        ret

;; Взять код из кольца. A = 0, если нажатий не было.
;;
;; Прерывания на время чтения гасим: обработчик двигает те же указатели, и
;; попасть между чтением хвоста и его записью - значит потерять клавишу.
_fw_kbd_get::
        di
        ld      a, (kbd_tail)
        ld      hl, #kbd_head
        cp      (hl)
        jr      nz, kbd_get_have
        ei
        xor     a
        ret
kbd_get_have:
        ld      e, a
        ld      d, #0
        ld      hl, #kbd_ring
        add     hl, de
        ld      c, (hl)
        ld      a, e
        inc     a
        and     #KBD_RING-1
        ld      (kbd_tail), a
        ei
        ld      a, c
        ret

;; Ждать нажатия. Возвращает код в A. Через HALT: пока клавиш нет, машина
;; спит до следующего кадра, а не отнимает такты у шины.
_fw_kbd_wait::
        halt
        call    _fw_kbd_get
        or      a
        jr      z, _fw_kbd_wait
        ret

;; Забыть накопленное: после долгой операции нажатия уже не к месту.
_fw_kbd_flush::
        di
        xor     a
        ld      (kbd_head), a
        ld      (kbd_tail), a
        ei
        ret

        .area   _DATA
kbd_caps:       .ds     1       ; CAPS SHIFT нажат прямо сейчас

        .area   _CODE

;; Сорок клавиш в порядке опроса: полуряд за полурядом, бит за битом.
;; Ноль - это сами шифты, они клавишами не считаются.
kbd_table:
        .db     0,    0x7A, 0x78, 0x63, 0x76      ; CS  z x c v
        .db     0x61, 0x73, 0x64, 0x66, 0x67      ; a s d f g
        .db     0x71, 0x77, 0x65, 0x72, 0x74      ; q w e r t
        .db     0x31, 0x32, 0x33, 0x34, 0x35      ; 1 2 3 4 5
        .db     0x30, 0x39, 0x38, 0x37, 0x36      ; 0 9 8 7 6
        .db     0x70, 0x6F, 0x69, 0x75, 0x79      ; p o i u y
        .db     0x0D, 0x6C, 0x6B, 0x6A, 0x68      ; ENTER l k j h
        .db     0x20, 0,    0x6D, 0x6E, 0x62      ; SPACE SS m n b
