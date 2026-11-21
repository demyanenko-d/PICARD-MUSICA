// SPDX-License-Identifier: MIT
#pragma once

// Конфигуратор-терминал на плате: меню (core/config/config_ui) рисует
// экран в страницу, ПЗУ его выводит и присылает нажатия. Цикл Core1.

#include <cstdint>

#include "player/config/config_page.h"
#include "player/config/config_ui.h"

namespace player::config {

// Память меню: две копии настроек и рабочая сетка. Только в режиме
// конфигуратора (PSRAM, взятая при входе в него).
uint32_t config_term_mem_bytes();

// Память меню при входе в конфигуратор; nullptr - меню будет в ПЗУ.
void config_term_reserve(void* mem);

// Страница - терминал, меню - с начала, если память дали. files - файлы
// ПЗУ на карте, nullptr - нет. Зовётся до выхода машины из сброса.
bool config_term_begin(uint8_t* page, const soundsinth::config::ConfigPageFiles* files);
bool config_term_on();

// Нажатия, захват клавиши, кадр. Возвращает выход, который выбрали.
soundsinth::config::ConfigUiExit config_term_serve(uint8_t* page);
const soundsinth::config::Settings& config_term_draft();

} // namespace player::config
