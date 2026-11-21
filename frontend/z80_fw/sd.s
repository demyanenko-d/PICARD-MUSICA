;; sd.s - карта SD через Z-Controller или DivMMC, только чтение.
;;
;; Устройства (_sd_use):
;;
;;   0  Z-Controller  0x57 данные, 0x77 выбор: бит 1 = 0 выбирает карту
;;   1  DivMMC, карта 1  0xEB данные, 0xE7 выбор: бит 0 = 0
;;   2  DivMMC, карта 2  0xEB данные, 0xE7 выбор: бит 1 = 0
;;
;; Порт данных у обоих конвейерный: запись отдаёт байт MOSI, чтение
;; возвращает MISO прошлого обмена и запускает такт вхолостую.
;;
;; Соглашение: всё через регистры, стек не трогаем без нужды. Возврат
;; ошибки - флаг переноса (CY=1 плохо, CY=0 хорошо), так дешевле всего
;; проверять у вызывающего. Порт данных - в C: у каждой точки входа своя
;; загрузка (sd_port).

        .module sd
        .globl  _sd_init
        .globl  _sd_use
        .globl  _sd_dev
        .globl  _sd_read_sector
        .globl  _sd_read_sector_to_port
        .globl  _sd_type
        .globl  _sd_read_multi_to_port
        .globl  _sd_multi_count

SD_DEVS .equ    3

;; Порт данных платы, то же число, что BUS_PORT_DAT клиента шины: чтение
;; сектора идёт в порт мимо клиента, а заголовок Си ассемблер не берёт.
BUS_DAT .equ    0x67

;; _sd_type после _sd_init: 0 - карта не поднята; CT_BLOCK - аргумент чтения
;; номер сектора; SDSC (байтовая адресация) - поднята, но читать её
;; sd_cmd_lba не умеет.
CT_BLOCK .equ   0x08
CT_BYTE  .equ   0x04

        .area   _DATA
;; Устройство, с которым работают все вызовы. Ноль после запуска -
;; Z-Controller.
_sd_dev::
        .ds     1
_sd_type::
        .ds     1
;; Порты и выбор устройства _sd_dev - копия строки sd_devs.
sd_dat:
        .ds     1
sd_ctl:
        .ds     1
sd_sel:
        .ds     1
sd_desel:
        .ds     1
;; _sd_type каждого устройства, пока работают с другим.
sd_types:
        .ds     SD_DEVS
sd_arg:                         ; аргумент команды, старший байт первым
        .ds     4

;; Сколько секторов забрать одной командой CMD18.
_sd_multi_count::
        .ds     1
sd_left:                        ; сколько осталось в пачке CMD18
        .ds     1

        .area   _CODE

;; Порт данных, порт выбора, выбрать, снять выбор.
sd_devs:
        .db     0x57, 0x77, 0x01, 0x03
        .db     0xEB, 0xE7, 0xFE, 0xFF
        .db     0xEB, 0xE7, 0xFD, 0xFF

;; -- Устройство --
;;
;; Вход: A = устройство (0..2), чужой номер - Z-Controller. Карта
;; устройства, поднятая раньше, остаётся поднятой: _sd_type у каждого свой.
;; Портит HL, DE, BC.
_sd_use::
        cp      #SD_DEVS
        jr      c, sd_use_dev
        xor     a
sd_use_dev:
        ld      c, a
        ld      b, #0
        ld      a, (_sd_dev)
        cp      #SD_DEVS        ; память не обнулена - прежнего нет
        jr      nc, sd_use_load
        ld      hl, #sd_types
        ld      e, a
        ld      d, b
        add     hl, de
        ld      a, (_sd_type)
        ld      (hl), a         ; запомнить тип прежнего
sd_use_load:
        ld      a, c
        ld      (_sd_dev), a
        ld      hl, #sd_types
        add     hl, bc
        ld      a, (hl)
        ld      (_sd_type), a
        ld      hl, #sd_devs
        add     hl, bc
        add     hl, bc
        add     hl, bc
        add     hl, bc
        ld      de, #sd_dat
        ld      bc, #4
        ldir
        ret

