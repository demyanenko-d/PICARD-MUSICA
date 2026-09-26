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

// Встроить всегда: тело в горячем цикле, внестрочная копия легла бы во флеш
// и звалась бы из SRAM на каждом витке.
#if defined(_MSC_VER)
#define SOUNDSINTH_ALWAYS_INLINE __forceinline
#else
#define SOUNDSINTH_ALWAYS_INLINE __attribute__((always_inline)) inline
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
