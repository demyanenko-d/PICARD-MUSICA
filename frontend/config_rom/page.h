/*
 * page.h - страница настроек глазами машины.
 *
 * Плата кладёт её на 0x2000 (банк 0 ОЗУ DivMMC) и пишет туда прямо: это
 * её массив в SRAM, по шине ничего не едет.
 *
 * Раскладка обязана совпадать с core/config/config_page.h платы. Порядок
 * байт у обоих младшим вперёд, поэтому структуры читаются как есть.
 */

#ifndef CONFIG_ROM_PAGE_H
#define CONFIG_ROM_PAGE_H

#include "../common/types.h"

#define PAGE_BASE 0x2000u

/* 'PCFG' младшим байтом вперёд. */
#define PAGE_MAGIC 0x47464350uL
#define PAGE_VERSION 2u

#define PAGE_KIND_NUMBER 0
#define PAGE_KIND_CHOICE 1

typedef struct {
    u32 magic;
    u16 version;
    u16 field_count;
    u16 desc_offset;
    u16 values_offset;
    /* Значения из флеша: правка их не меняет, по ним и видно, что изменено. */
    u16 saved_offset;
    u16 mask_offset;
    u16 strings_offset;
    u16 reserved;
} page_header_t;

/* 16 байт на запись: переход к полю - сдвиг, а не умножение. */
typedef struct {
    u16 name;
    u16 comment;
    u16 section;
    u16 choices; /* 0 - поле числовое */
    u8 min;
    u8 max;
    u8 kind;
    u8 reserved[5];
} page_field_t;

typedef struct {
    u8 value;
    u8 pad;
    u16 name;
} page_choice_t;

/* Доступ к странице: смещения внутри неё, а не указатели - так короче на
   Z80 и так их пишет плата. */
#define page_at(off) ((u8 *)(PAGE_BASE + (off)))
#define page_hdr() ((const page_header_t *)PAGE_BASE)
#define page_str(off) ((const char *)(PAGE_BASE + (off)))

#endif
