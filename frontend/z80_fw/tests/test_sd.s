;; test_sd.s - проверка драйвера карты из-под Z80.
;;
;; Инициализирует карту, читает три сектора и складывает их подряд по
;; 0x9000. Эмулятор сверяет содержимое с образом карты.
;;
;; Результат в 0x8F00:
;;   0x8F00  0 успех, 1 инициализация не удалась, 2 чтение не удалось
;;   0x8F01  _sd_type после инициализации
;;   0x8F02  на каком секторе споткнулись (индекс 0..2)

        .module test_sd
        .globl  _sd_init
        .globl  _sd_read_sector
        .globl  _sd_type
        .globl  start

RESULT  .equ    0x8F00
BUFFER  .equ    0x9000

        .area   _CODE

start::
        ld      sp, #0x8F00     ; стек под результатом, ниже кода

        xor     a
        ld      (RESULT+0), a
        ld      (RESULT+1), a
        ld      (RESULT+2), a

        call    _sd_init
        jr      nc, init_ok
        ld      a, #1
        ld      (RESULT+0), a
        halt
init_ok:
        ld      a, (_sd_type)
        ld      (RESULT+1), a

        ld      ix, #BUFFER
        ld      iy, #sector_list
        ld      b, #3           ; сколько секторов читаем
        ld      c, #0           ; индекс текущего - для отчёта об ошибке
read_next:
        push    bc

        ld      a, (RESULT+2)   ; запоминаем индекс ДО чтения
        ld      a, c
        ld      (RESULT+2), a

        ld      d, 0 (iy)       ; номер сектора: DEHL, старший байт первым
        ld      e, 1 (iy)
        ld      h, 2 (iy)
        ld      l, 3 (iy)
        call    _sd_read_sector
        jr      c, read_fail

        ld      de, #4
        add     iy, de
        pop     bc
        inc     c
        djnz    read_next

        xor     a               ; всё прочитано
        ld      (RESULT+0), a
        halt

read_fail:
        pop     bc
        ld      a, #2
        ld      (RESULT+0), a
        halt

        .area   _DATA
sector_list:
        .db     0x00, 0x00, 0x00, 0x00      ; сектор 0 - загрузочный
        .db     0x00, 0x00, 0x00, 0x01      ; сектор 1
        .db     0x00, 0x01, 0xB8, 0xFA      ; сектор 112890 - начало данных