;; C = порт данных.
sd_port:
        ld      a, (sd_dat)
        ld      c, a
        ret

;; -- Примитивы --

;; Байт A в порт выбора. C (порт данных) сохраняется.
sd_ctl_out:
        push    bc
        ld      b, a
        ld      a, (sd_ctl)
        ld      c, a
        out     (c), b
        pop     bc
        ret

;; Отпустить шину: снять выбор и выдать байт 0xFF - карта освобождает MISO
;; только по тактам после снятия выбора. Вход: C = порт данных.
sd_release:
        ld      a, (sd_desel)
        call    sd_ctl_out
        ld      a, #0xFF
        out     (c), a
        ret

;; Холостые такты: B штук. Вход: C = порт данных.
sd_skip:
        ld      a, #0xFF
sd_skip_loop:
        out     (c), a
        djnz    sd_skip_loop
        ret

;; -- Команда --
;;
;; Вход:  E  = код команды (уже с битом 0x40), C = порт данных
;;        sd_arg = аргумент, старший байт первым
;; Выход: A  = ответ R1, CY=1 если карта не ответила вовсе
;;
;; Последовательность: снять выбор, такт, выбрать, такт, шесть байт,
;; потом до десяти попыток дождаться байта с погашенным старшим битом.
sd_cmd:
        call    sd_release
        ld      a, (sd_sel)
        call    sd_ctl_out
        ld      a, #0xFF
        out     (c), a

        out     (c), e

        ld      hl, #sd_arg
        ld      b, #4
sd_cmd_arg:
        ld      a, (hl)
        out     (c), a
        inc     hl
        djnz    sd_cmd_arg

        ;; CRC настоящий нужен только двум командам - до инициализации
        ;; карта его проверяет. Дальше ей всё равно, шлём заглушку.
        ld      a, e
        cp      #0x40           ; CMD0
        jr      z, sd_crc_cmd0
        cp      #0x48           ; CMD8
        jr      z, sd_crc_cmd8
        ld      a, #0x01
        jr      sd_crc_out
sd_crc_cmd0:
        ld      a, #0x95
        jr      sd_crc_out
sd_crc_cmd8:
        ld      a, #0x87
sd_crc_out:
        out     (c), a

        ld      b, #10
sd_cmd_wait:
        in      a, (c)
        bit     7, a
        jr      z, sd_cmd_ok    ; старший бит погас - это ответ
        djnz    sd_cmd_wait
        scf                     ; не дождались
        ret
sd_cmd_ok:
        or      a               ; CY=0
        ret

;; Заполнить sd_arg нулями.
sd_arg_zero:
        ld      hl, #sd_arg
        xor     a
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (hl), a
        ret

;; -- Инициализация --
;;
;; Карта устройства _sd_dev. Выход: CY=0 успех, _sd_type заполнен; CY=1
;; карта не поднялась.
_sd_init::
        ld      a, (_sd_dev)
        call    _sd_use         ; порты устройства: после запуска их ещё нет
        call    sd_port
        xor     a
        ld      (_sd_type), a

        ld      a, (sd_desel)   ; снять выбор
        call    sd_ctl_out
        ld      b, #250         ; 0xFF пачкой, чтобы карта проснулась
        call    sd_skip
        ld      b, #250
        call    sd_skip

        ;; CMD0 - уйти в простой. Ответ обязан быть ровно 0x01.
        call    sd_arg_zero
        ld      e, #0x40
        call    sd_cmd
        jr      c, sd_init_fail
        cp      #0x01
        jr      nz, sd_init_fail

        ;; CMD8 - узнать, SDv2 ли это, и заодно проверить эхо 0x01AA.
        call    sd_arg_zero
        ld      hl, #sd_arg+2
        ld      (hl), #0x01
        inc     hl
        ld      (hl), #0xAA
        ld      e, #0x48
        call    sd_cmd
        jr      c, sd_init_fail
        cp      #0x01
        jr      nz, sd_init_fail

        ;; Хвост R7: четыре байта, значимы последние два.
        in      a, (c)
        in      a, (c)
        in      a, (c)
        cp      #0x01
        jr      nz, sd_init_fail
        in      a, (c)
        cp      #0xAA
        jr      nz, sd_init_fail

        ;; ACMD41 с битом HCS, пока карта не выйдет из простоя. Счётчик
        ;; попыток - на стеке: E занят командой, HL - аргументом.
        ld      hl, #0x8000     ; с запасом, но не вечно
