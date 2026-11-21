// SPDX-License-Identifier: MIT
#pragma once

// Карта аппаратных ресурсов RP2350: блоки PIO, автоматы, каналы DMA,
// приоритеты прерываний.
//
// Ресурсы кончаются молча. Захват лишнего автомата роняет чужой явный
// захват по panic, и плата встаёт без строки в лог. Поэтому раскладка
// в одном месте, включая звук.
//
// --- Блоки PIO ---
//
//   pio0   DivMMC - детектор чтения памяти (SM0), сборка адреса (SM1),
//          трапы (SM2); TR-DOS - отдача ПЗУ навигатора и отметка выборки;
//          своё ПЗУ - отдача и вход в DOS; только порты - сторож сброса.
//          ROM_BLK_N ведут rom_detect и rom_join (DivMMC) и отдача
//          навигатора (TR-DOS), при своём ПЗУ - SIO.
//   pio1   всё, что выставляет байт: пины D0..D7 и BUFF_OE_N привязаны к
//          блоку, а привязка у PIO блочная. Детектор портов (SM0), захват
//          записей (SM1), выдача байта (SM2).
//   pio2   звук и port_join (с TR-DOS - port_join_hold); DivMMC - trap_join
//          и trap_route; TR-DOS - trdos_gate и dos_drive (вывод DOS_N);
//          своё ПЗУ - paging_filter (фильтр 7FFD) и dos_drive. Все автоматы
//          по остатку.
//
// Звуку нужен отдельный блок: i2s_out (GPIO 44-46) требует
// pio_set_gpio_base(16), настройка блочная, а шинные автоматы читают
// пины ниже 16 (MREQ 7, WR 4, RD 5, M1 3, A0..A7 8-15, BUFF_OE_N 2).
// pioasm проставляет .used_gpio_ranges, SDK сверяет их с базой блока при
// загрузке программы. Занятость инструкций считать по .pio.h.
//
// --- Порядок захвата ---
//
// Кто берёт автомат по явному номеру, обязан успеть раньше тех, кто
// зовёт pio_claim_unused_sm, иначе номер уводят и явный захват падает по
// panic. Явные захваты: автомат выдачи байта, детекторы памяти, детектор
// портов, захват записей, трапы. По остатку: звук.
//
// --- Каналы DMA ---
//
// Всего 16, все по остатку: порты - три, звук - два (ping/pong); DivMMC -
// ещё четыре на чтение памяти (таблица страниц и байт, по паре на
// уровень) и семь на трапы; TR-DOS - два на ПЗУ навигатора; своё ПЗУ -
// пять; только порты - канал выдачи байта. Занятость в лог не печатается.

#include "hardware/pio.h"

namespace bus {
// Окно 8 КБ адресов Z80: ПЗУ DivMMC и навигатора на 0x0000-0x1FFF, банк их
// ОЗУ на 0x2000-0x3FFF.
inline constexpr uint32_t kZ80BankBytes = 0x2000u;
} // namespace bus

// ОЗУ DivMMC, 16 банков по 8 КБ: верх SRAM, адрес задаёт скрипт компоновщика.
// Граница 64 КБ; банки 8-15 при Beta Disk платы заняты его кодом и данными.
extern "C" uint8_t bus_divmmc_ram[16][bus::kZ80BankBytes];
// Код Beta Disk - в тех банках: SOUNDSINTH_BDI_CODE (platform/compiler.h).
// Таблицы шины и ПЗУ DivMMC: массив в своей секции .busmem.<имя>; порядок
// секций и выравнивание держит скрипт компоновщика.
#define SOUNDSINTH_BUSMEM(name) __attribute__((section(".busmem." name)))

namespace bus {

// Автомат выдачи байта - в pio1: пины данных и OE_N привязаны к этому
// блоку. Детектор и сборка адреса пинов не ведут, они в pio0.
inline PIO const PIO_SERVE     = pio1;
inline constexpr uint SM_SERVE = 2;
// Детектор чтения и сборка адреса. Звука здесь нет.
inline PIO const PIO_DETECT         = pio0;
inline constexpr uint SM_ROM_DETECT = 0;
inline constexpr uint SM_ROM_JOIN   = 1;
// Наблюдатели (только читают пины, ничего не выставляют) - в pio1.
inline PIO const PIO_WATCH         = pio1;
inline PIO const PIO_AUDIO         = pio2;
inline constexpr uint SM_BUS_WRITE = 1; // PIO1: захват записей в память и в порты
inline constexpr uint SM_ROM_TRAP  = 2; // PIO0: трапы DivMMC по выборке команды
// PIO0: TR-DOS с ПЗУ машины - отдача ПЗУ навигатора и отметка выборки. Ни с
// DivMMC, ни со своим ПЗУ не бывает: блок у них один. Решатель DOS и вывод
// DOS_N живут в pio2 и берутся по остатку.
inline constexpr uint SM_TRDOS_NAV   = 0;
inline constexpr uint SM_TRDOS_FETCH = 1;
inline constexpr uint SM_PORT_DETECT = 0; // PIO1: детектор чтения порта
// PIO0: своё ПЗУ Пентагона. Ни с DivMMC, ни с триггером
// TR-DOS не бывает: блок у них один.
inline constexpr uint SM_PENTAGON_ROM = 0;
inline constexpr uint SM_DOS_ENTER    = 1;

// --- Приоритеты прерываний ---
//
// Меньше число - выше приоритет. Порядок задан сроками: у каждого
// обработчика свой запас до момента, когда Z80 защёлкнет байт.
//
//   0x40  запись в порт: байт уже на шине, состояние пора менять
//   0x60  чтение порта состоялось: байт ушёл переносом DMA задолго до
//         входа сюда, обработчик готовит следующий
//   0x80  звук и будильник сна Core1: их прерывать можно
//   0xC0  USB-хост: ниже звука, ответ машине на чтение порта USB не ждёт
//
// 0x80 равен configMAX_SYSCALL_INTERRUPT_PRIORITY (порог BASEPRI
// планировщика): 0x80 и больше FreeRTOS маскирует в критических секциях,
// меньше - никогда. Шинные обработчики стоят выше
// порога намеренно и не маскируются ни одной критической секцией.

inline constexpr uint8_t IRQ_PRIO_BUS_WR  = 0x40; // запись в порт
inline constexpr uint8_t IRQ_PRIO_PORT_RD = 0x60; // чтение порта состоялось
inline constexpr uint8_t IRQ_PRIO_RELAXED = 0x80; // звук, будильник сна Core1
inline constexpr uint8_t IRQ_PRIO_BDI     = 0xa0; // поток Beta Disk: ядро ВГ93
inline constexpr uint8_t IRQ_PRIO_USB     = 0xc0; // контроллер USB-хоста
static_assert(IRQ_PRIO_BUS_WR < IRQ_PRIO_PORT_RD && IRQ_PRIO_PORT_RD < IRQ_PRIO_RELAXED && IRQ_PRIO_RELAXED < IRQ_PRIO_BDI && IRQ_PRIO_BDI < IRQ_PRIO_USB,
              "the interrupt priority ladder is bus write, port read, audio, Beta Disk, USB");

// --- Линии DMA_IRQ ---
//
// Индексы для dma_irqn_* и DMA_IRQ_NUM. Линии 2 и 3 свободны.
inline constexpr uint DMA_IRQ_INDEX_AUDIO   = 0; // завершения каналов I2S
inline constexpr uint DMA_IRQ_INDEX_PORT_RD = 1; // чтение порта состоялось

} // namespace bus
