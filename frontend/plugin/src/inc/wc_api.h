/*
 * wc_api.h - API Wild Commander для SDCC Z80.
 *
 * Единственная точка входа ко всем функциям - CALL 0x6006, номер функции
 * в A. WC портит AF, BC, DE, HL, IX при каждом вызове и сохраняет IY,
 * AF', BC', DE', HL', SP.
 */

#ifndef WC_API_H
#define WC_API_H

#include "types.h"

/* --
 * Константы
 * -- */

#define WC_ENTRY            0x6006  /* адрес точки входа WC              */

/* Стили рамки окна (type[3:0]): стандартные окна с курсором */
#define WC_WIN_NO_BORDER    0x00    /* без рамки, стандартное с курсором  */
#define WC_WIN_SINGLE       0x01    /* однолинейная рамка, без курсора    */
#define WC_WIN_DOUBLE       0x02    /* двойная рамка + курсор             */
#define WC_WIN_TYPE3        0x03    /* рамка 2-го типа, с курсором        */
#define WC_WIN_TYPE4        0x04    /* рамка 2-го типа, с заголовком/текстом, без курсора */
#define WC_WIN_TYPE5        0x05    /* рамка 2-го типа + курсор           */
#define WC_WIN_TYPE6        0x06    /* рамка 3-го типа, с курсором        */
#define WC_WIN_TYPE7        0x07    /* рамка 3-го типа, с заголовком/текстом, без курсора */
#define WC_WIN_TYPE8        0x08    /* рамка 3-го типа + курсор           */

/* Флаги type[7:4] */
#define WC_WIN_HEADER       0x10    /* инверсная строка-заголовок вверху  */
#define WC_WIN_WIDE_CURSOR  0x40    /* курсор на всю ширину окна          */
#define WC_WIN_SHADOW       0x80    /* тень вокруг окна                   */

/* wc_yn() mode */
#define WC_YN_OK_CANCEL     0x01
#define WC_YN_YES_NO        0x02
#define WC_YN_POLL          0x00    /* опросить без ожидания              */
#define WC_YN_EXIT          0xFF    /* закрыть меню YN                   */

/* wc_kbscn() mode */
#define WC_KBSCN_NORMAL     0x00    /* учитывать SHIFT / CL / Lang        */
#define WC_KBSCN_RAW        0x01    /* сырой скан-код                     */

/* wc_mngv_pl() mode */
#define WC_VIDEO_TXT        0x00    /* текстовый режим WC                 */
#define WC_VIDEO_BUF1       0x01    /* видеобуфер 1                       */
#define WC_VIDEO_BUF2       0x02
#define WC_VIDEO_BUF3       0x03
#define WC_VIDEO_BUF4       0x04

/* wc_adir() mode */
#define WC_ADIR_SEEK_START  0x00    /* установить позицию на начало       */
#define WC_ADIR_RESET_NEXT  0x01    /* сброс FindNext                     */

/* wc_stream() mode */
#define WC_STREAM_ROOT      0xFF    /* корневой поток                     */
#define WC_STREAM_CLONE     0xFE    /* клонировать текущий поток          */
#define WC_STREAM_WCDIR     0xFD    /* каталог WC                         */
/* 0x00, 0x01 = номер потока 0 или 1                                      */

/* wc_int_pl() mode */
#define WC_INT_DISABLE_ALL  0x00    /* отключить все прерывания WC        */
#define WC_INT_NO_TIME      0x01    /* отключить таймерное INT WC         */
#define WC_INT_NO_PS2       0x02    /* отключить PS/2 INT WC              */
#define WC_INT_PLUGIN       0xFF    /* установить обработчик плагина       */

/* wc_turbopl() mode */
#define WC_TURBO_CPU        0x00    /* B=0: настройка CPU; C=частота      */
#define WC_TURBO_AY         0x01    /* B=1: AY частота                    */
#define WC_TURBO_RESTORE    0xFF    /* восстановить оригинальный режим     */

/* Коды ошибок файловых операций */
#define WC_EOF              0x0F    /* конец файла / конец цепочки (LOAD512) - не ошибка! */

/* Флаги типа файла (wc_fentry, wc_mkfile, wc_delfl, wc_rename) */
#define WC_FILE_FLAG        0x00    /* объект является файлом             */
#define WC_DIR_FLAG         0x10    /* объект является каталогом          */

/* Коды ошибок FAT-операций */
#define WC_ERR_LONG_NAME    1       /* длинное имя некорректно            */
#define WC_ERR_SHORT_IDX    2       /* короткое имя требует индекса       */
#define WC_ERR_LONG_EXISTS  3       /* длинное имя уже существует         */
#define WC_ERR_SHORT_EXISTS 4       /* короткое имя уже существует        */
#define WC_ERR_NOT_FOUND    8       /* источник не найден                 */
#define WC_ERR_NO_SPACE     16      /* нет свободного места               */
#define WC_ERR_UNKNOWN      255     /* неизвестная ошибка                 */

/* Константы прокрутки wc_scrlwow (flags) */
#define WC_SCRL_DOWN        0x00    /* направление - вниз                 */
#define WC_SCRL_UP          0x01    /* направление - вверх                */
/* правка/влево (0x02, 0x03) зарезервированы, не работают                 */
#define WC_SCRL_WITH_ATTRS  0x80    /* прокручивать атрибуты вместе с символами */
#define WC_SCRL_CLEAR_SRC   0x40    /* очищать исходную позицию после прокрутки */
/* [5:2] = шаг: 0=1 символ, 0xF=16 символов (сдвинуть на N+1)           */
/* При step=0 и без флагов [7:2] - автоматически использует DMA          */

/* --
 * СТРАНИЦЫ ПАМЯТИ WC (физические номера страниц #00-#FF)
 * -- */

/* Текстовые экраны */
#define WC_PAGE_MTXPG       0x00    /* основной текстовый экран           */
#define WC_PAGE_FONT0       0x01    /* шрифт основного TXT + Hrust/MegaLZ*/
#define WC_PAGE_BUFPG       0x08    /* второй текстовый экран / буфер     */
#define WC_PAGE_FONT1       0x09    /* шрифт второго TXT                  */

/* Код WC */
#define WC_PAGE_CODE        0x02    /* основной код WC (#8000-#AFFF)      */
#define WC_PAGE_FAT_ENGINE  0x05    /* FAT engine (#4000) + stack + код   */

/* Каталоги панелей */
#define WC_PAGE_CTZPG       0x0B    /* начало каталога LEFT панели        */
#define WC_PAGE_CAT_LEFT    0x0B    /* стр. #0B-#13 = 9 стр.              */
#define WC_PAGE_CAT_RIGHT   0x14    /* стр. #14-#1C = 9 стр.              */
#define WC_CAT_MAX_PAGES    9       /* макс. страниц на каталог           */
#define WC_PAGE_RESB        0x1D    /* буфер сохранения окон              */
#define WC_PAGE_CPPG1       0x1E    /* позиции каталога LEFT              */
#define WC_PAGE_CPPG2       0x1F    /* позиции каталога RIGHT             */

/* Megabuffer / видеобуферы */
#define WC_PAGE_TVBPG       0x20    /* начало MEGABUFFER (64 стр.)        */
#define WC_PAGE_TVEPG       0x5F    /* конец MEGABUFFER                   */
#define WC_PAGE_GV1         0x20    /* видеобуфер 1 (16 стр.)             */
#define WC_PAGE_GV2         0x30    /* видеобуфер 2 (16 стр.)             */
#define WC_PAGE_GV3         0x40    /* видеобуфер 3 (16 стр.)             */
#define WC_PAGE_GV4         0x50    /* видеобуфер 4 (16 стр.)             */
#define WC_VIDEO_PAGES_PER_BUF  16  /* страниц в одном видеобуфере        */

