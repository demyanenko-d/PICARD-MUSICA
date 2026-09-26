;; test_ui.s - прерывания, клавиатура и экран.
;;
;; Проверяем не "не упало", а три вещи, каждая из которых ломается молча:
;;   - обработчик действительно наш и вызывается раз в кадр (счётчик);
;;   - символ лёг на экран по правильному адресу и правильными точками -
;;     раскладка экрана у Спектрума такая, что ошибиться в ней легко, а
;;     заметить трудно;
;;   - нажатия доехали до приложения в нужном порядке и с нужными кодами,
;;     включая стрелки, которых на этой клавиатуре физически нет.
;;
;; Результат по 0xA000:
;;   +0   0 успех, 1 обработчик не сработал, 2 клавиш пришло не столько
;;   +1   кадров насчитано (2 байта)
;;   +3   сколько клавиш забрали
;;   +4   их коды подряд
;;
;; Эмулятор нажимает клавиши сам, между отсчётами кадров. Метки: 1 - экран
;; нарисован, ждём клавиш; 2 - клавиши собраны.

        .module test_ui
        .globl  _fw_irq_init
        .globl  _fw_irq_stop
        .globl  _fw_ticks
        .globl  _fw_wait_frames
        .globl  _fw_kbd_get
        .globl  _fw_scr_cls
        .globl  _fw_scr_at_hl
        .globl  _fw_scr_puts
        .globl  _fw_scr_putc
        .globl  _fw_scr_attr
        .globl  start

RESULT  .equ    0xA000
DBGMARK .equ    0x33
KEYS    .equ    6               ; сколько нажатий ждём

        .area   _DATA
ui_got:         .ds     1
ui_ptr:         .ds     2

        .area   _CODE

start::
        ld      sp, #0x9F00
        xor     a
        ld      (ui_got), a

        call    _fw_irq_init

        ;; Экран: чёрный фон, белые буквы.
        ld      a, #0x07
        call    _fw_scr_cls

        ;; Латиница и кириллица одной строкой - шрифт обязан уметь и то и
        ;; другое, иначе имена файлов показывать нечем.
        ld      h, #2
        ld      l, #1
        call    _fw_scr_at_hl
        ld      hl, #ui_text
        call    _fw_scr_puts

        ;; Отдельный символ в дальний угол: если раскладка адресов неверна,
        ;; на краю это видно сразу, а в первой строке может и сойтись.
        ld      h, #31
        ld      l, #23
        call    _fw_scr_at_hl
        ld      a, #0xDB        ; сплошной блок
        call    _fw_scr_putc

        ld      a, #1
        out     (DBGMARK), a

        ;; Собрать нажатия. Ждём кадрами, а не пустым циклом: клавиатуру
        ;; опрашивает прерывание, и без кадров ждать нечего.
        ld      hl, #RESULT+4
        ld      (ui_ptr), hl
ui_wait:
        ld      a, #2
        call    _fw_wait_frames
        call    _fw_kbd_get
        or      a
        jr      z, ui_check
        ld      hl, (ui_ptr)
        ld      (hl), a
        inc     hl
        ld      (ui_ptr), hl
        ld      a, (ui_got)
        inc     a
        ld      (ui_got), a
ui_check:
        ld      a, (ui_got)
        cp      #KEYS
        jr      c, ui_wait

        ld      a, #2
        out     (DBGMARK), a

        ;; Сколько кадров насчитал обработчик.
        ld      hl, (_fw_ticks)
        ld      (RESULT+1), hl
        ld      a, (ui_got)
        ld      (RESULT+3), a

        ld      a, h
        or      l
        jr      nz, ui_ticks_ok
        ld      a, #1                   ; обработчик не сработал ни разу
        jr      ui_done
ui_ticks_ok:
        xor     a
ui_done:
        ld      (RESULT+0), a
        call    _fw_irq_stop
        ld      a, #0xFF
        out     (DBGMARK), a            ; эмулятору: программа закончила
        halt

ui_text:
        .db     0x50, 0x72, 0x69, 0x76, 0x65, 0x74, 0x20     ; "Privet "
        .db     0x8F, 0xE0, 0xA8, 0xA2, 0xA5, 0xE2           ; "Привет" в CP866
        .db     0x00
