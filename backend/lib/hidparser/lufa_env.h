#pragma once

// Подложка под HIDParser.c из LUFA вместо LUFA/Common/Common.h.
//
// Разборщику от всего окружения библиотеки нужны ровно три вещи: два
// макроса атрибутов и bool. Остальное - стандартные заголовки. Поэтому
// здесь десяток строк, а не перенос половины LUFA.
//
// Сами HIDParser.c, HIDParser.h и HIDReportData.h взяты как есть; правлены
// в них только строки #include в HIDParser.h. Так обновление с верховьев
// остаётся сравнением двух файлов, а не разбором чужих правок.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Подсказки компилятору, не влияющие на смысл: у LUFA они объявляют, что
// указатель не бывает нулевым и что функция чистая.
#ifndef ATTR_NON_NULL_PTR_ARG
#if defined(__GNUC__)
#define ATTR_NON_NULL_PTR_ARG(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#define ATTR_NON_NULL_PTR_ARG(...)
#endif
#endif

#ifndef ATTR_CONST
#if defined(__GNUC__)
#define ATTR_CONST __attribute__((const))
#else
#define ATTR_CONST
#endif
#endif

// Склейка имён через раскрытие аргументов: HIDReportData.h собирает ими
// имена HID_RI_DATA_BITS_<n> из значений.
#ifndef CONCAT
#define CONCAT(x, y) x##y
#endif
#ifndef CONCAT_EXPANDED
#define CONCAT_EXPANDED(x, y) CONCAT(x, y)
#endif

// Больше из HIDParser не выйдет: у него MAX сравнивает размеры отчётов.
#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

// Единственное, что разборщику нужно из HIDClassCommon.h, - три
// значения. Взяты оттуда как есть, вместо переноса всего заголовка.
enum HID_ReportItemTypes_t {
    HID_REPORT_ITEM_In = 0,
    HID_REPORT_ITEM_Out = 1,
    HID_REPORT_ITEM_Feature = 2,
};

// Пределы разбора. У LUFA умолчания рассчитаны на AVR с парой килобайт
// ОЗУ, и они нам как раз впору - таблица предметов это и есть вся память
// разборщика.
//
//   REPORTITEMS - сколько полей запомнить. Единица: отбор
//                 CALLBACK_HIDParser_FilterHIDReportItem разбирает поле
//                 на месте и всегда отвечает "не класть", так что в
//                 таблице не остаётся ничего. Предмет к вызову отбора
//                 заполнен целиком, включая смещение и номер отчёта.
//                 Тридцать два было мало по-настоящему: пульт с двумя
//                 десятками кнопок, шестью осями и шляпкой даёт больше, и
//                 дескриптор переставал разбираться целиком;
//   COLLECTIONS - глубина и число вложенных коллекций;
//   REPORT_IDS  - сколько разных номеров отчётов у устройства.
#ifndef HID_MAX_REPORTITEMS
#define HID_MAX_REPORTITEMS 1
#endif
#ifndef HID_MAX_COLLECTIONS
#define HID_MAX_COLLECTIONS 10
#endif
#ifndef HID_MAX_REPORT_IDS
#define HID_MAX_REPORT_IDS 8
#endif
#ifndef HID_USAGE_STACK_DEPTH
#define HID_USAGE_STACK_DEPTH 16
#endif
#ifndef HID_STATETABLE_STACK_DEPTH
#define HID_STATETABLE_STACK_DEPTH 3
#endif
