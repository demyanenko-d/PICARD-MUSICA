#pragma once

// Пометка "держать в SRAM, а не во флеше". На RP2350 (SOUNDSINTH_RP2350_HOT_PATH,
// задаётся только прошивкой) - секция линковщика __not_in_flash_func, на PC
// пустой макрос.
//
// Причина: флеш (XIP) и PSRAM сидят на одной шине QMI. Код, выполняемый из
// флеша, ждёт освобождения шины, пока другое ядро читает сэмплы из PSRAM.
// Поэтому в SRAM держится то, что обязано работать без этой конкуренции:
// рендер звука (декодирование голоса, TrackerEngine::render_add, тик
// секвенсора и непрерывных эффектов, ревербератор; разбор строки паттерна -
// во флеше, раз на строку) и обработка шины Z80 на Core1 (HostProtocol,
// эмуляция SD и Z-Controller).

#if SOUNDSINTH_RP2350_HOT_PATH
#include "pico.h" // pico/platform.h напрямую не включается (#error), только через pico.h
#define SOUNDSINTH_HOT_PATH(func_name) __not_in_flash_func(func_name)
// Та же секция атрибутом перед типом - для Class::method вне класса, где
// SOUNDSINTH_HOT_PATH даёт невалидный синтаксис. section_name - суффикс
// .time_critical.
#define SOUNDSINTH_HOT_PATH_ATTR(section_name) __not_in_flash(section_name)
#else
#define SOUNDSINTH_HOT_PATH(func_name) func_name
#define SOUNDSINTH_HOT_PATH_ATTR(section_name)
#endif
