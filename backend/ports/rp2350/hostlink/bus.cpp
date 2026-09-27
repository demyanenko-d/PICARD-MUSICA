// Реализация bus.h. Разделы в том же порядке: пины, плагинная шина,
// эмуляция ПЗУ.

#include "bus_setup.h"

#include <atomic>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#include "divmmc.h"
#include "player/live/ay_tap.h"
#include "core/live_midi/ay_midi.h"
#include "memory.pio.h" // PAGE_TABLE_BASE_SHIFT
#include "ports.pio.h" // PORT_TABLE_BASE_SHIFT
#include "player/protocol/host_frame.h"

namespace bus {

// --- Пины и перемычки ---
// Опрос перемычек режима при старте.

// Анонимного пространства имён нет: монтаж в bus_setup.cpp, регистры,
// таблицы и обработчики должны быть видны оттуда. Имена в namespace bus с
// префиксом s_ или k.

// Перемычка читается один раз при загрузке, дальше все спрашивают флаг:
// режим не должен меняться на ходу. Определение - рядом с таблицами ответов
// (плагинная шина ниже): set_rd_word читает флаг от их общей базы.
extern bool s_divmmc;

void latch_divmmc_jumper() {
    gpio_init(PIN_DIVMMC_JUMPER);
    gpio_set_function(PIN_DIVMMC_JUMPER, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_DIVMMC_JUMPER, GPIO_IN);
    gpio_pull_up(PIN_DIVMMC_JUMPER);

    // Подтяжке нужно время поднять линию: у RP2350 она 50-80 кОм, с ёмкостью
    // дорожки и перемычки фронт неспешный. Чтение сразу после включения
    // подтяжки может дать ноль на пустом месте и включить DivMMC.
    busy_wait_us(100);

    s_divmmc = (gpio_get(PIN_DIVMMC_JUMPER) == 0);
}

bool divmmc_selected() {
    return s_divmmc;
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

// Перемычка стоит, DivMMC включён. Пишет latch_divmmc_jumper при загрузке.
bool s_divmmc = false;

// Единственное место, где слово ответа попадает на шину: байт - в ячейку
// ответа порта (путь портов один на оба положения перемычки).
__force_inline void __not_in_flash_func(set_rd_word)(uint8_t port, uint32_t word) {
    rom_emu_set_port(port, rd_word_byte(word));
}

// Разбор записи в порт - один на оба режима: bus_wr_isr без перемычки,
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

void z80_bus_pio_register_wr(uint8_t port, PortWriteFn fn) {
    s_wr_table[port] = fn;
}

// В SRAM: вызывается из обоих обработчиков на каждом байте эмулируемой
// карты; во флеше - промах кэша XIP, а горячий путь должен укладываться в
// несколько микросекунд между командами IN/OUT Z80.
void __not_in_flash_func(z80_bus_pio_set_rd)(uint8_t port, uint8_t value) {
    set_rd_word(port, encode_rd_word(value));
}

void z80_bus_pio_register_rd_done(uint8_t port, PortReadFn fn) {
    s_rd_done_table[port] = fn;
}

PioStats z80_bus_pio_get_stats() {
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
              "вариант таблицы страниц - ровно окно адреса, которое собирает rom_detect");
// Выравнивание вдвое больше варианта: младший разряд базы в X - номер
// варианта по чётности, он же ROM_BLK_N (rom_detect, mov pins, x).
alignas(2u << PAGE_TABLE_BASE_SHIFT) uint32_t s_pagetabs[PAGE_TAB_VARIANTS][PAGE_TAB_HALVES][PAGES_PER_TABLE];
uint32_t s_page_variant = 0;

// Откуда брать байт ответа на порт, ноль - порт не наш. Младший разряд
// записи - маршрут склейщика: ноль значит готовый адрес ячейки (он чётный),
// единица - база страницы на 256 байт, сдвинутая вправо на 8. Таблица
// выровнена на 1024: адрес записи собирается склейкой в детекторе
// (in x,22 / in pins,8 / in null,2).
alignas(1u << PORT_TABLE_BASE_SHIFT) uint32_t s_porttab[256];

// Ячейки ответов, отдельно от таблицы: значение пишется сюда, разрешение
// отвечать - указателем в таблице.
uint32_t s_portval[256];

int s_dma_port_addr = -1; // rxf детектора портов -> read_addr следующего
int s_dma_port_ptr = -1; // porttab[порт] -> al3_read_addr_trig канала выдачи ответа
int s_dma_port_join = -1; // rxf склейщика -> al3_read_addr_trig канала выдачи байта
int s_sm_port_join = -1;  // склейщик ячейки ответа, в блоке звука

int s_dma_tab_addr = -1; // rxf детектора -> read_addr следующего
int s_dma_tab_data = -1; // pagetab[регион] -> txf rom_join
int s_dma_byte_addr = -1; // rxf rom_join -> read_addr следующего
int s_dma_byte_data = -1; // страница[смещение] -> txf rom_serve

// Трапы DivMMC без ядра: rom_trap (pio0) -> канал -> склейщик trap_join
// (pio2) -> пара каналов читает запись карты -> канал слова варианта в
// очередь детектора. Запись карты - указатель на слово "войти" или
// "выйти"; ноль - пустой триггер, цепочка стоит.
alignas(128) uint32_t s_trap_page_map[kTrapPageMapEntries];   // 0x2000-0x3FFF по странице
uint32_t s_trap_word[2];  // слова варианта: [0] войти, [1] выйти - ставит divmmc
int s_sm_trap_join = -1;
int s_dma_trap_in = -1;   // rom_trap rxf -> склейщик txf
int s_dma_trap_addr = -1; // склейщик rxf -> read_addr канала записи
int s_dma_trap_entry = -1; // запись карты -> al3_read_addr_trig канала слова
int s_dma_trap_word = -1; // слово варианта -> txf детектора
int s_dma_trap_note = -1; // адрес отданного слова -> s_trap_last (после word)
// Адрес последнего отданного слова: вход или выход. Пустой триггер пишет
// ноль в read_addr канала слова, поэтому состояние - не там, а здесь: его
// копирует канал, которого запускает только настоящий трап.
volatile uint32_t s_trap_last = 0;

// Обработчик записей: разобрать очередь и отдать слова разбору (логика в
// divmmc.cpp).
//
// Цикл, а не по слову: между двумя записями Z80 может быть меньше
// времени, чем вход в прерывание, оставленное слово ждало бы следующего
// события неизвестно сколько.
// Наибольшее число слов, разобранных обработчиком записей за один вызов.
// Пишет bus_wr_isr, читает log_task.
uint32_t s_bus_wr_peak = 0;
uint32_t bus_wr_peak() {
    return s_bus_wr_peak;
}

// Запись в порты AY (#FFFD выбор регистра, #BFFD данные: A15 = 1, младший
// байт #FD, A14 различает) - в кольцо живого MIDI, разбор на Core0.
__force_inline void tap_ay_write(uint32_t raw) {
    if ((raw & (1u << PIN_IORQ_N)) != 0u) return; // запись в память
    const uint16_t addr = addr_of(raw);
    if ((addr & 0x80FFu) != 0x80FDu) return;
    const uint16_t sel = (addr & 0x4000u) ? soundsinth::midi_in::kAyWriteSelect : 0u;
    player::live::ay_tap_push(static_cast<uint16_t>(sel | data_of(raw)));
}

void __not_in_flash_func(bus_wr_isr)() {
    // Флаг снимается до разбора, иначе запись, пришедшая между чтением
    // очереди и снятием флага, потеряла бы пробуждение.
    pio_interrupt_clear(PIO_WATCH, 0);

    // Без DivMMC автомат ловит только записи в порт: разбор - сразу по
    // таблице обработчиков.
    uint32_t drained = 0;
    while (!pio_sm_is_rx_fifo_empty(PIO_WATCH, SM_BUS_WRITE)) {
        const uint32_t raw = pio_sm_get(PIO_WATCH, SM_BUS_WRITE);
        tap_ay_write(raw);
        if (s_divmmc) {
            divmmc_on_bus_write(raw);
        } else {
            dispatch_port_write(static_cast<uint8_t>(addr_of(raw)), data_of(raw));
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
        if (s_divmmc) divmmc_note_lost(drained);
    }
}

// --- Замеры шины ---
//
// Три случая, которых по нынешним счётчикам не видно, и от каждого зависит,
// нужна ли перестройка обработчиков. Пишут только обработчики, по одному
// приращению в уже существующей ветке; читает задача логгера.

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
    if (s_divmmc) {
        divmmc_on_port_read(port);
    } else {
        dispatch_port_read_done(port);
    }
}

// Зовётся только из build_pagetabs при сбросе. Область - регион 8 КБ, как
// у divmmc; гранула 256 байт раскладывается здесь. На пути трапа таблицы
// не трогаются: меняется вариант, одно слово в регистре детектора.
void __not_in_flash_func(rom_emu_fill_pages)(uint32_t variant, uint32_t region, const void* page) {
    // Регионов в таблице два: она покрывает только 0x0000-0x3FFF.
    if (variant >= PAGE_TAB_VARIANTS || region >= PAGE_TAB_REGIONS) return;
    // kPageNotOurs - "страница не наша": X = 0, rom_join обрывает цикл по
    // jmp !x, на шину никто не выходит. Указатель выровнен на 256 байт (младшие 8 бит
    // теряются; s_rom и s_ram - alignas(8192)).
    const uint32_t base = page == nullptr ? 0u : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(page) >> 8);
    static_assert(kPageNotOurs == 1u, "rom_join: разряд 0 - ROM_BLK_N, X = 0 - страница не наша");
    const uint32_t first = region * PAGES_PER_REGION;
    for (uint32_t i = 0; i < PAGES_PER_REGION; ++i) {
        // Каждой 256-байтовой странице своя база: одна на все 32 означала бы, что
        // весь регион читается из первых 256 байт.
        const uint32_t e = (base == 0u) ? kPageNotOurs : page_entry(base + i);
        s_pagetabs[variant][kFetchHalf][first + i] = e;
        s_pagetabs[variant][kDataHalf][first + i] = e;
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
void __not_in_flash_func(rom_emu_set_fetch_page)(uint32_t variant, uint32_t page, const void* mem) {
    if (variant >= PAGE_TAB_VARIANTS || page >= PAGES_PER_TABLE) return;
    s_pagetabs[variant][kFetchHalf][page] =
        mem == nullptr ? kPageNotOurs : page_entry(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem) >> 8));
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
    pio_sm_put(PIO_DETECT, SM_ROM_DETECT,
               static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_pagetabs[variant][0][0])) >> PAGE_TABLE_BASE_SHIFT);
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
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_pagetabs[variant][0][0])) >> PAGE_TABLE_BASE_SHIFT;
}
} // namespace