/* Плагины */
#define WC_PAGE_PLHPG       0x60    /* заголовки плагинов (96)            */
#define WC_PAGE_PLGPG       0x61    /* начало кода плагинов (97)          */
#define WC_PAGE_PLEPG       0xA0    /* конец кода плагинов (160)          */
#define WC_PLUGIN_MAX_PAGES 64      /* макс. страниц для всех плагинов    */

/* TAP */
#define WC_PAGE_TAP_START   0xA1    /* смонтированный TAP-файл            */
#define WC_PAGE_TAP_END     0xBF    /* конец TAP (31 стр.)                */

/* vDOS RAM Disk */
#define WC_PAGE_VDOS_RAM    0xC0    /* начало RAM-диска vDOS              */
#define WC_PAGE_VDOS_END    0xE7    /* конец RAM-диска (40 стр. = 640 КБ) */
#define WC_PAGE_VDOS_FREE   0xE8    /* свободные стр. (#E8-#EF)          */

/* Системные потоки */
#define WC_PAGE_PNLPG       0xF0    /* поток левой панели                 */
#define WC_PAGE_PNL_RIGHT   0xF1    /* поток правой панели                */
#define WC_PAGE_INIPG       0xF2    /* поток плагина #00 / INI            */
#define WC_PAGE_STRPG       0xF3    /* поток плагина #01                  */
#define WC_PAGE_DBPG        0xF4    /* буфер SPG-загрузчика               */
#define WC_PAGE_D5PG        0xF5    /* загрузочная стр. 5                 */
#define WC_PAGE_D2PG        0xF6    /* загрузочная стр. 2                 */
#define WC_PAGE_RPPG        0xF7    /* пути панелей + флаг активной       */

/* vDOS системные */
#define WC_PAGE_VROM_START  0xF8    /* Virtual ROM (4 стр.)               */
#define WC_PAGE_VROM_END    0xFB
#define WC_PAGE_VDOS_TABLES 0xFC    /* vDOS Tables (#FC-#FD)              */
#define WC_PAGE_VDOS_DRV    0xFE    /* vDOS Drivers                       */
#define WC_PAGE_VDOS_TRAPS  0xFF    /* vDOS Traps                         */

/* --
 * СИСТЕМНАЯ ОБЛАСТЬ WC (#6000-#602F)
 * -- */

#define WC_SYS_PG0          0x6000  /* [1] страница в окне #0000          */
#define WC_SYS_PG4          0x6001  /* [1] страница в окне #4000          */
#define WC_SYS_PG8          0x6002  /* [1] страница в окне #8000          */
#define WC_SYS_PGC          0x6003  /* [1] страница в окне #C000          */
#define WC_SYS_ABT          0x6004  /* [1] ABT: флаг ESC (1=нажат)        */
#define WC_SYS_ENT          0x6005  /* [1] ENT: флаг ENTER (1=нажат)      */
#define WC_SYS_FUN          0x6006  /* [3] API entry point (JP instr)     */
#define WC_SYS_TMN          0x6009  /* [2] таймер INT (uint16, ++/frame)  */
#define WC_SYS_FEP          0x600B  /* [1] страница FAT engine в окне 0   */
#define WC_SYS_XPP          0x600C  /* [1] сохранённая страница           */
#define WC_SYS_CNFV         0x600D  /* [1] конфиг видео (%000-%111)       */
#define WC_SYS_HEI          0x600E  /* [1] высота экрана (25/30/36)       */
#define WC_SYS_TXTMD        0x600F  /* [1] текстовый режим                */
#define WC_SYS_INIFLG       0x6010  /* [1] INI загружен                   */
#define WC_SYS_RSQBC        0x6011  /* [2] RSQ buffer counter             */
#define WC_SYS_RSQB2        0x6013  /* [2] RSQ buffer 2                   */
#define WC_SYS_SDBSF        0x6015  /* [1] SD1 driver state               */
#define WC_SYS_SDBSF2       0x6016  /* [1] SD2 driver state               */
#define WC_SYS_F3ENTFL      0x6017  /* [1] F3/ENTER execution flag        */
#define WC_SYS_FEP2         0x6018  /* [2] FAT engine page extra          */
#define WC_SYS_PLRESAD      0x6020  /* [8] резидент перехода (JP page+HL) */
#define WC_SYS_PLRESCALL    0x6028  /* [8] резидент вызова (CALL page+HL) */

/* --
 * КОДЫ УСТРОЙСТВ (для wc_stream, регистр B)
 * -- */

#define WC_DEV_SD_ZC        0       /* SD (Z-Controller)                  */
#define WC_DEV_IDE_NEMO_M   1       /* IDE Nemo Master                    */
#define WC_DEV_IDE_NEMO_S   2       /* IDE Nemo Slave                     */
#define WC_DEV_SD_NGS       3       /* SD (NeoGS)                         */
#define WC_DEV_IDE_SMUC_M   4       /* IDE SMUC Master                    */
#define WC_DEV_IDE_SMUC_S   5       /* IDE SMUC Slave                     */
#define WC_DEV_SD2_ZC       6       /* SD2 (Z-Controller)                 */
#define WC_DEV_COUNT        7       /* всего устройств                    */

/* --
 * КОНФИГУРАЦИЯ ВИДЕО (CNFv, #600D)
 * -- */

#ifndef WC_CNFV_PWM
#define WC_CNFV_PWM         0       /* ШИМ (IDE)                          */
#endif
#ifndef WC_CNFV_VDAC3
#define WC_CNFV_VDAC3       1       /* 3-bit VDAC                         */
#endif
#ifndef WC_CNFV_VDAC4
#define WC_CNFV_VDAC4       2       /* 4-bit VDAC                         */
#endif
#ifndef WC_CNFV_VDAC5
#define WC_CNFV_VDAC5       3       /* 5-bit VDAC                         */
#endif
#ifndef WC_CNFV_VDAC2
#define WC_CNFV_VDAC2       7       /* VDAC2 (FT812)                      */
#endif

/* --
 * ВИДЕОРЕЖИМЫ TSConfig (для wc_gvmod)
 * -- */

/* Разрешение [7:6] */
#define WC_RRES_256x192     0x00
#define WC_RRES_320x200     0x40
#define WC_RRES_320x240     0x80
#define WC_RRES_360x288     0xC0

/* Графический режим [1:0] */
#define WC_VMODE_ZX         0x00    /* ZX Spectrum (6912 байт)            */
#define WC_VMODE_16C        0x01    /* 16 цветов (4 бит/пиксель)          */
#define WC_VMODE_256C       0x02    /* 256 цветов (8 бит/пиксель)         */
#define WC_VMODE_TXT        0x03    /* текстовый режим                    */

/* Прочие биты */
#define WC_VMODE_NOGFX      0x20    /* выключить графику                  */
#define WC_VMODE_NOTSU      0x10    /* выключить TSU                      */

/* Размеры текстового экрана */
#define WC_SCREEN_80x25     25
#define WC_SCREEN_80x30     30
#define WC_SCREEN_90x36     36
#define WC_SCREEN_W80       80
#define WC_SCREEN_W90       90

/* --
 * АППАРАТНЫЕ ПОРТЫ TSConfig (для __sfr / inline asm)
 * -- */

