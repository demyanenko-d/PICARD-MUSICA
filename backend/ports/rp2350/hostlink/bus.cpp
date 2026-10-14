// SPDX-License-Identifier: MIT
// Реализация bus.h. Разделы в том же порядке: пины, плагинная шина,
// эмуляция ПЗУ.

#include "bus_setup.h"

#include <atomic>
#include <cinttypes>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico.h"

#include "divmmc.h"
#include "platform/log.h"
#include "player/live/ay_tap.h"
#include "core/live_midi/ay_midi.h"
#include "memory.pio.h"      // PAGE_TABLE_BASE_SHIFT
#include "ports.pio.h"       // PORT_TABLE_BASE_SHIFT
#include "trap_join.pio.h"   // TRAP_LO_BASE_SHIFT, TRAP_SUB_BASE_SHIFT
#include "reset_watch.pio.h" // IRQ_ROM48, IRQ_PAGING_LOCKED
#include "trdos.pio.h"       // trdos_detect_offset_asserted
#include "player/protocol/host_frame.h"

namespace bus {

// --- Пины и режим ---
//

// Анонимного пространства имён нет: монтаж в bus_setup.cpp, регистры,
// таблицы и обработчики должны быть видны оттуда. Имена в namespace bus с
// префиксом s_ или k.

// Режим ставится один раз при загрузке, дальше все спрашивают флаг: на
// ходу он не меняется. Определение - рядом с таблицами ответов (плагинная
// шина ниже): set_rd_word читает флаг от их общей базы.
extern bool s_rom_emu;
extern bool s_trdos;
extern bool s_config_rom;

void rom_emu_set_enabled(bool on) {
    s_rom_emu = on;
}

void config_rom_set_enabled(bool on) {
    s_config_rom = on;
}

bool config_rom_enabled() {
    return s_config_rom;
}

bool rom_emu_enabled() {
    return s_rom_emu;
}

namespace {
bool s_reset_watch = false;
} // namespace

void reset_watch_set_enabled(bool on) {
    s_reset_watch = on;
}

bool reset_watch_enabled() {
    return s_reset_watch;
}

void trdos_set_enabled(bool on) {
    s_trdos = on;
}

bool trdos_enabled() {
    return s_trdos;
}

// --- Плагинная шина ---
// Всё на пути байта шины - в SRAM (__not_in_flash_func). Флеш и PSRAM на
// одном QMI: при догрузке сэмплов промах кэша XIP в обработчике длится до
// 6.8 мкс против 0.3 мкс работы, и RX FIFO записи переполняется.

player::protocol::HostProtocol* s_protocol = nullptr;

// Определение ниже: байт слова ответа - в ячейку ответа порта.
__force_inline void set_rd_word(uint8_t port, uint32_t word);

PioStats s_stats;

// --- Вооружённый кадр ---
//
// Протокол отдаёт команду целиком (код и аргументы), слова ответа на все
// её байты кодируются один раз (host_frame.h). Дальше ISR чтения протокол
// не спрашивает: сдвинула индекс, положила готовое слово.
uint32_t s_frame_words[player::protocol::host_frame::kWords];
volatile uint8_t s_frame_pos = 0; // двигает ISR чтения, обнуляет arm_frame

// Приёмник кадра: слово ответа - в таблицу и копию пути эмуляции ПЗУ.
struct FrameSink {
    static constexpr uint8_t kCmd = PORT_CMD;
    static constexpr uint8_t kDat = PORT_DAT;
    __force_inline void set(uint8_t port, uint32_t word) { set_rd_word(port, word); }
    __force_inline uint32_t encode(uint8_t byte) { return encode_rd_word(byte); }
    // Статус - последним и для ядра, и для DMA: барьер компилятора не даёт
    // переставить записи, dmb дожидается их до записи статуса. Публикация -
    // одна 32-битная запись, без блокировок и запрета прерываний.
    __force_inline void publish_fence() {
        std::atomic_signal_fence(std::memory_order_release);
        __dmb();
    }
};

void __not_in_flash_func(arm_frame)(void*, uint8_t code, const uint8_t* args, uint8_t n) {
    FrameSink sink;
    player::protocol::host_frame::arm(sink, s_frame_words, s_frame_pos, code, args, n);
}

// Байт команды хоста пришёл во время вооружения: команду протокол вернёт
// после разбора кадра хоста.
void __not_in_flash_func(hide_frame)(void*) {
    FrameSink sink;
    player::protocol::host_frame::hide(sink);
}

PortWriteFn s_wr_table[256];

// Хуки "чтение состоялось" - только для портов, зарегистрированных
// снаружи (bus.h). Для порта данных своя ветка в ISR, чтобы не удлинять
// самый горячий путь.
PortReadFn s_rd_done_table[256];

// Эмуляция ПЗУ поднята (DivMMC или конфигуратор). Пишет rom_emu_set_enabled
// при загрузке, из настроек.
bool s_rom_emu = false;
// Триггер TR-DOS включён. Вместе с DivMMC не бывает: дисковая система одна.
bool s_trdos = false;
// Режим конфигуратора: вместо образа DivMMC подставляется ПЗУ настроек, и
// подстановка держится с первой выборки, не дожидаясь трапа.
bool s_config_rom = false;

// Единственное место, где слово ответа попадает на шину: байт - в ячейку
// ответа порта (путь портов один на оба режима).
__force_inline void __not_in_flash_func(set_rd_word)(uint8_t port, uint32_t word) {
    port_answer(port, rd_word_byte(word));
}

// Разбор записи в порт - один на оба режима: bus_wr_isr без DivMMC,
// divmmc_on_bus_write для чужих ей портов в режиме DivMMC. Счёт команд и
// данных плагина здесь же, один набор на оба режима. Счётчики после
// обработки: Z80 на OUT ответа не ждёт.
void __not_in_flash_func(dispatch_port_write)(uint8_t port, uint8_t data) {
    const PortWriteFn fn = s_wr_table[port];
    if (fn != nullptr) fn(port, data);
    if (port == PORT_CMD) {
        ++s_stats.cmd_bytes;
    } else if (port == PORT_DAT) {
        ++s_stats.dat_bytes;
    }
}

// --- Обработчики портов плагина - тонкие обёртки для ISR ---

void __not_in_flash_func(port_write_cmd)(uint8_t, uint8_t data) {
    FrameSink sink;
    player::protocol::host_frame::command_byte(sink, s_protocol, data);
}
void __not_in_flash_func(port_write_dat)(uint8_t, uint8_t data) {
    s_protocol->on_data_byte(data);
}

volatile uint32_t s_host_status_read_us = 0;

uint32_t host_status_read_us() {
    return s_host_status_read_us;
}

// Чтение порта состоялось - общее тело для обоих путей. Кадр порта данных
// продвигается здесь: путь портов эмуляции ПЗУ зовёт только эту функцию,
// мимо s_rd_done_table.
void __not_in_flash_func(dispatch_port_read_done)(uint8_t addr) {
    // Клиент хоста в своём цикле всё время читает порт состояния: тишина на
    // нём - хост ушёл из плеера.
    if (addr == PORT_CMD) s_host_status_read_us = timer_hw->timerawl;
    if (addr != PORT_DAT) {
        const PortReadFn done = s_rd_done_table[addr];
        if (done) done(addr);
        return;
    }
    FrameSink sink;
    player::protocol::host_frame::read_done(sink, s_frame_words, s_frame_pos);
}

void port_on_write(uint8_t port, PortWriteFn fn) {
    s_wr_table[port] = fn;
}

// В SRAM: вызывается из обоих обработчиков на каждом байте эмулируемой
// карты; во флеше - промах кэша XIP, а горячий путь должен укладываться в
// несколько микросекунд между командами IN/OUT Z80.
void __not_in_flash_func(port_set_read)(uint8_t port, uint8_t value) {
    set_rd_word(port, encode_rd_word(value));
}

void port_on_read_done(uint8_t port, PortReadFn fn) {
    s_rd_done_table[port] = fn;
}

PioStats bus_stats() {
    return s_stats;
}

// --- Эмуляция ПЗУ и памяти ---
// Детекторы, цепочки DMA, трапы, порты.

// Таблица страниц [вариант][M1_N][страница 256 байт]: база страницы >> 8
// или ноль (не наше); заполняется при сбросе, в работе меняется только
// вариант. Половина по M1 - ради окна 0x3D00-0x3DFF: выборка команды
// оттуда наша, чтение данных (знакогенератор) - машины.
//
// Выровнено на 512 байт (вариант): адрес собирается склейкой (in x,23 /
// in y,1 / in pins,6 / in null,2).
static_assert(sizeof(uint32_t) * PAGE_TAB_HALVES * PAGES_PER_TABLE == 1u << PAGE_TABLE_BASE_SHIFT,
              "the page table variant is exactly the address window rom_detect assembles");
// Выравнивание вдвое больше варианта: младший разряд базы в X - номер
// варианта по чётности, он же ROM_BLK_N (rom_detect, mov pins, x).
alignas(2u << PAGE_TABLE_BASE_SHIFT) __attribute__((section(".bss.bus_pagetabs"))) uint32_t s_pagetabs[PAGE_TAB_VARIANTS][PAGE_TAB_HALVES][PAGES_PER_TABLE];
uint32_t s_page_variant = 0;

// Откуда брать байт ответа на порт, ноль - порт не наш. Старший разряд
// записи - маршрут склейщика: ноль значит готовый адрес ячейки, единица -
// база страницы на 256 байт, сдвинутая вправо на 8. Адреса SRAM младше
// 2^31, разряд свободен.
//
// Таблица выровнена на 1024: адрес записи собирается склейкой в детекторе
// (in x,22 / in pins,8 / in null,2).
alignas(1u << PORT_TABLE_BASE_SHIFT) __attribute__((section(".bss.bus_porttab"))) uint32_t s_porttab[256];

// Ячейки ответов, отдельно от таблицы: значение пишется сюда, разрешение
// отвечать - указателем в таблице.
uint32_t s_portval[256];

int s_dma_port_addr       = -1; // rxf детектора портов -> read_addr следующего
int s_dma_port_ptr        = -1; // porttab[порт] -> al3_read_addr_trig канала выдачи ответа
int s_dma_port_join       = -1; // rxf склейщика -> al3_read_addr_trig канала выдачи байта
int s_sm_port_join        = -1; // склейщик ячейки ответа, в блоке звука
int s_sm_trdos_drive      = -1; // выдатчик уровня DOS_N, там же
int s_trdos_detect_offset = -1;
int s_sm_reset_watch      = -1;

int s_dma_tab_addr  = -1; // rxf детектора -> read_addr следующего
int s_dma_tab_data  = -1; // pagetab[регион] -> txf rom_join
int s_dma_byte_addr = -1; // rxf rom_join -> read_addr следующего
int s_dma_byte_data = -1; // страница[смещение] -> txf rom_serve

// Трапы DivMMC без ядра: rom_trap (pio0) -> канал -> склейщик trap_join
// (pio2) -> пара каналов читает запись карты -> канал слова варианта в
// очередь детектора. Запись карты - указатель на слово "войти" или
// "выйти"; ноль - пустой триггер, цепочка стоит. Карта страниц
// 0x2000-0x3FFF - trap_page_map(), в дыре таблицы страниц.
uint32_t s_trap_word[2];     // слова варианта: [0] войти, [1] выйти - ставит divmmc
uint32_t s_trap_word_ptr[2]; // ячейки с указателями на них - для пути страницы
int s_sm_trap_join    = -1;
int s_sm_trap_route   = -1;
int s_dma_trap_in     = -1; // rom_trap rxf -> склейщик txf
int s_dma_trap_addr   = -1; // склейщик rxf -> read_addr канала первого уровня
int s_dma_trap_entry  = -1; // запись первого уровня -> txf разборщика маршрута
int s_dma_trap_addr2  = -1; // разборщик rxf -> read_addr канала указателя
int s_dma_trap_entry2 = -1; // указатель -> al3_read_addr_trig канала слова
int s_dma_trap_word   = -1; // слово варианта -> txf детектора
int s_dma_trap_note   = -1; // адрес отданного слова -> s_trap_last (после word)
// Адрес последнего отданного слова: вход или выход. Пустой триггер пишет
// ноль в read_addr канала слова, поэтому состояние - не там, а здесь: его
// копирует канал, которого запускает только настоящий трап.
volatile uint32_t s_trap_last = 0;

// Наибольшее число слов, разобранных обработчиком записей за один вызов.
// Пишет bus_wr_isr, читает log_task.
uint32_t s_bus_wr_peak = 0;
uint32_t bus_wr_peak() {
    return s_bus_wr_peak;
}

// Запись в порты AY (#FFFD выбор регистра, #BFFD данные: A15 = 1, младший
// байт #FD, A14 различает) - в кольцо живого MIDI, разбор на Core0.
constexpr uint16_t kAyPortMask   = 0x80ffu;
constexpr uint16_t kAyPortMatch  = 0x80fdu;
constexpr uint16_t kAyDataSelect = 0x4000u; // A14: ноль - данные, единица - выбор регистра
__force_inline void tap_ay_write(uint32_t raw) {
    if (is_memory_write(raw)) return;
    const uint16_t addr = addr_of(raw);
    if ((addr & kAyPortMask) != kAyPortMatch) return;
    const uint16_t sel = (addr & kAyDataSelect) ? soundsinth::midi_in::kAyWriteSelect : 0u;
    player::live::ay_tap_push(static_cast<uint16_t>(sel | data_of(raw)));
}

// Запись в порт 0x7FFD (A15 = 0, A1 = 0) - для триггера TR-DOS: бит 4
// выбирает ПЗУ 48, бит 5 закрывает порт до сброса машины. Оба держат флаги
// IRQ блока триггера, гасит их сторож сброса.
constexpr uint16_t kPagingPortMask = 0x8002u;
constexpr uint8_t kPagingRom48     = 0x10u;
constexpr uint8_t kPagingLock      = 0x20u;
__force_inline void tap_paging_write(uint32_t raw) {
    if (s_trdos_detect_offset < 0 || is_memory_write(raw)) return;
    if ((addr_of(raw) & kPagingPortMask) != 0u) return;
    if ((PIO_DETECT->irq & (1u << IRQ_PAGING_LOCKED)) != 0u) return;
    const uint8_t data = data_of(raw);
    if ((data & kPagingRom48) != 0u) {
        PIO_DETECT->irq_force = 1u << IRQ_ROM48;
    } else {
        PIO_DETECT->irq = 1u << IRQ_ROM48;
    }
    if ((data & kPagingLock) != 0u) PIO_DETECT->irq_force = 1u << IRQ_PAGING_LOCKED;
}

void __not_in_flash_func(bus_wr_isr)() {
    // Флаг снимается до разбора, иначе запись, пришедшая между чтением
    // очереди и снятием флага, потеряла бы пробуждение.
    pio_interrupt_clear(PIO_WATCH, 0);

    // Без DivMMC автомат ловит только записи в порт: разбор - сразу по
    // таблице обработчиков.
    //
    // Цикл, а не по слову: между двумя записями Z80 бывает меньше времени,
    // чем вход в прерывание.
    uint32_t drained = 0;
    while (!pio_sm_is_rx_fifo_empty(PIO_WATCH, SM_BUS_WRITE)) {
        const uint32_t raw = pio_sm_get(PIO_WATCH, SM_BUS_WRITE);
        tap_ay_write(raw);
        tap_paging_write(raw);
        if (s_rom_emu) {
            divmmc_on_bus_write(raw);
        } else {
            dispatch_port_write(port_of(raw), data_of(raw));
        }
        ++drained;
    }
    // Пик очереди за вызов: близко к восьми - обработчик не успевает, что-то
    // его держит.
    if (drained > s_bus_wr_peak) s_bus_wr_peak = drained;
    if (drained > s_stats.wr_fifo_max_level) s_stats.wr_fifo_max_level = static_cast<uint8_t>(drained);

    // RXSTALL - push упёрся в полную очередь, запись потеряна. Молчать
    // нельзя: страница разъехалась бы с тем, что записал процессор.
    const uint32_t stall = 1u << (PIO_FDEBUG_RXSTALL_LSB + SM_BUS_WRITE);
    if (PIO_WATCH->fdebug & stall) {
        PIO_WATCH->fdebug = stall;
        ++s_stats.wr_fifo_overruns;
#if SOUNDSINTH_DIVMMC_TRACE
        if (s_rom_emu) divmmc_note_lost(drained);
#endif
    }
}

// --- Замеры шины ---
//
// Случай, которого по остальным счётчикам не видно.

// --- Порты на чтение в режиме эмуляции ПЗУ ---
//
// Детектор портов рядом с детектором памяти отдаёт байт тому же rom_serve;
// циклы MREQ и IORQ у Z80 взаимно исключены, цепочки в одной его очереди
// не сталкиваются. Цепочка: детектор -> указатель из s_porttab -> триггер
// канала выдачи ответа -> байт в rom_serve.
//
// Прерывание включает setup_ports, после divmmc_reset: состояние карты к
// первому чтению готово.
void __not_in_flash_func(port_rd_isr)() {
    dma_irqn_acknowledge_channel(DMA_IRQ_INDEX_PORT_RD, s_dma_port_join);
    // Порт восстанавливается из канала указателей: там адрес записи таблицы,
    // по которой только что читали. Таблица выровнена на 1024, младшие десять
    // бит - номер порта, умноженный на четыре.
    //
    // Спешить некуда: байт ушёл на шину переносом DMA задолго до входа сюда.
    // Обработчик подставляет следующий байт (у эмулятора карты - очередной
    // байт SPI).
    const uint8_t port = table_index(dma_hw->ch[s_dma_port_ptr].read_addr);
    if (s_rom_emu) {
        divmmc_on_port_read(port);
    } else {
        dispatch_port_read_done(port);
    }
}

// Зовётся только из build_pagetabs при сбросе - потому и во флеше, а не в
// SRAM. Завести вызов с пути шины нельзя: там флеша быть не должно.
// Область - регион 8 КБ, как у divmmc; гранула 256 байт раскладывается
// здесь. На пути трапа таблицы не трогаются: меняется вариант, одно слово
// в регистре детектора.
void rom_emu_fill_pages(uint32_t variant, uint32_t region, const void* page) {
    // Регионов в таблице два: она покрывает только 0x0000-0x3FFF.
    if (variant >= PAGE_TAB_VARIANTS || region >= PAGE_TAB_REGIONS) return;
    // kPageNotOurs - "страница не наша": X = 0, rom_join обрывает цикл по
    // jmp !x, на шину никто не выходит. Указатель выровнен на 256 байт:
    // младшие 8 бит теряются.
    const uint32_t base = page == nullptr ? 0u : addr32(page) >> 8;
    static_assert(kPageNotOurs == 1u, "rom_join: bit 0 is ROM_BLK_N, X = 0 means the page is not ours");
    const uint32_t first = region * PAGES_PER_REGION;
    for (uint32_t i = 0; i < PAGES_PER_REGION; ++i) {
        // Каждой 256-байтовой странице своя база: одна на все 32 означала бы, что
        // весь регион читается из первых 256 байт.
        const uint32_t e                           = (base == 0u) ? kPageNotOurs : page_entry(base + i);
        s_pagetabs[variant][kFetchHalf][first + i] = e;
        s_pagetabs[variant][kDataHalf][first + i]  = e;
    }
}

// Одна страница в одной половине - ради неё гранула и мельче.
//
// Окно BDI 0x3D00-0x3DFF отвечает нашей памятью на выборку команды и без
// подстановки: у divIDE вход через 3Dxx немедленный, а не отложенный на
// команду, как у остальных точек входа.
//
// Чтение данных из того же окна остаётся машине: в тех же 8 КБ
// знакогенератор 0x3C00-0x3FFF, иначе пропал бы шрифт.
//
// Как и rom_emu_fill_pages, зовётся только при сбросе - потому во флеше.
void rom_emu_set_fetch_page(uint32_t variant, uint32_t page, const void* mem) {
    if (variant >= PAGE_TAB_VARIANTS || page >= PAGES_PER_TABLE) return;
    s_pagetabs[variant][kFetchHalf][page] = mem == nullptr ? kPageNotOurs : page_entry(addr32(mem) >> 8);
}

// База таблицы - слово в очередь детектора, X он переписывает сам в начале
// чтения памяти (rom_detect). Команды через INSTR не подсовываются: автомат
// в эту минуту может держать в OSR снимок шины.
//
// Очередь разбирается на каждом чтении памяти, у Z80 оно есть в каждой
// команде. Полная очередь - процессор стоит; слово сверху отбросилось бы
// молча, и после пуска остался бы не последний вариант.
namespace {
void __not_in_flash_func(queue_page_base)(uint32_t variant) {
    if (pio_sm_is_tx_fifo_full(PIO_DETECT, SM_ROM_DETECT)) {
        pio_sm_clear_fifos(PIO_DETECT, SM_ROM_DETECT);
    }
    pio_sm_put(PIO_DETECT, SM_ROM_DETECT, addr32(&s_pagetabs[variant][0][0]) >> PAGE_TABLE_BASE_SHIFT);
}
} // namespace

void __not_in_flash_func(rom_emu_select_pages)(uint32_t variant) {
    if (variant >= PAGE_TAB_VARIANTS || variant == s_page_variant) return;
    s_page_variant = variant;
    queue_page_base(variant);
}

void __not_in_flash_func(rom_emu_force_pages)(uint32_t variant) {
    if (variant >= PAGE_TAB_VARIANTS) return;
    s_page_variant = variant;
    queue_page_base(variant);
}

// Слово в очереди детектора - база варианта >> сдвиг (как queue_page_base).
namespace {
__force_inline uint32_t variant_word(uint32_t variant) {
    return addr32(&s_pagetabs[variant][0][0]) >> PAGE_TABLE_BASE_SHIFT;
}
} // namespace

namespace {
uint32_t s_trap_sub_used = 0; // сколько подтаблиц роздано

static_assert((1u << TRAP_LO_BASE_SHIFT) == kTrapLoEntries * sizeof(uint32_t), "TRAP_LO_BASE_SHIFT follows the size of trap level 1");
static_assert((1u << TRAP_SUB_BASE_SHIFT) == kTrapSubEntries * sizeof(uint32_t), "TRAP_SUB_BASE_SHIFT follows the size of a trap sub-table");

// Запись первого уровня - маршрут во второй: база подтаблицы без разрядов
// выравнивания, сдвинутая на бит, и единица маршрута в младшем разряде.
constexpr uint32_t trap_lo_route_entry(uint32_t sub_base) {
    return ((sub_base >> TRAP_SUB_BASE_SHIFT) << 1) | 1u;
}
// Адрес подтаблицы из записи первого уровня с маршрутом.
constexpr uint32_t trap_sub_of(uint32_t entry) {
    return (entry >> 1) << TRAP_SUB_BASE_SHIFT;
}
// Запись карты страниц: адрес ячейки, сдвинутый на бит; младший ноль -
// маршрут "насквозь".
constexpr uint32_t trap_page_entry(uint32_t cell) {
    return cell << 1;
}
} // namespace

void rom_emu_trap_clear() {
    for (uint32_t& e : s_trap_lo_storage) {
        e = 0u;
    }
    for (uint32_t i = 0; i < kTrapSubTables; ++i) {
        uint32_t* const sub = trap_sub(i);
        for (uint32_t e = 0; e < kTrapSubEntries; ++e) {
            sub[e] = 0u;
        }
    }
    uint32_t* const map = trap_page_map();
    for (uint32_t e = 0; e < kTrapPageMapEntries; ++e) {
        map[e] = 0u;
    }
    for (uint32_t k = 0; k < 2u; ++k) {
        s_trap_word_ptr[k] = addr32(&s_trap_word[k]);
    }
    s_trap_sub_used = 0;
}

void rom_emu_trap_set(uint16_t addr, TrapKind kind) {
    const auto k   = static_cast<uint32_t>(kind);
    const auto ptr = addr32(&s_trap_word[k]);
    if (addr < kTrapAddrSpan) {
        const uint32_t lo = addr & 0xffu;
        const uint32_t hi = (addr >> 8) & (kTrapSubEntries - 1u);
        uint32_t* sub     = nullptr;
        if (s_trap_lo_storage[lo] != 0u) {
            sub = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(trap_sub_of(s_trap_lo_storage[lo])));
        } else {
            if (s_trap_sub_used >= kTrapSubTables) {
                panic("rom_emu: trap sub-tables exhausted at 0x%04x", static_cast<unsigned>(addr));
            }
            sub = trap_sub(s_trap_sub_used++);
            // Маршрут в младшем бите: единица - идти во второй уровень.
            s_trap_lo_storage[lo] = trap_lo_route_entry(addr32(sub));
        }
        sub[hi] = ptr;
    } else if (addr < 0x4000u) {
        // Путь страницы проходит ту же ступень "адрес ячейки -> указатель",
        // поэтому в карте лежит адрес ЯЧЕЙКИ, сдвинутый влево: младший бит
        // ноль - маршрут "насквозь".
        trap_page_map()[(addr >> 8) & (kTrapPageMapEntries - 1u)] = trap_page_entry(addr32(&s_trap_word_ptr[k]));
    }
}

