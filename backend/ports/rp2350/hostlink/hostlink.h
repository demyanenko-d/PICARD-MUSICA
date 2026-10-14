// SPDX-License-Identifier: MIT
#pragma once

// Связь платы с хостом ZX Spectrum по шине Z80: ответ хосту, приём файла, телеметрия.
// О музыке и PSRAM не знает; эмуляция GS живёт в плеере, divmmc - эмулятор SD.
//
// Вся связь на Core1: ISR, протокол, фасад. HostProtocol не потокобезопасен,
// log_task на Core0 читает только счётчики. Фасад в пути байта не участвует;
// горячие функции в SRAM: один промах кэша XIP - около 0.3 мкс, обработчик
// целиком во флеше - до 6.8 мкс.

#include <cstdint>

#include "player/protocol/host_protocol.h"

namespace hostlink {

// Кому отдавать пришедшее с хоста - колбэки протокола (host_protocol.h).
using Callbacks = player::protocol::HostProtocol::Callbacks;

// Поднять шину. Вызывается один раз с ядра, которое будет её обслуживать:
// irq_set_exclusive_handler регистрирует обработчик на вызывающем ядре,
// вызов с другого ядра ломает связь целиком.
void init(player::protocol::HostProtocol& protocol, const Callbacks& callbacks);

// Обслужить связь: разобрать команду хоста и вооружить следующую (на
// каждом вызове), напечатать здоровье связи и события протокола (не чаще
// раза в 20 мс). Можно звать в каждом обороте цикла.
void service();

// То же, что service(), плюс wfi: для циклов ожидания данных, ядро будит ISR.
void service_and_wait();

// Общая длительность и счётчики содержимого - хост показывает их сразу
// после разбора метаданных, до первого звука. Через фасад, а не протокол:
// зовёт и эмуляция GS, у которой протокола нет.
void set_file_info(uint8_t minutes, uint8_t seconds, uint16_t num_samples, uint16_t num_patterns, uint16_t num_instruments);

} // namespace hostlink
