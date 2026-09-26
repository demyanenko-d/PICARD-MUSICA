/* Базовые типы для Z80/SDCC.
 *
 * Соглашение вызова --sdcccall 1 (проверено по коду, который выдаёт sdcc):
 *
 *   (uint8_t)                 | A
 *   (uint16_t) или указатель  | HL
 *   (uint32_t)                | HL старшее слово, DE младшее
 *   (uint8_t, uint8_t)        | A, L
 *   (uint8_t, uint16_t)       | A, DE
 *   (uint16_t, uint8_t)       | HL, второй через стек (чистит вызванный)
 *   Возврат uint8_t           | A
 *   Возврат uint16_t / ptr    | HL
 *
 * Половинки uint32_t лежат обратно ассемблерной части проекта, где
 * старшее слово в DE, - отсюда ex de,hl в fw_glue.s.
 */

#ifndef TYPES_H
#define TYPES_H

typedef unsigned char  uint8_t;
typedef signed   char  int8_t;
typedef unsigned int   uint16_t;   /* SDCC Z80: int = 16 бит */
typedef signed   int   int16_t;
typedef unsigned long  uint32_t;   /* SDCC Z80: long = 32 бит */
typedef signed   long  int32_t;

typedef uint8_t bool_t;
#define TRUE  ((bool_t)1)
#define FALSE ((bool_t)0)

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef int8_t   i8;
typedef int16_t  i16;
typedef uint32_t u32;
typedef int32_t  i32;

#endif /* TYPES_H */