// Повторяет путь железа: первый уровень по младшему байту, маршрут в
// младшем бите, второй уровень по старшим; выше kTrapAddrSpan - карта
// страниц, адрес ячейки и указатель из неё. Нужна самопроверке - ошибка в
// раскладке должна найтись на загрузке, а не неверной выборкой у машины.
uint32_t rom_emu_trap_resolve(uint16_t addr) {
    if (addr >= 0x4000u) return 0u;
    if (addr >= kTrapAddrSpan) {
        const uint32_t pe = trap_page_map()[(addr >> 8) & (kTrapPageMapEntries - 1u)];
        if (pe == 0u) return 0u;
        return *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(pe >> 1));
    }
    const uint32_t e = s_trap_lo_storage[addr & 0xffu];
    if ((e & 1u) == 0u) return 0u;
    const auto* sub = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(trap_sub_of(e)));
    return sub[(addr >> 8) & (kTrapSubEntries - 1u)];
}

void __not_in_flash_func(rom_emu_trap_variants)(uint32_t enter_variant, uint32_t exit_variant) {
    s_trap_word[static_cast<uint32_t>(TrapKind::Enter)] = variant_word(enter_variant);
    s_trap_word[static_cast<uint32_t>(TrapKind::Exit)]  = variant_word(exit_variant);
}