#define WC_PORT_PW0         0x10AF  /* Page Window 0                      */
#define WC_PORT_PW1         0x11AF  /* Page Window 1                      */
#define WC_PORT_PW2         0x12AF  /* Page Window 2                      */
#define WC_PORT_PW3         0x13AF  /* Page Window 3                      */
#define WC_PORT_VMOD        0x00AF  /* Video mode                         */
#define WC_PORT_VPAG        0x01AF  /* Video page                         */
#define WC_PORT_VPAL        0x07AF  /* Palette                            */
#define WC_PORT_BORDER      0x0FAF  /* Border color                       */
#define WC_PORT_SYSCONF     0x20AF  /* System config (turbo [1:0])        */
#define WC_PORT_CACHECONF   0x2BAF  /* Cache config (per-window enable)   */
/* Значения, которыми оперирует сама WC: */
#define WC_CACHE_ON         0x02    /* CACHE  - то, что WC ставит после turbo */
#define WC_CACHE_ON2        0x06    /* CACH2  - второй вариант из WC          */
#define WC_CACHE_OFF        0x00    /* всё выключено                          */

/* Записать CacheConf (#2BAF). Кэш сам не сбрасывается, данные с карты
 * приходят DMA мимо процессора, а WC включает кэш заново при каждой смене
 * турбо. */
void wc_cache_set(u8 value);

#define WC_PORT_FDDVIRT     0x29AF  /* FDD Virtualization (vDOS)          */

/* DMA порты */
#define WC_PORT_DMA_SL      0x1AAF  /* DMA Source Low                     */
#define WC_PORT_DMA_SH      0x1BAF  /* DMA Source High                    */
#define WC_PORT_DMA_SX      0x1CAF  /* DMA Source Page                    */
#define WC_PORT_DMA_DL      0x1DAF  /* DMA Dest Low                       */
#define WC_PORT_DMA_DH      0x1EAF  /* DMA Dest High                      */
#define WC_PORT_DMA_DX      0x1FAF  /* DMA Dest Page                      */
#define WC_PORT_DMA_N       0x26AF  /* DMA burst size                     */
#define WC_PORT_DMA_C       0x27AF  /* DMA command / status               */
#define WC_PORT_DMA_T       0x28AF  /* DMA burst count                    */

/* --
 * FAT32 ENTRY (стандартная 32-байтная структура каталога)
 * -- */

typedef struct {
    uint8_t  name[11];    /* +0:  короткое имя 8.3 (без точки)      */
    uint8_t  attr;        /* +11: атрибуты (EFLG)                   */
    uint8_t  _res1;       /* +12: reserved (NTRes)                  */
    uint8_t  crt_time_ms; /* +13: время создания (0.1 сек)          */
    uint16_t crt_time;    /* +14: время создания (FAT format)       */
    uint16_t crt_date;    /* +16: дата создания (FAT format)        */
    uint16_t acc_date;    /* +18: дата последнего доступа            */
    uint16_t cluster_hi;  /* +20: старшее слово кластера (CLSDE)    */
    uint16_t mod_time;    /* +22: время модификации                 */
    uint16_t mod_date;    /* +24: дата модификации                  */
    uint16_t cluster_lo;  /* +26: младшее слово кластера (CLSHL)    */
    uint32_t file_size;   /* +28: размер файла (SIZIK)              */
} wc_fat_entry_t;

/* Атрибуты FAT entry (attr поле) */
#define WC_FAT_ATTR_RDONLY  0x01    /* Read-only                          */
#define WC_FAT_ATTR_HIDDEN  0x02    /* Hidden                             */
#define WC_FAT_ATTR_SYSTEM  0x04    /* System                             */
#define WC_FAT_ATTR_VOLLBL  0x08    /* Volume label                       */
#define WC_FAT_ATTR_DIR     0x10    /* Directory                          */
#define WC_FAT_ATTR_ARCHIVE 0x20    /* Archive                            */
#define WC_FAT_ATTR_LFN     0x0F    /* Long File Name entry               */

/* Макросы для FAT date/time */
#define WC_FAT_TIME(h,m,s)  (uint16_t)(((h)<<11)|((m)<<5)|((s)>>1))
#define WC_FAT_DATE(y,m,d)  (uint16_t)((((y)-1980)<<9)|((m)<<5)|(d))

#define WC_FAT_TIME_HOUR(t)  (((t) >> 11) & 0x1F)
#define WC_FAT_TIME_MIN(t)   (((t) >> 5) & 0x3F)
#define WC_FAT_TIME_SEC(t)   (((t) & 0x1F) << 1)
#define WC_FAT_DATE_YEAR(d)  ((((d) >> 9) & 0x7F) + 1980)
#define WC_FAT_DATE_MONTH(d) (((d) >> 5) & 0x0F)
#define WC_FAT_DATE_DAY(d)   ((d) & 0x1F)

/* --
 * FindNext выходной буфер (формат записи)
 * -- */

/**
 * Структура записи, возвращаемой wc_findnext.
 * Порядок полей зависит от флагов (size/date/time опциональны).
 * При всех включённых флагах ([4:2]=111):
 */
typedef struct {
    uint32_t size;        /* [2]=1: размер файла                    */
    uint16_t date;        /* [3]=1: дата создания (FAT формат)      */
    uint16_t time;        /* [4]=1: время создания (FAT формат)     */
    uint8_t  flag;        /* Entry Flag: 0x10=DIR, 0x00=file        */
    char     name[1];     /* имя файла (переменная длина, ZS)       */
} wc_findnext_entry_t;

/* Флаги для wc_findnext */
#define WC_FIND_ALL         0x00    /* все записи [1:0]=00                */
#define WC_FIND_FILES       0x01    /* только файлы [1:0]=01              */
#define WC_FIND_DIRS        0x02    /* только каталоги [1:0]=10           */
#define WC_FIND_SHORT_ONLY  0x80    /* только короткие имена [7]=1        */
#define WC_FIND_WITH_SIZE   0x04    /* включить поле size [2]=1           */
#define WC_FIND_WITH_DATE   0x08    /* включить поле date [3]=1           */
#define WC_FIND_WITH_TIME   0x10    /* включить поле time [4]=1           */
#define WC_FIND_FULL (WC_FIND_WITH_SIZE | WC_FIND_WITH_DATE | WC_FIND_WITH_TIME)

/* --
 * КАТАЛОЖНАЯ ЗАПИСЬ ПАНЕЛИ WC (внутренний формат)
 * -- */

/**
 * INDEX entry (4 байта): ссылка из индекса на DATA entry.
 */
typedef struct {
    uint8_t  page;        /* +0: логическая страница (0-8)          */
    uint16_t addr;        /* +1: адрес в странице (#C000-#FDFF)     */
    uint8_t  name_len;    /* +3: длина имени                        */
} wc_cat_index_t;

/**
 * DATA entry (переменная длина): полная запись файла в каталоге.
 */
typedef struct {
    uint8_t  mark;        /* +0:  Mark flags (бит 0: помечен)       */
    char     ext[3];      /* +1:  расширение (без точки)            */
    uint32_t cluster;     /* +4:  первый кластер FAT32              */
    uint32_t size;        /* +8:  размер файла                      */
    uint16_t date;        /* +12: дата (FAT формат)                 */
    uint16_t time;        /* +14: время (FAT формат)                */
    uint8_t  flag;        /* +16: #10=файл, #00=каталог             */
    char     name[1];     /* +17: имя (переменная длина, ZS)        */
} wc_cat_data_t;

/* --
 * ЗАГОЛОВОК ПЛАГИНА (+0..+260)
 * -- */

#define WC_PLUGIN_ID        "WildCommanderMDL"
/* Версия формата заголовка. WC берёт плагин, если байт +32 лежит в
 * PLGOV(#02)..PLGCV(#10); на живом железе 0x10 даёт "wrong version",
 * поэтому в заголовок пишется 0x0A. */
#define WC_PLUGIN_VERSION   0x10    /* верх диапазона версий              */

