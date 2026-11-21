// SPDX-License-Identifier: MIT
#pragma once

// Атрибуты компилятора, GCC и MSVC.

// Не встраивать: редкая ветка, встроенная в горячий цикл, раздувает каждую
// его копию ради разового вызова; большой кадр вызываемой не ложится на
// кадр вызывающего.
#if defined(_MSC_VER)
#define SOUNDSINTH_NOINLINE __declspec(noinline)
#else
#define SOUNDSINTH_NOINLINE __attribute__((noinline))
#endif

// Не размножать под постоянные аргументы: копия функции в SRAM стоит места,
// а выигрыш от неё - пара команд на вызов. У MSVC таких копий нет.
#if defined(_MSC_VER)
#define SOUNDSINTH_NOCLONE
#else
#define SOUNDSINTH_NOCLONE __attribute__((noclone))
#endif

// Встроить всегда: тело в горячем цикле, внестрочная копия легла бы во флеш
// и звалась бы из SRAM на каждом витке.
#if defined(_MSC_VER)
#define SOUNDSINTH_ALWAYS_INLINE __forceinline
#else
#define SOUNDSINTH_ALWAYS_INLINE __attribute__((always_inline)) inline
#endif

// Редкий вызов (диагностика): тело собирается по размеру, а не по скорости.
#if defined(_MSC_VER)
#define SOUNDSINTH_COLD
#else
#define SOUNDSINTH_COLD __attribute__((cold))
#endif

// Разовый запуск из библиотеки, чей код живёт в ОЗУ: секция .text.startup
// остаётся во флеше.
#if defined(_MSC_VER)
#define SOUNDSINTH_INIT_CODE
#else
#define SOUNDSINTH_INIT_CODE __attribute__((section(".text.startup.init")))
#endif

// Код Beta Disk платы: функция - в банках ОЗУ DivMMC 8-15, секция
// .bdi_code.<имя> (скрипт компоновщика платы). Вне платы - обычный код.
#if defined(__arm__)
#define SOUNDSINTH_BDI_CODE(name) __attribute__((section(".bdi_code." name)))
#else
#define SOUNDSINTH_BDI_CODE(name)
#endif

// Условие обычно ложно: его ветка уходит с прямого пути.
#if defined(_MSC_VER)
#define SOUNDSINTH_UNLIKELY(x) (x)
#else
#define SOUNDSINTH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// Проверка аргументов printf-подобной функции: у MSVC такого атрибута нет.
#if defined(_MSC_VER)
#define SOUNDSINTH_PRINTF_FORMAT(fmt_index, first_arg)
#else
#define SOUNDSINTH_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#endif

// Своя секция .bss для выровненного массива: в общей .bss объектного файла
// он стоит набивки до своей границы. Выравнивание держит alignas, секция
// только выносит массив отдельно. У MSVC раскладки по секциям нет.
#if defined(_MSC_VER)
#define SOUNDSINTH_BSS_SECTION(name)
#else
#define SOUNDSINTH_BSS_SECTION(name) __attribute__((section(".bss." name)))
#endif