bool __not_in_flash_func(rom_emu_trap_entered)() {
    return s_trap_last == addr32(&s_trap_word[static_cast<uint32_t>(TrapKind::Enter)]);
}

void rom_emu_trap_reset_state() {
    s_trap_last = addr32(&s_trap_word[static_cast<uint32_t>(TrapKind::Exit)]);
}

void __not_in_flash_func(port_answer)(uint8_t port, uint8_t value) {
    // Сначала значение, потом разрешение отвечать: наоборот между двумя
    // записями поместилось бы чтение с шины и ушло бы прежнее содержимое
    // ячейки. Ноль-ответ живёт в ячейке, ноль-отказ - в таблице.
    s_portval[port] = value;
    std::atomic_signal_fence(std::memory_order_release);
    s_porttab[port] = addr32(&s_portval[port]);
}

// Страница ответов: байт выбирает старшая половина адреса. Нужна портам, у
// которых ответ от неё зависит, - клавиатура объединяет строки по нулевым
// разрядам, у мыши три порта делят младший байт. Страница живёт у
// устройства и меняется им же; здесь только разрешение отвечать.
void port_answer_page(uint8_t port, const uint8_t* page) {
    const uint32_t base = addr32(page);
    // Выравнивание обязательно: склейщик не складывает, а дописывает
    // младший байт адреса, и страница со сдвигом отвечала бы чужими байтами.
    if ((base & 0xffu) != 0u) {
        panic("port page 0x%02x not aligned: 0x%08x", port, static_cast<unsigned>(base));
    }
    s_porttab[port] = (base >> 8) | (1u << 31);
}