/* Типы вызова плагина (поле +197) */
#define WC_PCOND_EXT_ONLY   0x00    /* только по расширению               */
#define WC_PCOND_ON_LOAD    0x01    /* при загрузке (ENTER)               */
#define WC_PCOND_TIMER      0x02    /* по таймеру / F10                   */
#define WC_PCOND_F10        0x03    /* из меню F10                        */
#define WC_PCOND_VIEWER     0x04    /* из меню Shift+F3                   */
#define WC_PCOND_F10_VIEW   0x05    /* F10 или Shift+F3                   */
#define WC_PCOND_F10_F2     0x11    /* F10 или F2                         */
#define WC_PCOND_F2         0x12    /* F2                                 */
#define WC_PCOND_F10_F4     0x13    /* F10 или F4                         */
#define WC_PCOND_F4         0x14    /* F4                                 */
#define WC_PCOND_F10_V_F4   0x15    /* F10, Shift+F3 или F4              */

/* --
 * DMA операции (коды для wc_dmapl и прямого доступа)
 * -- */

/* DMA device codes (биты [2:0] регистра DMA_C) */
#define WC_DMA_RAM_RAM      0x01    /* W=0: RAM->RAM copy                  */
#define WC_DMA_BLT_RAM      0x01    /* W=1: RAM blit (transparency)       */
#define WC_DMA_SPI_RAM_R    0x02    /* W=0: SPI->RAM                       */
#define WC_DMA_SPI_RAM_W    0x02    /* W=1: RAM->SPI                       */
#define WC_DMA_IDE_RAM_R    0x03    /* W=0: IDE->RAM                       */
#define WC_DMA_IDE_RAM_W    0x03    /* W=1: RAM->IDE                       */
#define WC_DMA_FILL         0x04    /* W=0: FILL RAM                      */
#define WC_DMA_RAM_CRAM     0x04    /* W=1: RAM->CRAM (палитра)           */
#define WC_DMA_RAM_SFILE    0x05    /* W=1: RAM->SFILE (спрайты)          */

/* Подфункции dmapl (А' регистр) */
#define WC_DMA_INIT_SD      0x00    /* задать src + dst                   */
#define WC_DMA_INIT_SRC     0x01    /* задать только src                  */
#define WC_DMA_INIT_DST     0x02    /* задать только dst                  */
#define WC_DMA_INIT_SRC_WIN 0x03    /* src из окна WC                     */
#define WC_DMA_INIT_DST_WIN 0x04    /* dst из окна WC                     */
#define WC_DMA_SET_T        0x05    /* задать DMA_T                       */
#define WC_DMA_SET_N        0x06    /* задать DMA_N                       */
#define WC_DMA_SET_TN       0x07    /* задать DMA_T + DMA_N               */
#define WC_DMA_ALIGN_SRC    0x08    /* выравнивание src                   */
#define WC_DMA_ALIGN_DST    0x09    /* выравнивание dst                   */
#define WC_DMA_ALIGN_SIZE   0x0A    /* размер выравнивания                */
#define WC_DMA_ALIGN_ALL    0x0B    /* все параметры выравнивания         */
#define WC_DMA_RUN_RAM      0xFC    /* RAM->RAM с ожиданием                */
#define WC_DMA_RUN_BLT      0xFB    /* BLT->RAM                           */
#define WC_DMA_RUN_FILL     0xFA    /* FILL                               */
#define WC_DMA_RUN_CRAM     0xF9    /* RAM->CRAM                           */
#define WC_DMA_RUN_SFILE    0xF8    /* RAM->SFILE                          */
#define WC_DMA_RUN_NOWAIT   0xFD    /* запустить без ожидания             */
#define WC_DMA_RUN_WAIT     0xFE    /* запустить с ожиданием              */
#define WC_DMA_BUSY_WAIT    0xFF    /* ждать готовности DMA               */

/* DMA page flags (для subfunc #00-#02: бит 7 регистров B/C) */
#define WC_DMA_PAGE_PLUGIN  0x80    /* страница из блока плагина (0-5)    */
/*      WC_DMA_PAGE_VIDEO   0x00       страница из видеобуферов (0-63)    */

/* Адресное пространство плагина во время работы (справочник по страницам
 * TS-Config, vDOS и FAT engine WC - в документации проекта):
 *   #0000-#3FFF  Окно 0: обычно FAT engine / буферы (page #05);
 *                #0000-#1FFF портит FindNext, #1000-#1447 - GEDPL
 *   #4000-#5FFF  Окно 1: FAT engine (page #05), код #6000-#7FFF
 *   #6000-#602F  Системная область WC (переменные, API entry point)
 *   #8000-#BFFF  Окно 2: код плагина (page из PLGPG)
 *   #C000-#FFFF  Окно 3: переключаемая (TXT/данные/видео)
 */

/* SOW - Structure of Window (16 байт) */
typedef struct {
    uint8_t  type;        /* +0: стиль+флаги (WC_WIN_*)              */
    uint8_t  cur_mask;    /* +1: маска цвета курсора                 */
    uint8_t  x;           /* +2: позиция X в символах (255=центр)    */
    uint8_t  y;           /* +3: позиция Y в символах (255=центр)    */
    uint8_t  width;       /* +4: ширина (0=весь экран)               */
    uint8_t  height;      /* +5: высота (0=весь экран)               */
    uint8_t  color;       /* +6: атрибут цвета (EGA: bg[7:4]|fg[3:0])  */
    uint8_t  _reserved;   /* +7: зарезервировано, всегда 0          */
    uint16_t buf_addr;    /* +8: адрес буфера фона (0xFFFF=нет)     */
    uint8_t  divider1;    /* +10: Y-смещение разделит. линии 1 от низа окна (0=нет) */
    uint8_t  divider2;    /* +11: Y-смещение разделит. линии 2 от низа окна (0=нет) */
    uint8_t  _ext[12];    /* +12-15: расширение (заголовок/курсор)  */
} wc_window_t;

/* --
 * Вспомогательный макрос инициализации окна
 * --
 * Пример:
 *   static wc_window_t my_win = WC_WIN_INIT(1, 0, 5, 5, 30, 10, 0x47, 0xFFFF);
 *   (border=1, x=5, y=5, w=30, h=10, color=bright white on black, no buf)
 */
/* WC_WIN_INIT: инициализация структуры окна.
 * Поля +12..+15 (_ext) = 0 -> подходит для типов без курсора (SINGLE, DOUBLE).
 * Для типов 0/3/6 (с курсором) нужно вручную инициализировать _ext[0..3].
 */
#define WC_WIN_INIT(style, flags, _x, _y, _w, _h, _color, _buf) \
    { (uint8_t)((style) | (flags)), 0x07, (_x), (_y), (_w), (_h), \
      (_color), 0, (_buf), 0, 0, {0,0,0,0,0,0,0,0,0,0,0,0} }

/* --
 * INIT / СТРАНИЧНОЕ УПРАВЛЕНИЕ
 * -- */

/**
 * Переключить страницу плагина на адрес #C000.
 * @param page  номер страницы плагина:
 *              0x00-0x3F - страницы RAM плагина
 *              0xFF      - страница с шрифтом WC (не использовать #E000-#FFFF!)
 *              0xFE      - первый текстовый экран WC
 * Остальные страницы #8000 и #0000 при этом НЕ изменяются.
 */
void wc_mngc_pl(uint8_t page);

/** Отобразить страницу плагина (page) на адрес #0000 */
void wc_mng0_pl(uint8_t page);

/** Отобразить страницу плагина (page) на адрес #8000 */
void wc_mng8_pl(uint8_t page);

/* --
 * UI / ОКНА
 * -- */

/**
 * Восстановить дисплей WC: палитру, смещения, текстовый режим.
 *
 * Обязательно вызывать при старте плагина, прежде чем вызывать
 * любые функции вывода окон (wc_prwow и т.д.).
 *
 * Внимание: портит область #1000-#1447 на странице, отображённой
 *            на #0000! Сохранить данные в этом диапазоне до вызова!
 *
 * НЕ переключает страницу в окне #0000-#3FFF.
 * НЕ переключает страницу в окне #C000.
 */
