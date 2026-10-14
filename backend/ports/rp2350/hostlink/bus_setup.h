// SPDX-License-Identifier: MIT
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
//   0. Внутренний интерфейс - разбор слова записи, таблица страниц, карта
//      трапов, порты плагина в режиме DivMMC: зовут только bus.cpp,
//      bus_setup.cpp и divmmc.cpp.
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

#include "player/protocol/host_protocol.h"

namespace bus {

// --- 0. Внутренний интерфейс шины ---
//
// Остальные файлы порта его не видят: подстановку ведёт только divmmc.

// Поля снимка порта ввода (gpio_get_all, слово bus_wr): адрес A0..A15, его
// младший байт (номер порта) и данные D0..D7.
__force_inline uint16_t addr_of(uint32_t gpio) {
    return static_cast<uint16_t>((gpio >> PIN_A0) & 0xffffu);
}
__force_inline uint8_t port_of(uint32_t gpio) {
    return static_cast<uint8_t>((gpio >> PIN_A0) & 0xffu);
}
__force_inline uint8_t data_of(uint32_t gpio) {
    return static_cast<uint8_t>((gpio >> PIN_D0) & 0xffu);
}
// Запись в память, а не в порт: в цикле памяти IORQ_N высокий.
__force_inline bool is_memory_write(uint32_t gpio) {
    return (gpio & (1u << PIN_IORQ_N)) != 0u;
}

// Дождаться конца идущего цикла чтения памяти (MREQ_N и RD_N оба низкие).
// Смена подстановки DivMMC - вариант таблицы и ROM_BLK - действует со
// следующего цикла: смена варианта посреди выборки гасит ПЗУ машины, пока
// Z80 читает из неё (вход по 0x0038), или включает её навстречу нашему
// байту (выход по 1FF8). Без ожидания esxDOS грузится с мусором на экране и встаёт.
//
// Предел - 128 проходов по 6 тактов, 2.5 мкс: цикл M1 держит RD 430 нс,
// дольше чтение не бывает.
inline constexpr uint32_t kWaitReadEndLimit = 128u;
static __force_inline void wait_memory_read_end() {
    constexpr uint32_t kReadMask = (1u << PIN_MREQ_N) | (1u << PIN_RD_N);
    for (uint32_t n = 0; n < kWaitReadEndLimit && (gpio_get_all() & kReadMask) == 0u; ++n) {
        tight_loop_contents();
    }
}

// Варианты таблицы страниц: подстановка и банк, 16 банков ОЗУ DivMMC на
// три группы (раскладка - у variant_of). Номер варианта - младший разряд
// базы в X детектора, он же ROM_BLK_N: подстановка - чётные номера, без неё -
// нечётные. Групп три, номеров вчетверо больше банков: нечётных на
// шестнадцать больше нужного.
inline constexpr uint32_t PAGE_TAB_BANKS    = 16;
inline constexpr uint32_t PAGE_TAB_VARIANTS = 4 * PAGE_TAB_BANKS;

// Заполнить регион в заданном варианте таблицы. Варианты заготовлены,
// чтобы переключение подстановки стоило одной записи регистра.
void rom_emu_fill_pages(uint32_t variant, uint32_t region, const void* page);

// Одна 256-байтовая страница в половине "выборка команды": для окна BDI
// 0x3D00-0x3DFF, у которого вход немедленный, а чтение данных остаётся
// машине ради знакогенератора.
void rom_emu_set_fetch_page(uint32_t variant, uint32_t page, const void* mem);

// Выбрать вариант таблицы страниц 0..PAGE_TAB_VARIANTS-1.
void rom_emu_select_pages(uint32_t variant);
// То же без сверки с запомненным вариантом: слово в очередь детектора
// кладётся всегда.
// Для повтора после трапа, вклинившегося в смену варианта.
void rom_emu_force_pages(uint32_t variant);

// Трапы DivMMC - без ядра, цепочкой DMA по карте: ниже 0x2000 по адресу,
// 0x2000-0x3FFF по странице 256 байт. Сработавший трап кладёт в очередь
// детектора слово варианта "войти" или "выйти"; действует со следующего
// чтения памяти.
// Карта трапов по адресу (0x0000-0x1FFF) двухуровневая. Первый уровень - по
// младшему байту адреса (его склейщик получает очередью от rom_trap),
// второй - по A8..A12 с пинов. Подтаблица заводится только младшему байту,
// у которого есть трап.
//
// Запись первого уровня: ноль - трапов с таким младшим байтом нет; иначе
// (база подтаблицы >> TRAP_SUB_BASE_SHIFT) << 1 | 1, где младший бит -
// маршрут. Разбирает его второй автомат PIO (trap_route).
inline constexpr uint32_t kTrapAddrSpan   = 0x2000u; // область поадресных трапов
inline constexpr uint32_t kTrapLoEntries  = 256;     // первый уровень, индекс A0..A7
inline constexpr uint32_t kTrapSubEntries = 32;      // подтаблица, индекс A8..A12
inline constexpr uint32_t kTrapSubTables  = 24;      // нужно 14, прочее - запас
enum class TrapKind : uint8_t { Enter = 0, Exit = 1 };
void rom_emu_trap_clear();
// addr ниже 0x2000 - точка; 0x2000-0x3FFF - вся страница адреса.
void rom_emu_trap_set(uint16_t addr, TrapKind kind);
// Тот же путь, что проходит железо: указатель на слово варианта или ноль.
// Для самопроверки раскладки на загрузке.
uint32_t rom_emu_trap_resolve(uint16_t addr);
// Варианты, которые кладут трапы: ставить при каждой смене банка, MAPRAM,
// CONMEM - до того, как класть вариант самому.
void rom_emu_trap_variants(uint32_t enter_variant, uint32_t exit_variant);
// Последний сработавший трап - вход (подстановка по трапу стоит).
bool rom_emu_trap_entered();
// Без трапа: как после выхода.
void rom_emu_trap_reset_state();

// Наибольшее число записей, разобранных обработчиком записей за один
// вызов (очередь PIO - 8 слов). Близко к восьми - обработчик держат.
uint32_t bus_wr_peak();

// Порты плагина через путь портов эмуляции ПЗУ: в режиме DivMMC плагинные
// автоматы класть некуда.
void dispatch_port_write(uint8_t port, uint8_t data);
void dispatch_port_read_done(uint8_t port);
// Адрес записи таблицы страниц, прочитанной последней: последнее чтение
// памяти ниже 0x4000, выборка или данные. Меняется, пока Z80 что-то
// исполняет; не меняется - встал.
uint32_t last_memory_read_ptr();

// --- 1. Раскладка ---
//
// Блоки PIO, номера автоматов, каналы DMA и приоритеты прерываний - в
// hw_config.h. Здесь номера портов протокола, пины, производные от PIN_A0,
// и размеры таблиц, на которые ссылаются границы массивов ниже.

// Имена по роли, а не по адресу (0x63 и 0x67); одно определение на
// прошивку.
inline constexpr uint8_t PORT_CMD = SOUNDSINTH_BUS_PORT_CMD;
inline constexpr uint8_t PORT_DAT = SOUNDSINTH_BUS_PORT_DAT;
inline constexpr uint PIN_A8      = PIN_A0 + 8; // GPIO 16

// Размеры таблиц здесь: на них ссылаются границы массивов в объявлениях
// ниже.
// Варианты таблицы заготовлены заранее. Смена
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
inline constexpr uint32_t kFetchHalf      = 0;
inline constexpr uint32_t kDataHalf       = 1;
// Страниц по 256 байт на 0x0000-0x3FFF и в регионе 8 КБ. divmmc оперирует
// регионами; гранула таблицы мельче, чтобы отделить окно 0x3Dxx от
// знакогенератора.
inline constexpr uint32_t PAGES_PER_TABLE  = 0x4000u / 256u;
inline constexpr uint32_t PAGES_PER_REGION = 32;
inline constexpr uint32_t PAGE_TAB_REGIONS = PAGES_PER_TABLE / PAGES_PER_REGION;
// Запись таблицы: (база страницы >> 8) << 1 | ROM_BLK_N. Своя страница -
// разряд ноль: склейщик гасит им ПЗУ машины в этом же цикле (окно 3Dxx без
// подстановки). Чужая - только единица: ПЗУ машины остаётся, база ноль.
inline constexpr uint32_t kPageNotOurs = 1u;
// Карты трапов (trap_join): по адресу 0x0000-0x1FFF, по странице 0x2000-0x3FFF.
inline constexpr uint32_t kTrapPageMapEntries = 0x2000u / 256u;
constexpr uint32_t page_entry(uint32_t base_shr8) {
    return base_shr8 << 1;
}

// Адрес в SRAM как 32-битное слово: так его читают склейщики и каналы DMA.
__force_inline uint32_t addr32(const volatile void* p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}
// --- 2. Состояние монтажа ---
//
// Определения в bus.cpp, рядом с логикой, которая их читает; здесь
// объявления.

// Плагинная шина
//
// Слово ответа кадра: байт в разрядах 23:16. На шину уходит только байт
// (set_rd_word), "порт наш" решает запись s_porttab.

// Номер порта из адреса записи таблицы на 256 слов, выровненной на 1024:
// младшие десять бит адреса - порт, умноженный на четыре.
__force_inline uint8_t table_index(uint32_t entry_addr) {
    return static_cast<uint8_t>((entry_addr >> 2) & 0xffu);
}

constexpr uint32_t encode_rd_word(uint8_t byte) {
    return (0xffu << 24) | (static_cast<uint32_t>(byte) << 16);
}
// Байт ответа из слова encode_rd_word.
constexpr uint8_t rd_word_byte(uint32_t word) {
    return static_cast<uint8_t>((word >> 16) & 0xffu);
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
extern int s_sm_trdos_drive;
extern int s_trdos_detect_offset; // смещение программы детектора TR-DOS, -1 - не заведён
extern int s_sm_reset_watch;      // автомат reset_watch, -1 - не заведён

// Нечётные варианты таблицы страниц старше 2 * PAGE_TAB_BANKS DivMMC не
// выбирает никогда (variant_of): 16 кусков по 512 Б, каждый выровнен на 512.
// В них лежат подтаблицы трапов (по четыре на кусок) и карта страниц
// трапов: им нужно выравнивание 128, и отдельные массивы стоили бы 3.2 КБ.
inline constexpr uint32_t kPageTabHoleFirst = 2u * PAGE_TAB_BANKS + 1u;
inline constexpr uint32_t kPageTabHoles     = (PAGE_TAB_VARIANTS - kPageTabHoleFirst + 1u) / 2u;
inline constexpr uint32_t kPageTabHoleWords = PAGE_TAB_HALVES * PAGES_PER_TABLE;
inline constexpr uint32_t kTrapSubsPerHole  = kPageTabHoleWords / kTrapSubEntries;
inline constexpr uint32_t kTrapPageMapHole  = kTrapSubTables / kTrapSubsPerHole;
static_assert(kTrapSubTables % kTrapSubsPerHole == 0u, "trap sub-tables fill whole page table holes");
static_assert(kTrapPageMapHole < kPageTabHoles, "trap sub-tables and the trap page map fit the page table holes");
static_assert(kTrapPageMapEntries <= kPageTabHoleWords, "the trap page map fits one page table hole");

constexpr bool pagetab_is_hole(uint32_t variant) {
    return variant >= kPageTabHoleFirst && (variant & 1u) != 0u;
}
inline uint32_t* pagetab_hole(uint32_t k) {
    return &s_pagetabs[kPageTabHoleFirst + 2u * k][0][0];
}

// Трапы: карты, слова варианта, склейщик в pio2 и четыре канала.
// Первый уровень - объект .bss с выравниванием: так его размер виден
// линкеру и size. Подтаблицы и карта страниц - в дырах таблицы страниц.
extern uint32_t s_trap_lo_storage[kTrapLoEntries];
inline uint32_t* const s_trap_lo = s_trap_lo_storage;
inline uint32_t* trap_sub(uint32_t i) {
    return pagetab_hole(i / kTrapSubsPerHole) + (i % kTrapSubsPerHole) * kTrapSubEntries;
}
inline uint32_t* trap_page_map() {
    return pagetab_hole(kTrapPageMapHole);
}
extern uint32_t s_trap_word[2];
// Ячейки с указателями на слова: через них проходит путь страницы, чтобы
// обе ветви кончались одной ступенью "адрес ячейки -> указатель -> слово".
extern uint32_t s_trap_word_ptr[2];
extern int s_sm_trap_join;  // первый уровень и путь страницы
extern int s_sm_trap_route; // маршрут и второй уровень
extern int s_dma_trap_in;
extern int s_dma_trap_addr;
extern int s_dma_trap_entry;
extern int s_dma_trap_addr2;
extern int s_dma_trap_entry2;
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