// Снимается запись: обнуление ячейки порт не отключило бы, склейщик отдал
// бы адрес, а канал выдачи - ноль.
void port_release(uint8_t port) {
    s_porttab[port] = 0u;
}

// Что порт отвечает сейчас - для журнала: снят, страница или байт ячейки.
int32_t port_answer_state(uint8_t port) {
    const uint32_t e = s_porttab[port];
    if (e == 0u) return kPortReleased;
    if ((e & (1u << 31)) != 0u) return kPortPaged;
    return static_cast<int32_t>(s_portval[port] & 0xffu);
}

// Жив ли Z80 - без инструкций в горячем пути. Канал уровня 1 цепочки
// чтения памяти хранит адрес записи таблицы страниц, прочитанной
// последней, - это адрес последнего чтения памяти ниже 0x4000,
// пересчитанный в запись таблицы. Регистр DMA читается только при печати;
// число меняется, пока процессор что-то исполняет; замерло - не
// исполняет.
uint32_t last_memory_read_ptr() {
    if (s_dma_tab_data < 0) return 0u;
    return dma_hw->ch[s_dma_tab_data].read_addr;
}

// Пишет log_task на Core0 (bus_queues_poll), читает print_bus_health на
// Core1. Писатель один - relaxed load и store.
std::atomic<uint32_t> s_serve_late{0};
std::atomic<uint32_t> s_trap_lost{0};
std::atomic<uint32_t> s_join_lost{0};
std::atomic<uint32_t> s_route_lost{0};
std::atomic<uint32_t> s_detect_lost{0};
std::atomic<uint32_t> s_join_stall{0};
std::atomic<uint32_t> s_route_stall{0};
std::atomic<uint32_t> s_trdos_lost{0};
std::atomic<uint32_t> s_trdos_desync{0};
std::atomic<uint8_t> s_trdos_level{0};
std::atomic<uint8_t> s_trdos_state{0};
uint32_t s_serve_late_full = 0; // пишет и читает Core0