void wc_gedpl(void);

/**
 * Нарисовать окно win на экране (сохранить фон в buf_addr).
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата. Вызывать при 3.5 МГц!
 *
 * Структура win и строки заголовков должны находиться в #8000-#BFFF
 * или #0000-#3FFF (не в #C000-#FFFF!).
 *
 * После первого вызова WC записывает реальный адрес буфера в win->buf_addr
 * (если был 0). При buf_addr=0xFFFF фон не сохраняется.
 */
void wc_prwow(wc_window_t *win);

/**
 * Удалить окно win с экрана (восстановить фон из buf_addr).
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата. Вызывать при 3.5 МГц!
 */
void wc_rresb(wc_window_t *win);

/**
 * Напечатать строку str в окне win.
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата.
 *
 * @param y   строка внутри окна (1-based от верха содержимого)
 * @param x   столбец внутри окна (1-based)
 * @param len длина строки в символах
 *
 * Адрес win и строки str должны быть в #8000-#BFFF или #0000-#3FFF.
 */
void wc_prsrw(wc_window_t *win, const char *str, uint8_t y, uint8_t x, uint16_t len);

/**
 * Напечатать строку str в окне win c атрибутом цвета attr (EGA-формат: bg[7:4] | fg[3:0]).
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата.
 *
 * @param y   строка внутри окна (1-based от верха содержимого)
 * @param x   столбец внутри окна (1-based)
 * @param len длина строки в символах
 * @param attr EGA-атрибут: bg[7:4] | fg[3:0]
 *
 * Адрес win и строки str должны быть в #8000-#BFFF или #0000-#3FFF.
 */
void wc_prsrw_attr(wc_window_t *win, const char *str, uint8_t y, uint8_t x, uint16_t len, uint8_t attr);

/**
 * Вернуть физический адрес ячейки символа (y, x) внутри окна win.
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата.
 *
 * @param y  строка (1-based)
 * @param x  столбец (1-based)
 * @return   адрес байта символа в видеобуфере
 */
uint16_t wc_gadrw(wc_window_t *win, uint8_t y, uint8_t x);

/**
 * Нарисовать курсор в текущей позиции окна win.
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата.
 */
void wc_cursor(wc_window_t *win);

/**
 * Стереть курсор (восстановить исходный символ/атрибут).
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата.
 */
void wc_curser(wc_window_t *win);

/**
 * Меню выбора Да/Нет или ОК/Отмена.
 *
 * Работа в цикле (вызывать раз в кадр):
 *   1. wc_yn(WC_YN_OK_CANCEL)  - инициализация меню "ОК / Отмена"
 *   2. wc_yn(WC_YN_YES_NO)     - инициализация меню "Да / Нет"
 *   3. В цикле: wc_yn(WC_YN_POLL) - опросить и перерисовать кнопки
 *      (вызывать каждый кадр, пока не будет нажата клавиша)
 *   4. wc_yn(WC_YN_EXIT)       - закрыть меню (скрыть кнопки)
 *
 * После WC_YN_POLL:
 *   Z=1  -> нажато ОК / Да     (A = 0)
 *   Z=0  -> нажато Отмена / Нет (A != 0)
 *   Если клавиша ещё не нажата - никакого результата нет,
 *   нужно продолжать вызывать WC_YN_POLL в следующих кадрах.
 *
 * @param mode  WC_YN_OK_CANCEL (0x01), WC_YN_YES_NO (0x02),
 *              WC_YN_POLL (0x00), WC_YN_EXIT (0xFF)
 * @return  0 = ОК/Да нажато или ещё не нажато (Z=1),
 *          ненулевое = Отмена/Нет (Z=0)
 */
uint8_t wc_yn(uint8_t mode);

/**
 * Встроенный редактор строки ввода.
 *
 * Имеет несколько режимов работы (передаются как mode):
 *
 *   0xFF - инициализировать тип 0: редактировать экранный буфер.
 *          win=SOW, DE=CURMAX<<8|CURNOW (макс.длина и текущая позиция).
 *          Рисует курсор. Дальнейшие вызовы: 0x00 (опрос).
 *
 *   0x00 - опросить клавиатуру.  Вызывать раз в кадр.
 *          Возврат: A=0 - продолжает ввод, A!=0 - завершён.
 *          HL = текущий адрес курсора на экране.
 *
 *   0x01 - завершить ввод: стереть курсор.
 *          HL = адрес курсора на экране.
 *
 *   0xFD - инициализировать тип 2: редактировать буфер в памяти.
 *          HL=addr буфера, HL'=размер буфера,
 *          D=Y, E=X (позиция на экране),
 *          BC=CURMAX<<8|CURNOW.
 *
 *   0x02 - перерисовать блок редактора (обновить экран из буфера).
 *
 *   0xFE - инициализировать индикаторы (CAPS/INS/Lang/OVR).
 *          H=0: нижний заголовок, H=1: верхний.
 *
 *   0x03 - обновить индикаторы (вызывать раз в кадр).
 *
 *   0xFC - опросить состояние Caps/Ins/Ctrl+Shift.
 *          Возврат A: [7]=OVR, [6]=CAPS, [1:0]=язык.
 *
 * Нельзя одновременно вызывать 0x00 и 0xFC в одном кадре.
 *
 * @param win   SOW-структура окна
 * @param mode  режим (0xFF, 0x00, 0x01, 0xFD, 0x02, 0xFE, 0x03, 0xFC)
 */
void wc_istr(wc_window_t *win, uint8_t mode);

/**
 * Записать байт val в виде 2 шестнадцатеричных символов в экранный буфер.
 * @param addr  адрес в видеопамяти (в текущем экранном буфере)
 * @param val   значение, которое нужно отобразить как HEX (00-FF)
 *
 * Используется, например, для вывода шестнадцатеричного редактора.
 */
void wc_nork(uint16_t addr, uint8_t val);

/**
 * Напечатать форматированный текст str в окне win начиная с (y, x).
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата. Вызывать при 3.5 МГц!
 *
 * Текст может содержать управляющие коды: #0D (перенос строки),
 * #0E (центрирование), #0B,XX (сдвиг вправо), #0C,YY (сдвиг вниз),
 * #0x01-08 (цвета), #09 (инверсия), #FE,addr (ссылка).
 *
 * Структура win и строка str должны быть в #8000-#BFFF или #0000-#3FFF.
 *
 * @param y   начальная строка в окне (1-based)
 * @param x   начальный столбец в окне (1-based)
 * @return    номер строки, следующей за последней напечатанной
 */
uint8_t wc_txtpr(wc_window_t *win, const char *str, uint8_t y, uint8_t x);

/**
 * Напечатать n-е сообщение из блока текста str.
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата. Вызывать при 3.5 МГц!
 *
 * Блок состоит из абзацев, разделённых двойным нулём (0x00 0x00).
 * msg_num задаёт, который по счёту абзац использовать (0-based).
 *
 * @param msg_num  номер абзаца (0-based) в блоке текста
 * @param str      адрес начала текстового блока
 * @param y   начальная строка (1-based)
 * @param x   начальный столбец (1-based)
 * @return    номер строки после последней напечатанной
 */
uint8_t wc_mezz(wc_window_t *win, uint8_t msg_num, const char *str, uint8_t y, uint8_t x);

