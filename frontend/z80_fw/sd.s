;; sd.s - карта SD через Z-Controller, только чтение.
;;
;; Порты (сверено с драйвером SD из TS-BIOS, pff/diskio.c):
;;
;;   0x57  запись = отдать байт MOSI, чтение = такт вхолостую и вернуть MISO
;;   0x77  запись: бит 1 = SSEL, 0 выбирает карту; чтение: бит 1 = карта на месте
;;
;; Соглашение: всё через регистры, стек не трогаем без нужды. Возврат
;; ошибки - флаг переноса (CY=1 плохо, CY=0 хорошо), так дешевле всего
;; проверять у вызывающего.

        .module sd
        .globl  _sd_init
        .globl  _sd_read_sector
        .globl  _sd_read_sector_to_port
        .globl  _sd_type
        .globl  _sd_read_multi_to_port
        .globl  _sd_multi_count

SD_DAT  .equ    0x57
SD_CTL  .equ    0x77

;; Порт данных платы. Единственное место, где номер порта повторён:
;; вообще его знает только BUS_PORT_DAT в common/bus_client.h, но чтение
;; сектора идёт прямо в порт мимо клиента, а ассемблер заголовок не
;; подключает. Меняешь там - меняй и тут.
BUS_DAT .equ    0x67

;; Биты типа карты. Важен только CT_BLOCK: у карт с блочной адресацией
;; аргумент чтения - номер сектора, у старых - байтовое смещение.
CT_BLOCK .equ   0x08

        .area   _DATA
_sd_type::
        .ds     1
sd_arg:                         ; аргумент команды, старший байт первым
        .ds     4

;; Сколько секторов забрать одной командой CMD18.
_sd_multi_count::
        .ds     1

        .area   _CODE

;; -- Примитивы --

sd_cs_on:                       ; выбрать карту (бит 1 = 0)
        ld      a, #0x01
        out     (SD_CTL), a
        ret

sd_cs_off:                      ; снять выбор (бит 1 = 1)
        ld      a, #0x03
        out     (SD_CTL), a
        ret

;; Отпустить шину: снять выбор и дать карте такт, иначе она остаётся с
;; недоговорённым байтом на линии.
sd_release:
        call    sd_cs_off
        ld      a, #0xFF
        out     (SD_DAT), a
        ret

;; Холостые такты: B штук.
sd_skip:
        ld      a, #0xFF
sd_skip_loop:
        out     (SD_DAT), a
        djnz    sd_skip_loop
        ret

;; -- Команда --
;;
;; Вход:  C  = код команды (уже с битом 0x40)
;;        sd_arg = аргумент, старший байт первым
;; Выход: A  = ответ R1, CY=1 если карта не ответила вовсе
;;
;; Последовательность в точности как у драйвера tsbios: снять выбор,
;; такт, выбрать, такт, шесть байт, потом до десяти попыток дождаться
;; байта с погашенным старшим битом.
sd_cmd:
        call    sd_cs_off
        ld      a, #0xFF
        out     (SD_DAT), a
        call    sd_cs_on
        ld      a, #0xFF
        out     (SD_DAT), a

        ld      a, c
        out     (SD_DAT), a

        ld      hl, #sd_arg
        ld      b, #4
sd_cmd_arg:
        ld      a, (hl)
        out     (SD_DAT), a
        inc     hl
        djnz    sd_cmd_arg

        ;; CRC настоящий нужен только двум командам - до инициализации
        ;; карта его проверяет. Дальше ей всё равно, шлём заглушку.
        ld      a, c
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
        out     (SD_DAT), a

        ld      b, #10
sd_cmd_wait:
        in      a, (SD_DAT)
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
;; Выход: CY=0 успех, _sd_type заполнен; CY=1 карта не поднялась.
_sd_init::
        xor     a
        ld      (_sd_type), a

        call    sd_cs_off
        ld      b, #250         ; 0xFF пачкой, чтобы карта проснулась
        call    sd_skip
        ld      b, #250
        call    sd_skip

        ;; CMD0 - уйти в простой. Ответ обязан быть ровно 0x01.
        call    sd_arg_zero
        ld      c, #0x40
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
        ld      c, #0x48
        call    sd_cmd
        jr      c, sd_init_fail
        cp      #0x01
        jr      nz, sd_init_fail

        ;; Хвост R7: четыре байта, значимы последние два.
        in      a, (SD_DAT)
        in      a, (SD_DAT)
        in      a, (SD_DAT)
        cp      #0x01
        jr      nz, sd_init_fail
        in      a, (SD_DAT)
        cp      #0xAA
        jr      nz, sd_init_fail

        ;; ACMD41 с битом HCS, пока карта не выйдет из простоя.
        ld      de, #0x8000     ; счётчик попыток: с запасом, но не вечно
sd_init_acmd:
        call    sd_arg_zero
        ld      c, #0x77        ; CMD55
        call    sd_cmd
        jr      c, sd_init_fail

        call    sd_arg_zero
        ld      hl, #sd_arg
        ld      (hl), #0x40     ; бит 30 = HCS: "умею блочную адресацию"
        ld      c, #0x69        ; CMD41
        call    sd_cmd
        jr      c, sd_init_fail
        or      a
        jr      z, sd_init_ready
        dec     de
        ld      a, d
        or      e
        jr      nz, sd_init_acmd
        jr      sd_init_fail