sd_init_acmd:
        push    hl
        call    sd_arg_zero
        ld      e, #0x77        ; CMD55
        call    sd_cmd
        jr      c, sd_init_fail_pop

        call    sd_arg_zero
        ld      hl, #sd_arg
        ld      (hl), #0x40     ; бит 30 = HCS: "умею блочную адресацию"
        ld      e, #0x69        ; CMD41
        call    sd_cmd
        pop     hl
        jr      c, sd_init_fail
        or      a
        jr      z, sd_init_ready
        dec     hl
        ld      a, h
        or      l
        jr      nz, sd_init_acmd
        jr      sd_init_fail

sd_init_ready:
        ;; CMD58 - прочитать OCR и узнать, блочная адресация или байтовая.
        call    sd_arg_zero
        ld      e, #0x7A
        call    sd_cmd
        jr      c, sd_init_fail
        or      a
        jr      nz, sd_init_fail

        in      a, (c)          ; старший байт OCR, бит 6 = CCS
        and     #0x40
        ld      a, #CT_BLOCK
        jr      nz, sd_init_type
        ld      a, #CT_BYTE
sd_init_type:
        ld      (_sd_type), a
        in      a, (c)          ; дочитать OCR
        in      a, (c)
        in      a, (c)

        call    sd_release
        or      a               ; CY=0
        ret

sd_init_fail_pop:
        pop     hl
sd_init_fail:
        call    sd_release
        xor     a
        ld      (_sd_type), a
        scf
        ret

;; -- Чтение сектора --
;;
;; Вход:  DEHL = номер сектора (DE старшее слово), IX = куда положить 512 байт
;; Выход: CY=0 успех
;;
;; Портит B и C: у INIR они счётчик и порт, в sd_pump512 - счётчик витков и
;; порт. Держать в них что-то своё через вызов нельзя.

;; Команда чтения E (CMD17, CMD18) с номером сектора DEHL. CY=0 - карта
;; команду приняла (R1 = 0). Без блочной адресации команда не идёт. На
;; выходе C = порт данных.
sd_cmd_lba:
        ld      a, d
        ld      (sd_arg+0), a
        ld      a, e
        ld      (sd_arg+1), a
        ld      a, h
        ld      (sd_arg+2), a
        ld      a, l
        ld      (sd_arg+3), a
        ld      e, c
        call    sd_port
        ld      a, (_sd_type)
        and     #CT_BLOCK
        scf
        ret     z
        call    sd_cmd
        ret     c
        or      a
        ret     z
        scf
        ret

;; Дождаться токена данных: пока карта думает, она шлёт 0xFF. CY=0 -
;; пришёл 0xFE, за ним 512 байт; иной байт или тишина - CY=1.
sd_wait_token:
        ld      de, #0x4000
sd_token_loop:
        in      a, (c)
        cp      #0xFE
        ret     z
        inc     a
        scf
        ret     nz
        dec     de
        ld      a, d
        or      e
        jr      nz, sd_token_loop
        scf
        ret

;; CMD17 и токен. После CY=0 карта отдаёт ровно 512 байт; при отказе шина
;; уже отпущена.
sd_read_start:
        ld      c, #0x51        ; CMD17
        call    sd_cmd_lba
        call    nc, sd_wait_token
        ret     nc