// Единственный писатель - Core0: relaxed, без чтения-изменения-записи.
void count_lost(std::atomic<uint32_t>& c) {
    c.store(c.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
}

// Разряд fdebug стоит - погасить его записью единицы и посчитать. Флаги
// липкие, а опрос раз в 1-10 мс: число - опросов, заставших флаг, а не
// слов.
void take_flag(pio_hw_t* pio, uint32_t bit, std::atomic<uint32_t>& c) {
    if ((pio->fdebug & bit) == 0u) return;
    pio->fdebug = bit;
    count_lost(c);
}

// TR-DOS: потерянная отметка детектора и расхождение вывода DOS_N с его
// состоянием. Состояние автомат помнит адресом программы: released - от
// своей метки до asserted, asserted - до конца. Расхождение засчитывается,
// если держится два опроса подряд: между push и переходом в другую ветку
// автомат миг стоит в старой.
void trdos_poll() {
    if (s_trdos_detect_offset < 0) return;
    take_flag(PIO_DETECT, 1u << (PIO_FDEBUG_RXSTALL_LSB + SM_TRDOS_DETECT), s_trdos_lost);
    const uint32_t pc     = pio_sm_get_pc(PIO_DETECT, SM_TRDOS_DETECT) - static_cast<uint32_t>(s_trdos_detect_offset);
    const bool asserted   = pc >= trdos_detect_offset_asserted;
    const bool level_high = gpio_get(PIN_DOS_N);
    s_trdos_level.store(level_high ? 1u : 0u, std::memory_order_relaxed);
    s_trdos_state.store(asserted ? 1u : 0u, std::memory_order_relaxed);
    // asserted - DOS_N низкий, released - высокий.
    static uint8_t s_apart_polls = 0;
    if (asserted != level_high) {
        s_apart_polls = 0;
    } else if (s_apart_polls < 2u) {
        ++s_apart_polls;
        if (s_apart_polls == 2u) count_lost(s_trdos_desync);
    }
}

// Очереди трапов, тем же проходом, что отметки опоздания: на пути шины ни
// одной команды.
//
// Потеря слова: rom_trap кладёт push noblock (RXSTALL - выброшено), а в
// склейщик, в trap_route и в детектор слово льёт DMA - в полную TX оно
// отбрасывается молча, это TXOVER автомата-приёмника. RXSTALL у склейщиков -
// простой на полной RX (push блокирующий), не потеря.
//
// Очереди в pio0 и pio2; fdebug pio1 с Core0 не сбрасывается: его разряд
// ловит bus_wr_isr.
void trap_stall_poll() {
    take_flag(PIO_DETECT, 1u << (PIO_FDEBUG_RXSTALL_LSB + SM_ROM_TRAP), s_trap_lost);
    take_flag(PIO_DETECT, 1u << (PIO_FDEBUG_TXOVER_LSB + SM_ROM_DETECT), s_detect_lost);
    if (s_sm_trap_join >= 0) {
        const uint sm = static_cast<uint>(s_sm_trap_join);
        take_flag(PIO_AUDIO, 1u << (PIO_FDEBUG_TXOVER_LSB + sm), s_join_lost);
        take_flag(PIO_AUDIO, 1u << (PIO_FDEBUG_RXSTALL_LSB + sm), s_join_stall);
    }
    if (s_sm_trap_route >= 0) {
        const uint sm = static_cast<uint>(s_sm_trap_route);
        take_flag(PIO_AUDIO, 1u << (PIO_FDEBUG_TXOVER_LSB + sm), s_route_lost);
        take_flag(PIO_AUDIO, 1u << (PIO_FDEBUG_RXSTALL_LSB + sm), s_route_stall);
    }
    trdos_poll();
}

// Автомат reset_watch: отметки идут, пока машина стоит. Начало сброса - первый
// опрос с отметками после опроса без них.
std::atomic<uint32_t> s_machine_resets{0};

void reset_watch_poll() {
    if (s_sm_reset_watch < 0) return;
    static bool s_in_reset = false;
    const uint sm          = static_cast<uint>(s_sm_reset_watch);
    bool marks             = false;
    while (!pio_sm_is_rx_fifo_empty(PIO_DETECT, sm)) {
        (void)pio_sm_get(PIO_DETECT, sm);
        marks = true;
    }
    if (marks && !s_in_reset) {
        // ПЗУ TR-DOS - сразу, пока машина в сбросе: стартовать она должна с
        // обычного ПЗУ.
        trdos_release();
        count_lost(s_machine_resets);
        // Чей сброс: линию держит сама плата - её выход стоит в единице.
        const bool own = gpio_get_out_level(PIN_RESET_N);
        debug_logf("signal: machine reset seen (no M1 for 50 Z80 cycles at 3.5 MHz), %s, count %" PRIu32 "\n", own ? "held by the board" : "from the machine",
                   s_machine_resets.load(std::memory_order_relaxed));
    }
    s_in_reset = marks;
}

void trdos_release() {
    if (s_trdos_detect_offset < 0 || s_sm_trdos_drive < 0) {
        release_host_trdos();
        return;
    }
    pio_sm_exec(PIO_DETECT, SM_TRDOS_DETECT, pio_encode_jmp(static_cast<uint>(s_trdos_detect_offset) + trdos_detect_offset_released));
    pio_sm_put(PIO_AUDIO, static_cast<uint>(s_sm_trdos_drive), 1u);
}

uint32_t machine_resets_seen() {
    return s_machine_resets.load(std::memory_order_relaxed);
}

void bus_queues_poll() {
    trap_stall_poll();
    reset_watch_poll();
    uint32_t n = 0;
    while (!pio_sm_is_rx_fifo_empty(PIO_SERVE, SM_SERVE)) {
        (void)pio_sm_get(PIO_SERVE, SM_SERVE);
        ++n;
    }
    if (n != 0) s_serve_late.store(s_serve_late.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    // FIFO полон к опросу - отметки сверх его глубины потеряны, и число
    // опозданий занижено.
    if (n >= 4u) ++s_serve_late_full;
}

BusMeasurements bus_measurements() {
    BusMeasurements m;
    m.serve_late_full = s_serve_late_full;
    m.trap_lost       = s_trap_lost.load(std::memory_order_relaxed);
    m.join_lost       = s_join_lost.load(std::memory_order_relaxed);
    m.route_lost      = s_route_lost.load(std::memory_order_relaxed);
    m.detect_lost     = s_detect_lost.load(std::memory_order_relaxed);
    m.join_stall      = s_join_stall.load(std::memory_order_relaxed);
    m.route_stall     = s_route_stall.load(std::memory_order_relaxed);
    m.trdos           = s_trdos_detect_offset >= 0;
    m.trdos_lost      = s_trdos_lost.load(std::memory_order_relaxed);
    m.trdos_desync    = s_trdos_desync.load(std::memory_order_relaxed);
    m.trdos_level     = s_trdos_level.load(std::memory_order_relaxed);
    m.trdos_asserted  = s_trdos_state.load(std::memory_order_relaxed) != 0u;
    return m;
}

GenericPortsState generic_ports_state() {
    GenericPortsState g;
    g.tab_cmd    = s_porttab[PORT_CMD];
    g.tab_dat    = s_porttab[PORT_DAT];
    g.val_cmd    = static_cast<uint8_t>(s_portval[PORT_CMD]);
    g.val_dat    = static_cast<uint8_t>(s_portval[PORT_DAT]);
    g.serve_late = s_serve_late.load(std::memory_order_relaxed);
    return g;
}

} // namespace bus
