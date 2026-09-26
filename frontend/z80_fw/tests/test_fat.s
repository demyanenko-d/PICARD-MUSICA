;; test_fat.s - проверка монтирования и пересчёта кластера в сектор.
;;
;; Результат по 0x8F00:
;;   +0   0 успех, 1 карта не поднялась, 2 монтирование не удалось
;;   +1   fat_spc
;;   +2   fat_spc_shift
;;   +4   fat_start        (4 байта, младший вперёд)
;;   +8   fat_data_start   (4)
;;   +12  fat_root_clus    (4)
;;   +16  lba корневого кластера (4) - проверка fat_clus_to_lba

        .module test_fat
        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_clus_to_lba
        .globl  _fat_spc
        .globl  _fat_spc_shift
        .globl  _fat_start
        .globl  _fat_data_start
        .globl  _fat_root_clus
        .globl  _fat_clus
        .globl  _fat_lba
        .globl  start

RESULT  .equ    0x8F00

        .area   _CODE

start::
        ld      sp, #0x8F00

        call    _sd_init
        jr      nc, tf_init_ok
        ld      a, #1
        ld      (RESULT+0), a
        halt
tf_init_ok:

        call    _fat_mount
        jr      nc, tf_mount_ok
        ld      a, #2
        ld      (RESULT+0), a
        halt
tf_mount_ok:

        ld      a, (_fat_spc)
        ld      (RESULT+1), a
        ld      a, (_fat_spc_shift)
        ld      (RESULT+2), a

        ld      hl, #RESULT+4
        ld      de, #_fat_start
        call    tf_copy4
        ld      hl, #RESULT+8
        ld      de, #_fat_data_start
        call    tf_copy4
        ld      hl, #RESULT+12
        ld      de, #_fat_root_clus
        call    tf_copy4

        ;; Пересчитать корневой кластер в номер сектора: для spc=1 это
        ;; ровно data_start, так что расхождение сразу видно.
        ld      hl, #_fat_clus
        ld      de, #_fat_root_clus
        call    tf_copy4
        call    _fat_clus_to_lba
        ld      hl, #RESULT+16
        ld      de, #_fat_lba
        call    tf_copy4

        xor     a
        ld      (RESULT+0), a
        halt

tf_copy4:
        ld      b, #4
tf_copy4_l:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    tf_copy4_l
        ret
