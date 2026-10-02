// SPDX-License-Identifier: MIT
#pragma once

// Страница настроек для ПЗУ-конфигуратора.
//
// Машина видит её на 0x2000 как страницу ОЗУ DivMMC, плата - как свой
// массив в SRAM. Передавать по шине нечего: плата пишет страницу прямо,
// машина читает её как обычную память.
//
// Направления разные:
//
//   вниз (плата -> ПЗУ)   описатели полей, текущие значения, маска
//                         доступных полей;
//   вверх (ПЗУ -> плата)  только массив значений.
//
// Описатели строятся из той же таблицы ключей, что печатает файл
// настроек, поэтому новое поле появляется в конфигураторе само.
//
// Порядок байт - младшим вперёд, как у обоих: ARM и Z80.

#include <cstdint>

#include "core/config/config.h"

namespace soundsinth::config {

// Номер формата страницы. Поднимать при любой перестановке полей
// заголовка или записи поля: ПЗУ сверяет его и отказывается рисовать
// чужую страницу.
inline constexpr uint16_t kConfigPageVersion = 2;

inline constexpr uint32_t kConfigPageBytes = 8192;

// 'PCFG' младшим байтом вперёд.
inline constexpr uint32_t kConfigPageMagic = 0x47464350u;

// Запись поля - 16 байт, чтобы на Z80 переход к полю был сдвигом, а не
// умножением.
inline constexpr uint32_t kConfigFieldBytes = 16;

// Вид поля в странице: число правится прибавлением, перечисление -
// переходом по списку.
enum class PageKind : uint8_t { Number = 0, Choice = 1 };

// Заголовок страницы, 20 байт. Все смещения - от начала страницы.
struct ConfigPageHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t field_count;
    uint16_t desc_offset;   // массив записей по kConfigFieldBytes
    uint16_t values_offset; // uint8_t[field_count], следом сумма
    // Значения, какими они лежат во флеше. Пишутся один раз при сборке
    // страницы и правкой не меняются: ПЗУ по ним помечает изменённые поля
    // и показывает прежнее значение. Без этого правка увозится незаметно.
    uint16_t saved_offset;
    uint16_t mask_offset;    // биты доступности, по полю на бит
    uint16_t strings_offset; // начало области строк
    uint16_t reserved;
};
static_assert(sizeof(ConfigPageHeader) == 20, "the page header layout is frozen");

// Запись поля. Смещения ведут в область строк; строки с завершающим нулём.
struct ConfigPageField {
    uint16_t name;    // имя ключа
    uint16_t comment; // однострочное описание
    uint16_t section; // заголовок раздела перед полем, 0 - нет
    uint16_t choices; // список значений, 0 - поле числовое
    uint8_t min;
    uint8_t max;
    uint8_t kind; // PageKind
    uint8_t reserved[5];
};
static_assert(sizeof(ConfigPageField) == kConfigFieldBytes, "the field record layout is frozen");

// Список значений перечисления: счётчик, затем по записи на значение.
struct ConfigPageChoice {
    uint8_t value;
    uint8_t pad;
    uint16_t name;
};
static_assert(sizeof(ConfigPageChoice) == 4, "the choice record layout is frozen");

// Маска доступных полей: бит на поле в порядке таблицы, единица - поле
// правится. Недоступное поле ПЗУ показывает, но не даёт выбрать.
uint32_t settings_active_mask(const Settings& s);

// Сколько полей в странице. Оно же число байт в массиве значений.
uint32_t config_page_field_count();

// Построить страницу целиком: заголовок, описатели, строки, значения,
// маска. Возвращает занятую длину, ноль - не поместилось.
uint32_t config_page_build(const Settings& s, uint8_t* page, uint32_t cap);

// Переписать только значения и маску: зовётся после каждой правки с
// экрана, строки и описатели при этом не трогаются.
bool config_page_refresh(const Settings& s, uint8_t* page, uint32_t cap);

// Забрать значения из страницы в настройки, привести к допустимым и
// применить зависимости. false - страница не наша или сумма не сошлась,
// настройки не тронуты.
bool config_page_apply(const uint8_t* page, uint32_t cap, Settings& s);

} // namespace soundsinth::config