sd_init_ready:
        ;; CMD58 - прочитать OCR и узнать, блочная адресация или байтовая.
        call    sd_arg_zero
        ld      c, #0x7A
        call    sd_cmd
        jr      c, sd_init_fail
        or      a
        jr      nz, sd_init_fail

        in      a, (SD_DAT)     ; старший байт OCR, бит 6 = CCS
        and     #0x40
        jr      z, sd_init_byte_addr
        ld      a, #CT_BLOCK
        ld      (_sd_type), a
        jr      sd_init_tail
sd_init_byte_addr:
        ld      a, #0x04        ; SDv2, но адресация байтовая
        ld      (_sd_type), a
sd_init_tail:
        in      a, (SD_DAT)     ; дочитать OCR
        in      a, (SD_DAT)
        in      a, (SD_DAT)

        call    sd_release
        or      a               ; CY=0
        ret

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
;; Портит B: у INIR он счётчик, в sd_pump512 - счётчик витков. Держать в B
;; что-то своё через вызов нельзя.
;; Общее начало: выдать CMD17 и дождаться токена данных. После возврата с
;; CY=0 карта готова отдать ровно 512 байт. Оно одно у чтения в память и у
;; чтения прямо в порт.
sd_read_start:
        ;; Аргумент: у блочной адресации это номер сектора как есть, у
        ;; байтовой пришлось бы умножать на 512 - пока поддерживаем только
        ;; блочную, остальное отсекаем.
        ld      a, (_sd_type)
        and     #CT_BLOCK
        jr      z, sd_read_fail

        ld      a, d
        ld      (sd_arg+0), a
        ld      a, e
        ld      (sd_arg+1), a
        ld      a, h
        ld      (sd_arg+2), a
        ld      a, l
        ld      (sd_arg+3), a

        ld      c, #0x51        ; CMD17
        call    sd_cmd
        jr      c, sd_read_fail
        or      a
        jr      nz, sd_read_fail

        ;; Ждём токен данных. Пока карта думает, она шлёт 0xFF.
        ld      de, #0x4000
sd_read_token:
        in      a, (SD_DAT)
        cp      #0xFF
        jr      nz, sd_read_got_token
        dec     de
        ld      a, d
        or      e
        jr      nz, sd_read_token
        jr      sd_read_fail
sd_read_got_token:
        cp      #0xFE
        jr      nz, sd_read_fail
        or      a               ; CY=0
        ret

;; Общий хвост: дочитать CRC и отпустить шину.
sd_read_finish:
        in      a, (SD_DAT)     ; CRC - не считаем, карта уже проверила
        in      a, (SD_DAT)
        call    sd_release
        or      a
        ret

_sd_read_sector::
        call    sd_read_start
        ret     c

        ;; 512 байт двумя INIR по 256. INIR - 21 такт на байт, кладёт по (HL)
        ;; и адресует порт через C: приёмник переезжает из IX в HL, наружу
        ;; интерфейс тот же. B попадает на A8-A15, Z-Controller смотрит
        ;; только младший байт адреса.
        push    ix
        pop     hl
        ld      c, #SD_DAT
        ld      b, #0           ; 0 = 256 итераций
        inir
        ld      b, #0
        inir
        push    hl              ; вернуть IX туда, куда доехал приёмник
        pop     ix
        jp      sd_read_finish

sd_read_fail:
        call    sd_release
        scf
        ret

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
;; Пара стоит 22 такта: IN A,(n) 11 плюс OUT (n),A 11.

sd_pump512:
        ld      b, #8           ; 8 витков по 64 байта = 512
sd_rsp_loop:
        .rept   64
        in      a, (SD_DAT)
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
;; Счётчик секторов - в памяти: B занят счётчиком витков sd_pump512.
_sd_read_multi_to_port::
        ld      a, (_sd_type)
        and     #CT_BLOCK
        jr      z, sd_multi_fail

        ld      a, d
        ld      (sd_arg+0), a
        ld      a, e
        ld      (sd_arg+1), a
        ld      a, h
        ld      (sd_arg+2), a
        ld      a, l
        ld      (sd_arg+3), a

        ld      c, #0x52        ; CMD18
        call    sd_cmd
        jr      c, sd_multi_fail
        or      a
        jr      nz, sd_multi_fail

sd_multi_block:
        ;; Токен ждём перед каждым блоком: карта вправе задуматься между
        ;; ними, и молча считать, что данные уже пошли, нельзя.
        ld      de, #0x4000
sd_multi_token:
        in      a, (SD_DAT)
        cp      #0xFF
        jr      nz, sd_multi_got
        dec     de
        ld      a, d
        or      e
        jr      nz, sd_multi_token
        jr      sd_multi_stop_fail
sd_multi_got:
        cp      #0xFE
        jr      nz, sd_multi_stop_fail

        call    sd_pump512
        in      a, (SD_DAT)     ; CRC
        in      a, (SD_DAT)

        ld      hl, #_sd_multi_count
        dec     (hl)
        jr      nz, sd_multi_block

        call    sd_multi_stop
        or      a
        ret

sd_multi_stop_fail:
        call    sd_multi_stop
        scf
        ret
sd_multi_fail:
        call    sd_release
        scf
        ret

;; Остановить поток и дождаться, пока карта отпустит линию.
sd_multi_stop:
        call    sd_arg_zero
        ld      c, #0x4C        ; CMD12
        call    sd_cmd
        ld      de, #0x4000
sd_multi_busy:
        in      a, (SD_DAT)
        cp      #0xFF
        jr      z, sd_multi_idle
        dec     de
        ld      a, d
        or      e
        jr      nz, sd_multi_busy
sd_multi_idle:
        jp      sd_release
