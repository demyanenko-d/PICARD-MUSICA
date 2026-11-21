// SPDX-License-Identifier: MIT
#pragma once

// Модуль Wi-Fi с прошивкой AT (ESP-AT или AT ESP8266) на UART1: GPIO38 -
// передача, GPIO39 - приём, функция UART AUX. При WIFI=1 подключается к
// сети из настроек и выполняет запросы GET к zxart.ee для виртуального
// диска.

#include <cstdint>

#include "core/config/config.h"

namespace rp2350::wifi {

// Блок PSRAM модуля: кольцо тела ответа и за ним строки приёма. В активном
// приёме модуль не ждёт, и данные, которые Core1 ещё не забрал, копятся в
// кольце. Без блока модуль не поднимается.
constexpr uint32_t kBodyRingBytes  = 64u * 1024u; // степень двойки
constexpr uint32_t kLinkBlockBytes = kBodyRingBytes + 1152u;
void esp_link_attach(uint8_t* block);

// Зовётся на Core0 до запуска планировщика: заводит приём и задачу.
void esp_link_start(const soundsinth::config::Settings& s);

// В режиме конфигуратора вместо esp_link_start: та же задача ведёт
// терминал AT из раздела TOOLS, к сети не подключается.
void esp_link_start_tools();

// Нетронутый запас стека задачи модуля, байт; 0 - задача не запущена.
uint32_t esp_link_stack_free();

} // namespace rp2350::wifi