/**
 * Прокрутить прямоугольную область внутри окна.
 *
 * Включает основной текстовый экран на #C000 и оставляет его
 *    включённым после возврата. Вызывать при 3.5 МГц!
 *
 * Если step=0 и флаги [7:2] = 0 - автоматически используется DMA.
 *
 * @param y      верхняя строка области (1-based)
 * @param x      левый столбец области (1-based)
 * @param h      высота области в строках
 * @param w      ширина области в символах
 * @param flags  битовые флаги:
 *               [7]   = 1 -> прокручивать атрибуты вместе с символами
 *               [6]   = 1 -> очистить исходную позицию после прокрутки
 *               [5:2] = шаг прокрутки (0=1 символ, 0xF=16 символов)
 *               [1:0] = направление: 00=вниз, 01=вверх, 10/11=зарезерв.
 *               При [7:2]=0 и step=0 DMA задействуется автоматически.
 */
void wc_scrlwow(wc_window_t *win, uint8_t y, uint8_t x, uint8_t h, uint8_t w, uint8_t flags);

/* --
 * КЛАВИАТУРА
 * Все функции возвращают 0 = не нажато, 1 = нажато (wc_kbscn - код клавиши).
 * Автоповтор: у Space, стрелок, Tab, Enter, Bspc, F1-F10, Pgup/Pgdn, Home, End.
 * Без автоповтора: Alt, Shift, Ctrl.
 *
 * WC отдаёт результат флагом Z (NZ - нажата), в A мусор: обёртки переводят
 * Z в 0/1.
 * -- */

uint8_t wc_key_space(void);    /* Space / Пробел                       */
uint8_t wc_key_up(void);       /* Стрелка вверх                        */
uint8_t wc_key_down(void);     /* Стрелка вниз                         */
uint8_t wc_key_left(void);     /* Стрелка влево                        */
uint8_t wc_key_right(void);    /* Стрелка вправо                       */
uint8_t wc_key_tab(void);      /* Tab                                  */
uint8_t wc_key_enter(void);    /* Enter                                */
uint8_t wc_key_esc(void);      /* Escape                               */
uint8_t wc_key_bspc(void);     /* Backspace                            */
uint8_t wc_key_pgup(void);     /* Page Up                              */
uint8_t wc_key_pgdn(void);     /* Page Down                            */
uint8_t wc_key_home(void);     /* Home                                 */
uint8_t wc_key_end(void);      /* End                                  */
uint8_t wc_key_f1(void);       /* F1                                   */
uint8_t wc_key_f2(void);
uint8_t wc_key_f3(void);
uint8_t wc_key_f4(void);
uint8_t wc_key_f5(void);
uint8_t wc_key_f6(void);
uint8_t wc_key_f7(void);
uint8_t wc_key_f8(void);
uint8_t wc_key_f9(void);
uint8_t wc_key_f10(void);      /* F10                                  */
uint8_t wc_key_alt(void);      /* Alt (без автоповтора)                */
uint8_t wc_key_shift(void);    /* Shift (без автоповтора)              */
uint8_t wc_key_ctrl(void);     /* Ctrl (без автоповтора)               */

/**
 * Сканировать клавиатуру, вернуть код нажатой клавиши.
 * @param mode  WC_KBSCN_NORMAL (с учётом SHIFT/CL/Lang) или WC_KBSCN_RAW
 * @return  код клавиши, 0 = ничего не нажато
 */
uint8_t wc_kbscn(uint8_t mode);

uint8_t wc_key_del(void);      /* Delete                               */
uint8_t wc_key_caps(void);     /* Caps Lock                            */
uint8_t wc_key_any(void);      /* Любая клавиша                        */
void    wc_key_wait_release(void); /* Ждать отпускания всех клавиш     */
uint8_t wc_key_wait_any(void); /* Ждать нажатия любой клавиши          */
uint8_t wc_key_ins(void);      /* Insert                               */

/* --
 * ФАЙЛОВЫЕ ОПЕРАЦИИ
 * -- */

/**
 * Загрузить blocks блоков по 512 байт из текущего потока в dest.
 *
 * @param dest    адрес буфера назначения
 * @param blocks  количество блоков (blocks * 512 = байт)
 * @return  A = 0      - ОК, продолжение
 *          A = WC_EOF (0x0F) - нормальный конец цепочки FAT
 *                              (на последней секторной загрузке!)
 *          A = другое - реальная ошибка чтения
 *
 * HL при возврате = новое значение dest (указывает на следующий байт
 * после загруженных данных).
 *
 * Пример проверки ошибок:
 *   rc = wc_load512(addr, n);
 *   if (rc && rc != WC_EOF) { }  // I/O error
 */
uint8_t wc_load512(uint16_t dest, uint8_t blocks);

/**
 * Сохранить blocks блоков по 512 байт из src в текущий поток.
 *
 * HL при возврате = новое значение src (указывает на следующий байт
 * после сохранённых данных).
 *
 * @param src     адрес буфера источника
 * @param blocks  количество блоков
 */
void wc_save512(uint16_t src, uint8_t blocks);

/**
 * Перейти к началу файла плагина (позиционировать поток на первый блок).
 * Используется после открытия файла WC для повторного чтения.
 */
void wc_gipagpl(void);

/** Прочитать ENTRY (32 байта) файла в буфер по адресу addr */
void wc_tentry(uint16_t addr);

/**
 * Обойти цепочку секторов, записать в buf список секторов.
 * @param buf     адрес буфера назначения
 * @param bufend  адрес конца буфера
 */
void wc_chtosep(uint16_t buf, uint16_t bufend);

/**
 * Получить заголовок помеченного файла.
 * @param panel    IX = структура панели WC
 * @param filenum  номер файла в каталоге
 * @param namebuf  буфер для имени файла
 */
void wc_tmrkdfl(uint16_t panel, uint16_t filenum, uint16_t namebuf);

/**
 * Управление директорией / позиционирование.
 * @param mode  WC_ADIR_SEEK_START или WC_ADIR_RESET_NEXT
 */
void wc_adir(uint8_t mode);

/**
 * Открыть/переключить файловый поток.
 *
 * Параметр mode задаёт тип потока (передаётся внутри в регистр D):
 *   WC_STREAM_ROOT  (0xFF) - корневой поток FAT
 *   WC_STREAM_CLONE (0xFE) - клонировать текущий поток
 *   WC_STREAM_WCDIR (0xFD) - каталог WC
 *   0x00 / 0x01            - номер потока 0 или 1
 *
 * Если BC = 0xFFFF --- только переключить поток без пересоздания.
 * Если BC != 0xFFFF --- создать или пересоздать поток.
 *
 * @param mode  один из WC_STREAM_* или 0/1
 */
void wc_stream(uint8_t mode);

/**
 * Найти следующий файл/каталог в текущей директории.
 *
 * Внимание: портит область #0000-#1FFF на странице с драйвером.
 *    Не вызывать, если в этом диапазоне хранятся важные данные!
 *
 * @param entry_buf  адрес буфера для заголовка файла (32+ байт)
 * @param flags  фильтр:
 *               [7]   = 1 -> только короткие имена
 *               [4]   = 1 -> загрузить время создания
 *               [3]   = 1 -> загрузить дату создания
 *               [2]   = 1 -> загрузить размер файла
 *               [1:0] = 00 -> все объекты; 01 -> только файлы;
 *                      10 -> только каталоги
 * @return  0 (Z=1) = конец директории;
 *          ненулевое = найден файл/каталог
 */
uint8_t wc_findnext(uint16_t entry_buf, uint8_t flags);

/**
 * Найти файл по имени в текущей директории.
 * @param name_with_flag  адрес строки вида: [1 байт флагов][имя][0]
 * @return  0 = не найден, ненулевое = найден; после вызова DE=entry, HL=size
 */
uint8_t wc_fentry(uint16_t name_with_flag);

/** Пропустить n секторов по 512 байт в текущем потоке */
void wc_loadnone(uint8_t sectors);

/**
 * Открыть файл, найденный последним wc_fentry().
 * Позиционирует поток чтения на найденный файл.
 */
void wc_gfile(void);

/**
 * Войти в каталог, найденный последним wc_fentry().
 */
