#pragma once

// HOSTLINK - связь платы расширения с хостом ZX Spectrum по шине Z80.
//
// Плата отвечает хосту, принимает от него файл и отдаёт телеметрию.
// Ядро связи (bus, host_protocol, hostlink) о музыке и PSRAM не знает.
// Эмуляция GS живёт в плеере и зовёт загрузчик трека, divmmc - эмулятор SD.
//
// --- Что нужно для переноса в другой проект ---
//
//   backend/ports/rp2350/hostlink/                - каталог
//   backend/ports/rp2350/hw_config.h              - карта ресурсов PIO, DMA, IRQ
//   backend/ports/rp2350/firmware_config.h        (порты протокола, ключи SOUNDSINTH_*)
//   backend/ports/rp2350/storage/                 - эмулятор SD для divmmc, без него bus.cpp не собрать
//   backend/player/protocol/host_protocol.h/.cpp
//   backend/platform/log.h, log_rings.*         - журнал; консоль даёт порт
//
// Внешние зависимости: pico-sdk (hardware_pio/dma/irq/clocks, pico_time)
// и debug_log().
//
// --- Слои ---
//
//   bus.h, bus_setup.h  железо: PIO, DMA, IRQ, приём и выдача байта, счётчики
//   host_protocol.h     протокол поверх байтов (платформонезависим)
//   hostlink.h          фасад: запуск, обслуживание, печать здоровья связи
//
// --- Производительность ---
//
// На горячем пути нет виртуальных вызовов и лишних слоёв:
//
//   * путь байта с шины: ISR (SRAM) -> HostProtocol; окно данных
//     потребитель забирает сам (take_received), сессию и сброс зовёт
//     poll(). Фасад в пути байта не участвует: он настраивает связку на
//     старте и обслуживает её между обменами;
//   * колбэки - обычные указатели на функции;
//   * методы фасада в цикле ожидания (service, service_and_wait) тривиальны;
//   * горячие функции в SRAM (__not_in_flash_func): флеш и PSRAM делят
//     один QMI, промах кэша XIP в ISR - до 6.8 мкс.
//
// --- Владение ---
//
// Вся связь на одном ядре (Core1): ISR, протокол, фасад. HostProtocol не
// потокобезопасен, второе ядро к нему не обращается. Исключение - log_task
// на Core0: только счётчики и печать (rom_emu_serve_late_poll,
// строка эмуляции GS, геттеры divmmc). Данные между ядрами передаёт
// потребитель своими средствами. Телеметрию и сессию потребитель ведёт
// через сам HostProtocol (set_position, mark_session_ready и прочие).

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
void set_file_info(uint8_t minutes, uint8_t seconds, uint16_t num_samples, uint16_t num_patterns,
                   uint16_t num_instruments);

} // namespace hostlink
