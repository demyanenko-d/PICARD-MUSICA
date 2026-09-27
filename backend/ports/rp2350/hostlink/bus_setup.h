// Внутренний заголовок шины: развязка монтажа и логики.
//
// Логика - в bus.cpp; то, чем она пользуется, но что к ней не относится, -
// в bus_setup.cpp: раздача автоматов PIO, загрузка программ, цепочки DMA,
// регистрация обработчиков прерываний.
//
// Не публичный: наружу шина смотрит через bus.h, здесь только то, что
// нужно двум её единицам трансляции.
//
// Разделы:
//   1. Раскладка - номера портов протокола, производные пины, размеры
//      таблиц. inline constexpr: одно определение на все единицы
//      трансляции, без extern.
//   2. Состояние монтажа - захваченные каналы DMA и автоматы, таблицы для
//      железа. Определения в bus.cpp, рядом с логикой, которая их читает;
//      монтаж их расставляет.
//   3. Обработчики - определены в bus.cpp (логика), регистрируются в
//      bus_setup.cpp (монтаж).

#pragma once

#include "bus.h"
#include "firmware_config.h"
#include "hw_config.h"

#include "hardware/pio.h"

namespace bus {

// --- 1. Раскладка ---
//
// Блоки PIO, номера автоматов, каналы DMA и приоритеты прерываний - в
// hw_config.h. Здесь номера портов протокола, пины, производные от PIN_A0,
// и размеры таблиц, на которые ссылаются границы массивов ниже.

// Имена по роли, а не по адресу (0x63 и 0x67); одно определение на
// прошивку.
inline constexpr uint8_t PORT_CMD = SOUNDSINTH_BUS_PORT_CMD;
inline constexpr uint8_t PORT_DAT = SOUNDSINTH_BUS_PORT_DAT;
inline constexpr uint PIN_A8 = PIN_A0 + 8; // GPIO 16

// Размеры таблиц здесь: на них ссылаются границы массивов в объявлениях
// ниже.
// Варианты таблицы (PAGE_TAB_VARIANTS, bus.h) заготовлены заранее. Смена
// банка приходит записью в порт 0xE3, и следующая команда процессора может
// тут же читать из нового банка - срок порядка полумикросекунды. Перезапись региона в таблице - 64 записи в память;
// замер: 196 тактов уже на 32 записях при бюджете 252. Не успеть.
//
// Заготовленные варианты сводят любую смену состояния к одному слову в
// очередь детектора. Цена - 64 варианта по 512 байт = 32 КБ SRAM (48
// нужных, номер по чётности - ROM_BLK_N).
//
// Даже вариант "подстановки нет" зависит от банка: окно BDI 0x3D00-0x3DFF
// отдаётся нам по выборке команды и при снятой подстановке, а лежит оно в
// текущем банке.
//
// Половины варианта по M1: выборка команды и чтение данных.
inline constexpr uint32_t PAGE_TAB_HALVES = 2;
inline constexpr uint32_t kFetchHalf = 0;
inline constexpr uint32_t kDataHalf = 1;
// Страниц по 256 байт на 0x0000-0x3FFF и в регионе 8 КБ. divmmc оперирует
// регионами; гранула таблицы мельче, чтобы отделить окно 0x3Dxx от
// знакогенератора.
inline constexpr uint32_t PAGES_PER_TABLE = 0x4000u / 256u;
inline constexpr uint32_t PAGES_PER_REGION = 32;
inline constexpr uint32_t PAGE_TAB_REGIONS = PAGES_PER_TABLE / PAGES_PER_REGION;
// Запись таблицы: (база страницы >> 8) << 1 | ROM_BLK_N. Своя страница -
// разряд ноль: склейщик гасит им ПЗУ машины в этом же цикле (окно 3Dxx без
// подстановки). Чужая - только единица: ПЗУ машины остаётся, база ноль.
inline constexpr uint32_t kPageNotOurs = 1u;
// Карты трапов (trap_join): по адресу 0x0000-0x1FFF, по странице 0x2000-0x3FFF.
inline constexpr uint32_t kTrapPageMapEntries = 0x2000u / 256u;
constexpr uint32_t page_entry(uint32_t base_shr8) { return base_shr8 << 1; }
// --- 2. Состояние монтажа ---
//
// Определения в bus.cpp, рядом с логикой, которая их читает; здесь
// объявления.

// Плагинная шина
//
// Слово ответа для z80_rd_fsm: [31:24] = 0xFF в pindirs, [23:16] = байт в
// pins, [15:8] = 0x00 обратно в pindirs. Старший байт всегда 0xFF, поэтому
// слово ненулевое и при нулевом байте: отдельный маркер "порт наш" не
// нужен, хватает jmp !y в автомате.

// Номер порта из адреса записи таблицы на 256 слов, выровненной на 1024:
// младшие десять бит адреса - порт, умноженный на четыре.
__force_inline uint8_t table_index(uint32_t entry_addr) {
    return static_cast<uint8_t>((entry_addr >> 2) & 0xFFu);
}

constexpr uint32_t encode_rd_word(uint8_t byte) {
    return (0xFFu << 24) | (static_cast<uint32_t>(byte) << 16);
}
// Байт ответа из слова encode_rd_word.
constexpr uint8_t rd_word_byte(uint32_t word) {
    return static_cast<uint8_t>((word >> 16) & 0xFFu);
}

// Обработчики записи в порты плагина и приём кадра: определены в bus.cpp,
// расставляются по таблицам здесь.
void __not_in_flash_func(arm_frame)(void* user, uint8_t code, const uint8_t* args, uint8_t n);
void __not_in_flash_func(hide_frame)(void* user);
void __not_in_flash_func(port_write_cmd)(uint8_t port, uint8_t data);
void __not_in_flash_func(port_write_dat)(uint8_t port, uint8_t data);

extern player::protocol::HostProtocol* s_protocol;
extern PortWriteFn s_wr_table[256];
extern PortReadFn s_rd_done_table[256];

// Чтение ПЗУ: таблицы страниц и каналы двух уровней
extern uint32_t s_pagetabs[PAGE_TAB_VARIANTS][PAGE_TAB_HALVES][PAGES_PER_TABLE];
extern uint32_t s_page_variant;
extern int s_dma_tab_addr;
extern int s_dma_tab_data;
extern int s_dma_byte_addr;
extern int s_dma_byte_data;

// Порты: таблица записей, склейщик ячейки ответа и три канала. Нулевую
// запись склейщик отбрасывает - так решается "порт не наш". Байт выдаёт
// канал чтения памяти: он общий, циклы памяти и портов не пересекаются.
extern uint32_t s_porttab[256];
extern int s_dma_port_addr;
extern int s_dma_port_ptr;
extern int s_dma_port_join;
extern int s_sm_port_join;

// Трапы: карты, слова варианта, склейщик в pio2 и четыре канала.
inline uint32_t* const s_trap_addr_map = reinterpret_cast<uint32_t*>(kTrapAddrMapAt);
extern uint32_t s_trap_page_map[kTrapPageMapEntries];
extern uint32_t s_trap_word[2];
extern int s_sm_trap_join;
extern int s_dma_trap_in;
extern int s_dma_trap_addr;
extern int s_dma_trap_entry;
extern int s_dma_trap_word;
extern int s_dma_trap_note;
extern volatile uint32_t s_trap_last;

// --- 3. Обработчики прерываний ---
//
// Определены в bus.cpp, регистрируются в bus_setup.cpp. Секцию вешает
// определение, __not_in_flash_func в объявлении - для единообразия.

void __not_in_flash_func(bus_wr_isr)();
void __not_in_flash_func(port_rd_isr)();

} // namespace bus