void wc_gdir(void);

/**
 * Создать файл.
 * @param name_with_flag  [1 байт флагов][длина][имя][0]
 * @return  0 = OK, ненулевое = ошибка
 */
uint8_t wc_mkfile(uint16_t name_with_flag);

/**
 * Создать каталог.
 * @param name  указатель на имя каталога (ZS-строка)
 * @return  0 = OK, ненулевое = ошибка
 */
uint8_t wc_mkdir(uint16_t name);

/**
 * Переименовать файл/каталог.
 * @param old_name  [1 байт флагов][старое имя][0]
 * @param new_name  [новое имя][0]
 * @return  0 = OK (Z), ненулевое = ошибка
 */
uint8_t wc_rename(uint16_t old_name, uint16_t new_name);

/**
 * Удалить файл.
 * @param name_with_flag  [1 байт флагов][имя][0]
 * @return  0 = OK (Z), ненулевое = ошибка
 */
uint8_t wc_delfl(uint16_t name_with_flag);

/* --
 * ГРАФИКА
 * -- */

/**
 * Переключить видеорежим.
 * @param mode  WC_VIDEO_TXT / WC_VIDEO_BUF1..4
 */
void wc_mngv_pl(uint8_t mode);

/**
 * Отобразить видеостраницу vpage на адрес #C000.
 * @param vpage  номер страницы (0x00-0x3F)
 */
void wc_mngcvpl(uint8_t vpage);

/**
 * Установить видеорежим TSConfig.
 * @param mode  байт VConfig (TSCONF_VM_* | TSCONF_RRES_* | ...)
 */
void wc_gvmod(uint8_t mode);

/** Установить Y-смещение прокрутки графики */
void wc_gyoff(uint16_t y);

/** Установить X-смещение прокрутки графики */
void wc_gxoff(uint16_t x);

/** Установить страницу тайловой карты */
void wc_gvtm(uint8_t page);

/**
 * Установить страницу графики тайловой плоскости.
 * @param plane  0 = плоскость 0, 1 = плоскость 1
 * @param page   номер страницы
 */
void wc_gvtl(uint8_t plane, uint8_t page);

/** Установить страницу графики спрайтов */
void wc_gvsgp(uint8_t page);

/** Отобразить видеостраницу vpage на адрес #0000 */
void wc_mng0vpl(uint8_t vpage);

/** Отобразить видеостраницу vpage на адрес #8000 */
void wc_mng8vpl(uint8_t vpage);

/* --
 * DMA
 * -- */

/**
 * Настройка и запуск DMA TSConfig.
 *
 * Подфункции DMA (передаются как subfunc):
 *
 *  Конфигурация:
 *   0x00 - задать src+dst: B=src_page, HL=src_addr, C=dst_page, DE=dst_addr
 *   0x01 - задать только src
 *   0x02 - задать только dst
 *   0x03 - src из координат окна (адрес -> окно WC)
 *   0x04 - dst из координат окна
 *   0x05 - задать DMA_T (тип тайла/спрайта)
 *   0x06 - задать DMA_N (число объектов)
 *   0x07 - задать T + N
 *   0x08 - выравнивание источника
 *   0x09 - выравнивание назначения
 *   0x0A - выравнивание размера
 *   0x0B - задать все параметры
 *
 *  Запуск:
 *   0xFC - RAM->RAM (с ожиданием завершения)
 *   0xFB - BLT->RAM
 *   0xFA - FILL (заполнение)
 *   0xF9 - RAM->CRAM (палитра)
 *   0xF8 - RAM->SFILE (спрайты)
 *   0xFD - запустить без ожидания (следует вызвать 0xFF для синхронизации)
 *   0xFE - запустить с ожиданием
 *   0xFF - ждать готовности DMA (busy-wait)
 *
 * Обычная последовательность:
 *   // Настройка src/dst выполняется перед вызовом через регистры SDCC
 *   wc_dmapl(0xFC);  // запустить и подождать
 */
void wc_dmapl(uint8_t subfunc);

/* --
 * РАЗНОЕ
 * -- */

/**
 * Управление тактовой частотой CPU или AY через WC.
 *
 * WC запоминает выбранный режим и восстанавливает его при 0xFF. Смена
 * частоты через WC включает кэш заново.
 *
 * @param mode   WC_TURBO_CPU  (0x00) - выбрать частоту процессора:
 *                 param = 0: 3.5 МГц
 *                 param = 1: 7   МГц
 *                 param = 2: 14  МГц  ← для быстрых вычислений
 *                 param = 3: 28  МГц
 *               WC_TURBO_AY   (0x01) - выбрать частоту AY:
 *                 param = 0: 1.750  МГц
 *                 param = 1: 1.773  МГц
 *                 param = 2: 3.5    МГц
 *                 param = 3: 3.546  МГц
 *               WC_TURBO_RESTORE (0xFF) - восстановить режимы CPU+AY
 *                 из настроек WC (игнорирует param)
 *
 * @param param  параметр частоты (0-3); игнорируется при RESTORE
 */
void wc_turbopl(uint8_t mode, uint8_t param);

/**
 * Получить параметр из INI-файла плагина.
 * @param param_num  номер параметра
 * @return  номер опции (0-based), 0xFF если параметр не задан
 */
uint8_t wc_prm_pl(uint8_t param_num);

/**
 * Управление прерываниями WC.
 *
 * По выходу из плагина (возврат из main) WC автоматически
 *    восстанавливает все параметры INT, установленные этой функцией!
 *
 * @param mode  WC_INT_DISABLE_ALL (0x00) - отключить все прерывания WC
 *              WC_INT_NO_TIME     (0x01) - отключить таймерное INT
 *              WC_INT_NO_PS2      (0x02) - отключить PS/2 INT
 *              WC_INT_PLUGIN      (0xFF) - установить свой обработчик;
 *                в этом случае использовать wc_int_pl_handler(addr)
 */
void wc_int_pl(uint8_t mode);

/**
 * Установить ASM-обработчик прерываний плагина.
 * Вызывать после wc_int_pl(WC_INT_PLUGIN).
 *
 * Обработчик должен заканчиваться на RET (не RETI).
 * Всё, что нужно сохранить - регистры; стек WC НЕ менять.
 *
 * @param handler_addr  адрес ASM-обработчика в памяти плагина.
 */
void wc_int_pl_handler(uint16_t handler_addr);

/* --
 * ВСПОМОГАТЕЛЬНЫЕ МАКРОСЫ
 * -- */

/**
 * Атрибут цвета (EGA-формат): bg[7:4] | fg[3:0]
 * @param bg  цвет фона (0-15)
 * @param fg  цвет текста (0-15)
 * Пример: WC_COLOR(WC_BLUE, WC_BRIGHT_WHITE) = 0x1F
 */
#define WC_COLOR(bg, fg)    ((uint8_t)(((bg) << 4) | (fg)))

/* 16 цветов (YGRB: Bright, Green, Red, Blue) */
#define WC_BLACK            0
#define WC_BLUE             1
#define WC_RED              2
#define WC_MAGENTA          3
#define WC_GREEN            4
#define WC_CYAN             5
#define WC_YELLOW           6
#define WC_WHITE            7
#define WC_DARK_GRAY        8
#define WC_BRIGHT_BLUE      9
#define WC_BRIGHT_RED       10
#define WC_BRIGHT_MAGENTA   11
#define WC_BRIGHT_GREEN     12
#define WC_BRIGHT_CYAN      13
#define WC_BRIGHT_YELLOW    14
#define WC_BRIGHT_WHITE     15

