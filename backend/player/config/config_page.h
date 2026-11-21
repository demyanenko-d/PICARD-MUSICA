// SPDX-License-Identifier: MIT
#pragma once

// Поля настроек глазами конфигуратора.
//
// Поле - номер в общем ряду: ключи таблицы файла настроек, затем привязки
// клавиш джойстика, затем строки. Меню платы (config_ui) правит настройки
// через этот ряд, поэтому новый ключ таблицы становится полем сам.
//
// Страница конфигуратора - 8 КБ ОЗУ DivMMC: машина видит её на 0x2000,
// плата - как свой массив. В ней экран-терминал (config_term) и экран
// инструмента (config_tools).

#include <cstdint>

#include "core/config/config.h"
#include "core/config/config_fields.h"

namespace soundsinth::config {

inline constexpr uint32_t kConfigPageBytes = 8192;

// Как поле правится: число - стрелками в окне, перечисление - выбором из
// списка, клавиша - нажатием на USB, текст - вводом строки, файл - выбором
// из файлов карты, флаг - переключением.
enum class PageKind : uint8_t { Number = 0, Choice = 1, Key = 2, Text = 3, File = 4, Flag = 5 };

inline constexpr uint8_t kNoField = 0xff;

// Файлы ПЗУ в корне карты: их собирает порт при входе в конфигуратор, меню
// предлагает их для полей вида File.
inline constexpr uint32_t kRomFilesMax = 16;
struct ConfigPageFiles {
    uint8_t count;
    uint8_t pad;
    char name[kRomFilesMax][kRomFileMax + 1];
};

// Маска доступных полей: бит на поле, единица - поле правится. Недоступное
// меню показывает, но не даёт выбрать.
uint64_t settings_active_mask(const Settings& s);

struct PageFieldInfo {
    const char* name;
    Kind kind;
    PageKind page_kind;
    uint8_t min; // у строки - признак секрета
    uint8_t max; // у строки - знаков
};
PageFieldInfo config_field_info(uint32_t i);
// Значение поля; у строки - её длина.
uint8_t config_field_value(const Settings& s, uint32_t i);
// Строку не трогает: её пишут через config_field_text.
void config_field_set(Settings& s, uint32_t i, uint8_t v);
// Буфер строкового поля, nullptr - поле не строковое.
char* config_field_text(Settings& s, uint32_t i);
const char* config_field_text(const Settings& s, uint32_t i);
// Номер поля по имени ключа, kNoField - нет такого.
uint8_t config_field_by_name(const char* name);
// Имя значения n вида kind; у ПЗУ вместо SLOT1..3 - имя файла области.
const char* config_choice_name(const Settings& s, Kind kind, uint32_t n);

} // namespace soundsinth::config
