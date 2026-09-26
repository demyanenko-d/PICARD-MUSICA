;; fw_glue.s - переходники между z80_fw и соглашением вызовов SDCC.
;;
;; Библиотека внутри пользуется тем, что для Z80 дёшево: ошибка во флаге
;; переноса, 32-битные числа в DEHL, приёмник в IX. Соглашение SDCC
;; (проверено по коду, который выдаёт компилятор):
;;
;;     u8              -> A
;;     u16, указатель  -> HL
;;     u32             -> HL старшее слово, DE младшее   (у нас наоборот)  
;;     (u8, u8)        -> A, L
;;     (u8, u16)       -> A, DE
;;     (u16, u8)       -> HL, второй через стек
;;     возврат u8      -> A
;;
;; Больше половины рамки подходит без переходников: fw_scr_cls,
;; fw_scr_putc, fw_scr_puts, fw_kbd_get, fw_irq_init и прочие уже берут
;; аргумент там, где его кладёт SDCC, и объявлены в fw.h напрямую.
;;
;; Переходник стоит десяток тактов на вызов, случающийся раз на сектор, -
;; дешевле, чем переписывать горячие пути, считанные по тактам.
;;
;; IX сохраняется вокруг каждого вызова библиотеки: SDCC держит в нём
;; указатель кадра и ждёт, что вызванная функция его не тронет, а fat.s
;; (монтирование, каталог, чтение FAT, отрезки файла) и приём в память
;; по IX его портят. Без сохранения функция на Си, читающая свои
;; переменные через IX после такого вызова, получит чужой кадр, а на
;; выходе ld sp, ix - уход в никуда.

        .module fw_glue

        .globl  _fw_sd_init
        .globl  _fw_mount
        .globl  _fw_page_check
        .globl  _fw_dir_open_root
        .globl  _fw_dir_open_ent
        .globl  _fw_dir_next
        .globl  _fw_dir_load
        .globl  _fw_dir_get
        .globl  _fw_dir_page
        .globl  _fw_find
        .globl  _fw_open
        .globl  _fw_adopt
        .globl  _fw_use
        .globl  _fw_seek
        .globl  _fw_read_sectors
        .globl  _fw_read_bytes
        .globl  _fw_scr_at

        .globl  _sd_init
        .globl  _fat_mount
        .globl  _fat_page_check
        .globl  _fat_dir_open
        .globl  _fat_dir_open_root
        .globl  _fat_dir_next
        .globl  _fat_dir_load
        .globl  _fat_dir_get
        .globl  _fat_find
        .globl  _fat_open
        .globl  _fat_adopt
        .globl  _fat_use
        .globl  _fat_seek
        .globl  _fat_read_sectors
        .globl  _fat_read_bytes
        .globl  _fat_clus
        .globl  _fat_ent_clus
        .globl  _fw_scr_at_hl

        .area   _CODE

;; Флаг переноса -> код возврата: 0 получилось, 1 нет.
;;
;; В ассемблере ошибку носит перенос, в Си так не скажешь.
fw_cy:
        ld      a, #0
        rla                     ; перенос въезжает в младший бит
        ret

;; u8 fw_sd_init(void)
_fw_sd_init::
        push    ix
        call    _sd_init
        pop     ix
        jp      fw_cy

;; u8 fw_mount(void)
_fw_mount::
        push    ix
        call    _fat_mount
        pop     ix
        jp      fw_cy

;; u8 fw_page_check(void)
_fw_page_check::
        push    ix
        call    _fat_page_check
        pop     ix
        jp      fw_cy

;; u8 fw_dir_open_root(void)
_fw_dir_open_root::
        push    ix
        call    _fat_dir_open_root
        pop     ix
        jp      fw_cy

;; u8 fw_dir_open_ent(void) - открыть каталог, чей кластер лежит в записи
;; fat_ent_clus, то есть в той, которую только что выбрали.
_fw_dir_open_ent::
        ld      hl, #_fat_clus
        ld      de, #_fat_ent_clus
        ld      b, #4
fw_g_c4:
        ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    fw_g_c4
        push    ix
        call    _fat_dir_open
        pop     ix
        jp      fw_cy

;; u8 fw_dir_next(void): 1 есть запись, 0 каталог кончился, 0xFF карта.
_fw_dir_next::
        push    ix
        call    _fat_dir_next
        pop     ix
        ret     nc
        ld      a, #0xFF
        ret

;; u8 fw_dir_load(void)
_fw_dir_load::
        push    ix
        call    _fat_dir_load
        pop     ix
        jp      fw_cy

;; u8 fw_dir_get(u16 i) - номер записи уже в HL, как и надо библиотеке.
_fw_dir_get::
        push    ix
        call    _fat_dir_get
        pop     ix
        jp      fw_cy

;; u8 fw_find(const char *name): 1 нашли, 0 нет, 0xFF карта.
_fw_find::
        push    ix
        call    _fat_find
        pop     ix
        ret     nc
        ld      a, #0xFF
        ret

;; void fw_dir_page(u8 page) - включить страницу в окно 0xC000.
;;
;; Нужна сортировке списка: таблица указателей каталога лежит в этой
;; странице, и сортировать её на C можно только когда она домаплена.
_fw_dir_page::
        push    ix
        call    _fat_page_set
        pop     ix
        ret

;; u8 fw_open(u8 slot) - номер уже в A.
_fw_open::
        push    ix
        call    _fat_open
        pop     ix
        jp      fw_cy

;; void fw_adopt(void)
_fw_adopt::
        push    ix
        call    _fat_adopt
        pop     ix
        ret

;; void fw_use(u8 slot)
_fw_use::
        push    ix
        call    _fat_use
        pop     ix
        ret

;; void fw_seek(u32 pos)
;;
;; Единственное место, где половинки 32-битного числа меняются местами:
;; SDCC держит старшее слово в HL, библиотека - в DE.
_fw_seek::
        ex      de, hl
        push    ix
        call    _fat_seek
        pop     ix
        ret

;; u8 fw_read_sectors(void *dst, u16 sectors)
_fw_read_sectors::
        push    ix
        push    hl
        pop     ix
        ld      b, d
        ld      c, e
        call    _fat_read_sectors
        pop     ix
        jp      fw_cy

;; u8 fw_read_bytes(void *dst, u16 count)
_fw_read_bytes::
        push    ix
        push    hl
        pop     ix
        ld      b, d
        ld      c, e
        call    _fat_read_bytes
        pop     ix
        jp      fw_cy

;; void fw_scr_at(u8 x, u8 y) - SDCC кладёт x в A, y в L; библиотека ждёт
;; координаты в H и L.
_fw_scr_at::
        ld      h, a
        jp      _fw_scr_at_hl