void rom_emu_trap_clear() {
    for (uint32_t i = 0; i < kTrapAddrMapEntries; ++i) s_trap_addr_map[i] = 0u;
    for (uint32_t& e : s_trap_page_map) e = 0u;
}

void rom_emu_trap_set(uint16_t addr, TrapKind kind) {
    const auto ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_trap_word[static_cast<uint32_t>(kind)]));
    if (addr < kTrapAddrMapEntries) {
        s_trap_addr_map[addr] = ptr;
    } else if (addr < 0x4000u) {
        s_trap_page_map[(addr >> 8) & (kTrapPageMapEntries - 1u)] = ptr;
    }
}

void __not_in_flash_func(rom_emu_trap_variants)(uint32_t enter_variant, uint32_t exit_variant) {
    s_trap_word[static_cast<uint32_t>(TrapKind::Enter)] = variant_word(enter_variant);
    s_trap_word[static_cast<uint32_t>(TrapKind::Exit)] = variant_word(exit_variant);
}

bool __not_in_flash_func(rom_emu_trap_entered)() {
    return s_trap_last ==
           static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_trap_word[static_cast<uint32_t>(TrapKind::Enter)]));
}

void rom_emu_trap_reset_state() {
    s_trap_last = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_trap_word[static_cast<uint32_t>(TrapKind::Exit)]));
}