/* --
 * Параметры плагина (заполняются crt0.s при вызове плагина WC)
 * --
 *
 * Передаются WC через регистры:
 *   A'  -> wc_file_ext    (индекс расширения в списке WC)
 *   BC  -> wc_file_name   (указатель на z-строку с именем файла)
 *   HL  -> wc_file_size   (младшее слово, 16 бит)
 *   DE  -> wc_file_size+2 (старшее слово, 16 бит; для файлов > 64 КБ)
 *
 * wc_file_size занимает 4 байта в _DATA (32-bit LE):
 *   lo = *(uint16_t *)&wc_file_size
 *   hi = *((uint16_t *)&wc_file_size + 1)
 */
extern uint8_t   wc_file_ext;          /* индекс расширения             */
extern char *    wc_file_name;         /* char *: указатель на имя файла */
extern uint32_t  wc_file_size;         /* 32-bit LE: [+0]=lo, [+2]=hi   */

/* Код возврата плагина: запись в wc_exit_code перед return из main() */
extern uint8_t   wc_exit_code;

/** Номер текущего объекта в панели (от 1). Передаётся WC в HL'. */
extern uint16_t  wc_file_idx;
/** Количество объектов в панели (включая ".."). Передаётся WC в DE'.
 *  Если wc_file_idx == wc_file_count - это последний файл. */
extern uint16_t  wc_file_count;

#define WC_EXIT_ESC         0   /* закрыть плагин / выйти (стоп)      */
#define WC_EXIT_UNRECOGNIZED 1  /* файл не распознан -> другому плагину */
#define WC_EXIT_NEXT        2   /* перейти к следующему файлу           */
#define WC_EXIT_RELOAD_DIR  3   /* перечитать каталог WC               */
#define WC_EXIT_PREV        4   /* перейти к предыдущему файлу          */

/* --
 * СИСТЕМНЫЕ ПЕРЕМЕННЫЕ - inline чтение через volatile-указатели
 * --
 *
 * Эти макросы читают системную область WC (#6000+) напрямую:
 */

/** Текущая страница в окне 0 (#0000-#3FFF) */
#define wc_get_pg0()        (*(volatile uint8_t *)WC_SYS_PG0)
/** Текущая страница в окне 1 (#4000-#7FFF) */
#define wc_get_pg4()        (*(volatile uint8_t *)WC_SYS_PG4)
/** Текущая страница в окне 2 (#8000-#BFFF) */
#define wc_get_pg8()        (*(volatile uint8_t *)WC_SYS_PG8)
/** Текущая страница в окне 3 (#C000-#FFFF) */
#define wc_get_pgc()        (*(volatile uint8_t *)WC_SYS_PGC)

/** Флаг ESC (ABT): 1 = пользователь нажал ESC */
#define wc_get_abt()        (*(volatile uint8_t *)WC_SYS_ABT)
/** Флаг ENTER (ENT): 1 = пользователь нажал ENTER */
#define wc_get_ent()        (*(volatile uint8_t *)WC_SYS_ENT)

/** Таймер INT (инкрементируется каждым прерыванием) */
#define wc_get_timer()      (*(volatile uint16_t *)WC_SYS_TMN)

/** Страница FAT engine в окне 0 */
#define wc_get_fep()        (*(volatile uint8_t *)WC_SYS_FEP)

/** Конфигурация видео (CNFv): 0=PWM, 1=3bit, ..., 7=VDAC2 */
#define wc_get_cnfv()       (*(volatile uint8_t *)WC_SYS_CNFV)

/** Высота экрана в строках (25, 30 или 36) */
#define wc_get_height()     (*(volatile uint8_t *)WC_SYS_HEI)

/** Текстовый режим (TXTmd) */
#define wc_get_txtmode()    (*(volatile uint8_t *)WC_SYS_TXTMD)

/** SD1 driver state */
#define wc_get_sd1_state()  (*(volatile uint8_t *)WC_SYS_SDBSF)
/** SD2 driver state */
#define wc_get_sd2_state()  (*(volatile uint8_t *)WC_SYS_SDBSF2)

/* --
 * ВСПОМОГАТЕЛЬНЫЕ МАКРОСЫ ДЛЯ FAT
 * -- */

/**
 * Собрать 32-битный кластер из двух 16-битных полей FAT entry.
 * @param hi  cluster_hi (CLSDE, +20)
 * @param lo  cluster_lo (CLSHL, +26)
 */
#define WC_FAT_CLUSTER(hi, lo) ((uint32_t)(hi) << 16 | (uint32_t)(lo))

/**
 * Построить буфер с флагом для wc_fentry / wc_mkfile / wc_delfl.
 * Пример:
 *   static uint8_t buf[] = WC_FAT_NAME(WC_FILE_FLAG, "readme.txt");
 * Раскрывается в: { 0x00, 'r','e','a','d','m','e','.','t','x','t', 0 }
 */
#define WC_FAT_NAME(flag, name) { (flag), name "\0" }

/**
 * Построить буфер для wc_mkfile (flag + size + name).
 * Пример:
 *   static uint8_t buf[32];
 *   wc_mkfile_buf(buf, 1024, "output.bin");
 */
#define wc_mkfile_buf(buf, sz, nm) do { \
    (buf)[0] = WC_FILE_FLAG; \
    *(uint32_t*)&(buf)[1] = (uint32_t)(sz); \
    uint8_t _i = 0; \
    while ((nm)[_i]) { (buf)[5 + _i] = (nm)[_i]; _i++; } \
    (buf)[5 + _i] = 0; \
} while(0)

/* --
 * РЕЗИДЕНТНЫЙ ПЕРЕХОД / ВЫЗОВ (для многостраничных плагинов)
 * --
 *
 * Для плагинов, занимающих несколько страниц в #8000-#BFFF:
 *
 *   Резидент перехода (#6020):
 *     A  = номер страницы плагина (от 0)
 *     HL = адрес перехода (#8000-#BFFF)
 *     Выполняет: переключение страницы в окне 2, JP (HL)
 *
 *   Резидент вызова (#6028):
 *     A  = номер страницы плагина (от 0)
 *     HL = адрес вызова (#8000-#BFFF)
 *     Выполняет: переключение, CALL (HL), возврат обратно
 *     Сохраняет все регистры кроме A и HL!
 *
 * Пример:
 *   // Вызвать функцию load_data на странице 2 плагина
 *   wc_resident_call(2, (uint16_t)&load_data);
 */

/** Перейти (JP) в код на другой странице плагина (без возврата!) */
void wc_resident_jump(uint8_t page, uint16_t addr);

/** Вызвать (CALL) код на другой странице плагина (с возвратом) */
void wc_resident_call(uint8_t page, uint16_t addr);

/* --
 * LOAD256 / LOADNONE (дополнительные файловые функции)
 * -- */

/**
 * Загрузить blocks блоков по 256 байт (используется видео плеером).
 * Параметры и возврат аналогичны wc_load512.
 */
uint8_t wc_load256(uint16_t dest, uint8_t blocks);

/* --
 * MNG0VPL / MNG8VPL (видеостраницы в другие окна)
 * --
 *
 * MNG0VPL (Win 0, #0000-#3FFF):
 *   - WC API сохраняет/восстанавливает Win 0 при каждом CALL #6006
 *   - Но GEDPL затирает #1000-#1447!
 *   - Нельзя передавать через #0000-#3FFF параметры WC-функциям
 *     (SOW, строки, буферы), т.к. WC ремапит окно внутри вызова
 *   - Безопасно для данных плагина между вызовами WC API
 *
 * MNGCVPL (Win 3, #C000-#FFFF):
 *   - WC API сбрасывает Win 3 на TXT-страницу (#00) при выходе
 *     из каждого вызова P8000 и не восстанавливает.
 *   - После любого CALL #6006 нужно заново вызвать mngc_pl()
 *   - Удобно для загрузки: mngcvpl(N) -> load512(0xC000, 32)
 */






void wc_strset(char *dsr, uint16_t len, char c);


#endif /* WC_API_H */
