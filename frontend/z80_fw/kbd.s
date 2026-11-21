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
        .globl  _fw_kbd_take
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

kbd_caps:       .ds     1       ; CAPS SHIFT нажат прямо сейчас
kbd_sym:        .ds     1       ; SYMBOL SHIFT нажат прямо сейчас

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
        ;; Полуряд 0 - ещё раз, последним. CAPS SHIFT с клавишей (стрелка,
        ;; Esc) клавиатура USB ставит одним изменением; изменение между
        ;; чтением полуряда 0 и полуряда клавиши давало бы клавишу без CAPS.
        ld      b, #0xFE
        in      a, (c)
        ld      hl, #_fw_kbd_matrix
        and     (hl)
        ld      (hl), a

        ;; Модификаторы: CAPS SHIFT - полуряд 0 бит 0, SYMBOL SHIFT -
        ;; полуряд 7 бит 1.
        ld      a, (_fw_kbd_matrix+0)
        cpl
        and     #0x01
        ld      (kbd_caps), a
        ld      a, (_fw_kbd_matrix+7)
        cpl
        and     #0x02
        ld      (kbd_sym), a

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
        ;; Повтор не копится: пока приложение не забрало прошлый код,
        ;; нового не кладём. Иначе рисование списка отстаёт от шага в
        ;; шестьдесят миллисекунд, кольцо набивается, и после отпускания
        ;; курсор доезжает накопленное. Настоящее нажатие в очередь встаёт
        ;; всегда - терять его нельзя, а повтор потерять не жалко.
        ld      a, (kbd_head)
        ld      hl, #kbd_tail
        cp      (hl)
        ret     nz
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
        ;; С SYMBOL SHIFT - знак из своей таблицы, CAPS SHIFT тогда не
        ;; учитывается.
        ld      de, #kbd_table
        ld      a, (kbd_sym)
        or      a
        jr      z, kbd_emit_plain
        ld      de, #kbd_sym_table
kbd_emit_plain:
        add     hl, de
        ld      a, (hl)
        or      a
        jr      z, kbd_emit_shift       ; шифты и клавиши без знака не в счёт
        ld      b, a
        ld      a, (kbd_sym)
        or      a
        ld      a, b
        call    z, kbd_apply_caps
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

;; Взвести автоповтор на клавишу с кодом A; A сохраняется, C портится.
;;
;; Повторяются только стрелки: удержанный Enter запускал бы трек снова и
;; снова, а пробел - листал треки. Любая другая клавиша повтор снимает.
kbd_arm_repeat:
        ld      c, a
        sub     #8
        cp      #4              ; 8..11 - влево, вправо, вниз, вверх
        ld      a, #0
        jr      nc, kbd_arm_code
        ld      a, #KBD_REP_DELAY
        ld      (kbd_rep_timer), a
        ld      a, c
kbd_arm_code:
        ld      (kbd_rep_code), a
        ld      a, c
        ret

;; Учесть CAPS SHIFT: стрелки, Esc, забой и заглавные буквы.
;;
;; Стрелок на клавиатуре нет физически: все программы читают их как
;; CAPS SHIFT с цифрами. Портит B, C, HL.
kbd_apply_caps:
        ld      c, a
        ld      a, (kbd_caps)
        or      a
        ld      a, c
        ret     z

        ld      hl, #kbd_caps_table
        ld      b, #6
kbd_caps_next:
        cp      (hl)
        inc     hl
        jr      z, kbd_caps_hit
        inc     hl
        djnz    kbd_caps_next

        ;; Буквы с CAPS SHIFT - заглавные.
        cp      #0x61
        ret     c
        cp      #0x7B
        ret     nc
        and     #0xDF
        ret
kbd_caps_hit:
        ld      a, (hl)
        ret

;; Пары "клавиша с CAPS SHIFT - код".
kbd_caps_table:
        .db     0x35, 8         ; "5" -> влево
        .db     0x36, 10        ; "6" -> вниз
        .db     0x37, 11        ; "7" -> вверх
        .db     0x38, 9         ; "8" -> вправо
        .db     0x30, 12        ; "0" -> забой
        .db     0x20, 27        ; пробел -> Esc

;; Положить код A в кольцо. В полном кольце теряется самое старое
;; нажатие, а не новое.
kbd_push:
        ld      c, a
        ld      a, (kbd_head)
        ld      e, a
        inc     a
        and     #KBD_RING-1
        ld      hl, #kbd_tail
        cp      (hl)
        jr      nz, kbd_push_ok
        ;; Догнали хвост - подвинем его на шаг дальше головы.
        ld      b, a
        inc     a
        and     #KBD_RING-1
        ld      (hl), a
        ld      a, b
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
        call    _fw_kbd_take
        ei
        ret

;; То же без di/ei: для того, кто опрашивает клавиатуру сам при запрещённых
;; прерываниях.
_fw_kbd_take::
        ld      a, (kbd_tail)
        ld      hl, #kbd_head
        cp      (hl)
        jr      nz, kbd_get_have
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

;; Знаки с SYMBOL SHIFT, как на клавишах Спектрума. Ноль - у клавиши знака
;; нет (там слово Бейсика). На месте фунта - обратный апостроф: в шрифте
;; Спектрума это и есть фунт.
kbd_sym_table:
        .db     0,    0x3A, 0x60, 0x3F, 0x2F      ; CS  : фунт ? /
        .db     0x7E, 0x7C, 0x5C, 0x7B, 0x7D      ; ~ | обратная косая { }
        .db     0,    0,    0,    0x3C, 0x3E      ; q w e < >
        .db     0x21, 0x40, 0x23, 0x24, 0x25      ; ! @ # $ %
        .db     0x5F, 0x29, 0x28, 0x27, 0x26      ; _ ) ( ' &
        .db     0x22, 0x3B, 0,    0x5D, 0x5B      ; " ; i ] [
        .db     0x0D, 0x3D, 0x2B, 0x2D, 0x5E      ; ENTER = + - ^
        .db     0x20, 0,    0x2E, 0x2C, 0x2A      ; SPACE SS . , *