void __not_in_flash_func(rom_emu_set_port)(uint8_t port, uint8_t value) {
    // Сначала значение, потом разрешение отвечать: наоборот между двумя
    // записями поместилось бы чтение с шины и ушло бы прежнее содержимое
    // ячейки. Ноль-ответ живёт в ячейке, ноль-отказ - в таблице.
    s_portval[port] = value;
    std::atomic_signal_fence(std::memory_order_release);
    s_porttab[port] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_portval[port]));
}

// Страница ответов: байт выбирает старшая половина адреса. Нужна портам, у
// которых ответ от неё зависит, - клавиатура объединяет строки по нулевым
// разрядам, у мыши три порта делят младший байт. Страница живёт у
// устройства и меняется им же; здесь только разрешение отвечать.
void rom_emu_set_port_page(uint8_t port, const uint8_t* page) {
    const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(page));
    // Выравнивание обязательно: склейщик не складывает, а дописывает
    // младший байт адреса, и страница со сдвигом отвечала бы чужими байтами.
    if ((base & 0xFFu) != 0u) {
        panic("port page 0x%02x not aligned: 0x%08x", port, static_cast<unsigned>(base));
    }
    s_porttab[port] = ((base >> 8) << 1) | 1u;
}

// Снимается запись: обнуление ячейки порт не отключило бы, склейщик отдал
// бы адрес, а канал выдачи - ноль.
void rom_emu_clear_port(uint8_t port) {
    s_porttab[port] = 0u;
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

// Пишет log_task на Core0 (rom_emu_serve_late_poll), читает print_bus_health на
// Core1. Писатель один - relaxed load и store.
std::atomic<uint32_t> s_serve_late{0};
uint32_t s_serve_late_full = 0; // пишет и читает Core0

void rom_emu_serve_late_poll() {
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
    return m;
}

GenericPortsState generic_ports_state() {
    GenericPortsState g;
    g.tab_cmd = s_porttab[PORT_CMD];
    g.tab_dat = s_porttab[PORT_DAT];
    g.val_cmd = static_cast<uint8_t>(s_portval[PORT_CMD]);
    g.val_dat = static_cast<uint8_t>(s_portval[PORT_DAT]);
    g.serve_late = s_serve_late.load(std::memory_order_relaxed);
    return g;
}

} // namespace bus