sd_read_fail:
        call    sd_release
        scf
        ret

;; Общий хвост: дочитать CRC и отпустить шину.
sd_read_finish:
        in      a, (c)          ; CRC - не считаем, карта уже проверила
        in      a, (c)
        call    sd_release
        or      a
        ret

_sd_read_sector::
        call    sd_read_start
        ret     c

        ;; 512 байт двумя INIR по 256. INIR - 21 такт на байт, кладёт по (HL)
        ;; и адресует порт через C: приёмник переезжает из IX в HL, наружу
        ;; интерфейс тот же. B попадает на A8-A15, оба интерфейса смотрят
        ;; только младший байт адреса.
        push    ix
        pop     hl
        ld      b, #0           ; 0 = 256 итераций
        inir
        ld      b, #0
        inir
        push    hl              ; вернуть IX туда, куда доехал приёмник
        pop     ix
        jp      sd_read_finish

;; -- Чтение сектора прямо в порт платы, мимо памяти -------------------
;;
;; Сектор с карты прямо в порт платы: при загрузке трека байт всё равно
;; едет на плату. Вход: DEHL = номер сектора. Выход: CY=0.
_sd_read_sector_to_port::
        call    sd_read_start
        ret     c
        call    sd_pump512
        jp      sd_read_finish

;; 512 байт с карты прямо в порт платы. Вызывается и одиночным чтением, и
;; многосекторным - один цикл на оба, чтобы они не разъехались.
;;
;; Пара стоит 23 такта: IN A,(C) 12 плюс OUT (n),A 11.

sd_pump512:
        ld      b, #8           ; 8 витков по 64 байта = 512
sd_rsp_loop:
        .rept   64
        in      a, (c)
        out     (BUS_DAT), a
        .endm
        ;; Не djnz: его дальность 128 байт, а тело витка - 256. Пара
        ;; dec+jp обходится даже дешевле - 14 тактов против 13+2 у djnz с
        ;; коротким телом, и это раз на 64 байта.
        dec     b
        jp      nz, sd_rsp_loop
        ret

;; -- Многосекторное чтение прямо в порт (CMD18) --
;;
;; Вход: DEHL = первый сектор, _sd_multi_count = сколько секторов (1..255).
;; Выход: CY=0.
;;
;; Команда одна на всю пачку: карта гонит блок за блоком, пока не получит
;; CMD12. Чтение по одному сектору стоит команды и ожидания токена на
;; каждый, это 2.1 такта на байт.
;;
;; Счётчик секторов - в sd_left: B занят счётчиком витков sd_pump512, C -
;; портом, DE - ожиданием токена.
_sd_read_multi_to_port::
        ld      c, #0x52        ; CMD18
        call    sd_cmd_lba
        jp      c, sd_read_fail
        ld      a, (_sd_multi_count)
        ld      (sd_left), a

sd_multi_block:
        ;; Токен ждём перед каждым блоком: карта вправе задуматься между
        ;; ними, и молча считать, что данные уже пошли, нельзя.
        call    sd_wait_token
        jr      c, sd_multi_stop_fail
        call    sd_pump512
        in      a, (c)          ; CRC
        in      a, (c)
        ld      hl, #sd_left
        dec     (hl)
        jr      nz, sd_multi_block

        call    sd_multi_stop
        or      a
        ret

sd_multi_stop_fail:
        call    sd_multi_stop
        scf
        ret

;; Остановить поток и дождаться, пока карта отпустит линию.
sd_multi_stop:
        call    sd_arg_zero
        ld      e, #0x4C        ; CMD12
        call    sd_cmd
        ld      de, #0x4000
sd_multi_busy:
        in      a, (c)
        cp      #0xFF
        jr      z, sd_multi_idle
        dec     de
        ld      a, d
        or      e
        jr      nz, sd_multi_busy
sd_multi_idle:
        jp      sd_release
