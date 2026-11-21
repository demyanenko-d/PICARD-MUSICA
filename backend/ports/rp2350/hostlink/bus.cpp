// SPDX-License-Identifier: MIT
// Реализация bus.h. Разделы в том же порядке: пины, плагинная шина,
// эмуляция ПЗУ.

#include "bus.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <initializer_list>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/structs/m33.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico.h"

#include "platform/log.h"

#include "core/live_midi/ay_midi.h"

#include "player/nav/nav_page.h"

#include "player/live/ay_live.h"
#include "player/live/ay_port.h"
#include "player/live/live_tap.h"
#include "player/live/dac_live.h"
#include "player/live/saa_live.h"
#include "player/protocol/host_frame.h"

#include "bus_setup.h"
#include "divmmc.h"
#include "memory.pio.h"      // PAGE_TABLE_BASE_SHIFT
#include "ports.pio.h"       // PORT_TABLE_BASE_SHIFT
#include "trap_join.pio.h"   // TRAP_LO_BASE_SHIFT, TRAP_SUB_BASE_SHIFT
#include "reset_watch.pio.h" // IRQ_ROM48, IRQ_PAGING_LOCKED
#include "trdos.pio.h"       // TRDOS_IRQ_NAV, TRDOS_IRQ_FETCH
#include "pentagon.pio.h"    // IRQ_DOS
#include "rom_image_nav.h"

namespace bus {

// --- Пины и режим ---
//

// Анонимного пространства имён нет: монтаж в bus_setup.cpp, регистры,
// таблицы и обработчики должны быть видны оттуда. Имена в namespace bus с
// префиксом s_ или k.

// Режим ставится один раз при загрузке, дальше все спрашивают флаг: на
// ходу он не меняется.
extern bool s_rom_emu;
extern bool s_trdos;
extern bool s_config_rom;
extern bool s_pentagon;
extern bool s_pentagon_drive_dos;

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

void pentagon_rom_set_enabled(bool on, bool drive_dos) {
    s_pentagon           = on;
    s_pentagon_drive_dos = on && drive_dos;
}

bool pentagon_rom_enabled() {
    return s_pentagon;
}

// Страница Пентагона p - по адресу бит 4 << 15 | DOS << 14 от начала блока:
// так её собирает автомат pentagon_rom.
bool pentagon_rom_load(const uint8_t* image) {
    bool erased = true;
    for (uint32_t i = 0; i < kPentagonRomBytes && erased; ++i) {
        erased = image[i] == 0xffu;
    }
    if (erased) return false;
    uint8_t* const block     = pentagon_rom_block();
    constexpr uint32_t kPage = kPentagonRomBytes / 4u;
    for (uint32_t p = 0; p < 4u; ++p) {
        const uint32_t rom48 = p & 1u;
        const uint32_t dos   = p < 2u ? 1u : 0u;
        std::memcpy(block + (rom48 << kPentagonWordShift) + (dos << (kPentagonWordShift - 1u)), image + p * kPage, kPage);
    }
    return true;
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

// --- Выставленный кадр ---
//
// Протокол отдаёт команду целиком (код и аргументы), слова ответа на все
// её байты кодируются один раз (host_frame.h). Дальше ISR чтения протокол
// не спрашивает: сдвинула индекс, положила готовое слово.
uint32_t s_frame_words[player::protocol::host_frame::kWords];
volatile uint8_t s_frame_pos = 0; // двигает ISR чтения, обнуляет arm_frame

// Приёмник кадра: слово ответа - в ячейку ответа порта.
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

// Байт команды хоста пришёл во время выставления: команду протокол вернёт
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
// Своё ПЗУ Пентагона: выбрано настройками и ведёт ли DOS_N.
bool s_pentagon           = false;
bool s_pentagon_drive_dos = false;
// Слово базы автомата отдачи без бита 4: блок >> 15. Ноль - автомат не
// поднят.
uint32_t s_pentagon_word = 0;
// Своё ПЗУ при работающей машине (RESET_SIGNAL=0): ROM_BLK_N ставится
// по первому сбросу машины, а не под исполняемым кодом её ПЗУ.
bool s_rom_take_on_reset = false;
// Бит 4 на момент закрытия порта 7FFD.
bool s_pentagon_locked_rom48 = false;
// Записи в порт при своём ПЗУ: канал s_dma_wr_ring кладёт снимки bus_wr в
// кольцо, а копию каждого - фильтру 7FFD в pio2. Ядро читает кольцо, а не
// очередь автомата. -1 - кольца нет, ядро читает очередь.
alignas(kWrRingBytes) uint32_t s_wr_ring[kWrRingWords];
int s_dma_wr_ring     = -1;
uint32_t s_wr_ring_at = 0; // следующее слово для ядра
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
// мимо s_rd_done_table. Не встраивается: копия у divmmc всё равно нужна,
// а SRAM на счету.
__noinline void __not_in_flash_func(dispatch_port_read_done)(uint8_t addr) {
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
alignas(2u << PAGE_TABLE_BASE_SHIFT) SOUNDSINTH_BUSMEM("pagetabs") uint32_t s_pagetabs[PAGE_TAB_VARIANTS][PAGE_TAB_HALVES][PAGES_PER_TABLE];
uint32_t s_page_variant = 0;

// Откуда брать байт ответа на порт, ноль - порт не наш. Старший разряд
// записи - маршрут склейщика: ноль значит готовый адрес ячейки, единица -
// база страницы на 256 байт, сдвинутая вправо на 8. Адреса SRAM младше
// 2^31, разряд свободен.
//
// Таблица выровнена на 1024: адрес записи собирается склейкой в детекторе
// (in x,22 / in pins,8 / in null,2).
alignas(1u << PORT_TABLE_BASE_SHIFT) SOUNDSINTH_BUSMEM("porttab") uint32_t s_porttab[256];

// Без DivMMC порты отвечают по паре таблиц (port_detect_dos): при DOS - эта,
// без DOS - s_porttab, разряд kPortTabDosBit адреса записи - ~DOS. Обычные
// порты в обеих одинаковы, порты Beta Disk - только здесь.
//
// s_dos_mirror[порт] = 1 - ответ порта повторяется в таблице при DOS. Нули -
// порты Beta Disk; в режиме DivMMC таблица при DOS не читается.
uint8_t s_dos_mirror[256];
// Пока Beta Disk не поставил свой хук, чтений его портов при DOS нет:
// ячеек ещё нет. Пустая функция - на случай порядка подъёма.
void __not_in_flash_func(dos_rd_none)(uint8_t) {}
PortReadFn s_dos_rd_done          = dos_rd_none;
constexpr uint32_t kPortTabDosBit = 1u << PORT_TABLE_BASE_SHIFT;

// Ячейки ответов, отдельно от таблицы: значение пишется сюда, разрешение
// отвечать - указателем в таблице.
uint32_t s_portval[256];

int s_dma_port_addr  = -1; // rxf детектора портов -> read_addr следующего
int s_dma_port_ptr   = -1; // porttab[порт] -> al3_read_addr_trig канала выдачи ответа
int s_dma_port_join  = -1; // rxf склейщика -> al3_read_addr_trig канала выдачи байта
int s_sm_port_join   = -1; // склейщик ячейки ответа, в блоке звука
bool s_trdos_up      = false;
bool s_hold_up       = false;
int s_sm_reset_watch = -1;

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

// Худший проход bus_wr_isr в тактах ядра (DWT CYCCNT, включает setup_bus_wr)
// и сколько слов он разобрал. Применение записи ВГ93 будет жить в этом
// обработчике - это базовая строка его бюджета.
uint32_t s_bus_wr_cyc_max     = 0;
uint32_t s_bus_wr_cyc_drained = 0;

// Записи в порты звуковых чипов - в кольцо отвода, разбор на Core0. Штамп -
// часы шины в полумикросекундах, как bus_clock_stamp (та - в банках BDI,
// без BDI её нет).
//
// AY: #FFFD выбор регистра, #BFFD данные - A15 = 1, младший байт #FD, A14
// различает. Та же запись ведёт тень регистров для ответа на чтение #FFFD.
//
// SAA1099 MultiSound, пока включён живой SAA: младший байт #FF, A8 = 1 -
// номер регистра, 0 - данные. При включённом ПЗУ TR-DOS порт #FF -
// системный регистр Beta Disk, и запись в него чипу не идёт - в любом
// режиме с DOS, как у Soundrive: своё ПЗУ без DOS_N не исключение.
constexpr uint16_t kAyPortMask    = 0x80ffu;
constexpr uint16_t kAyPortMatch   = 0x80fdu;
constexpr uint16_t kAyDataSelect  = 0x4000u; // A14: ноль - данные, единица - выбор регистра
constexpr uint16_t kSaaPortLow    = 0x00ffu;
constexpr uint16_t kSaaAddrSelect = 0x0100u; // A8: ноль - данные, единица - номер регистра

// Цифровой звук машины (dac_live.h), пока что-то включено - в своё кольцо.
// ULA - любой чётный порт: бипер кладёт запись, только когда сменились
// слышные разряды (бордюр пишется в тот же порт). Soundrive - младший байт
// по маске #AF равен #0F: #0F, #1F, #4F, #5F; при ПЗУ TR-DOS #1F и #5F -
// порты Beta Disk, звука от них нет.
constexpr uint8_t kCovoxPort = 0xfbu;
constexpr uint8_t kSdMask    = 0xafu;
constexpr uint8_t kSdMatch   = 0x0fu;
constexpr uint8_t kSdDosBit  = 0x10u; // #1F и #5F
uint8_t s_fe_heard           = 0;
__force_inline void tap_dac_write(uint16_t addr, uint8_t data) {
    using namespace player::live;
    const uint8_t on = g_dac_on;
    if (on == 0u) return;
    const auto lo = static_cast<uint8_t>(addr);
    if ((lo & 1u) == 0u) {
        const uint8_t heard = data & on & (kDacBeeperEar | kDacBeeperMic);
        if (heard == s_fe_heard) return;
        s_fe_heard = heard;
        g_dac_ring.push(timer_hw->timerawl * 2u, static_cast<uint8_t>(DacPort::Beeper), heard);
        return;
    }
    if (lo == kCovoxPort) {
        if ((on & kDacCovox) != 0u) g_dac_ring.push(timer_hw->timerawl * 2u, static_cast<uint8_t>(DacPort::Covox), data);
        return;
    }
    if ((on & kDacSoundrive) == 0u || (lo & kSdMask) != kSdMatch) return;
    if ((lo & kSdDosBit) != 0u && (PIO_DETECT->irq & (1u << IRQ_DOS)) != 0u) return;
    // #0F - A, #1F - B, #4F - C, #5F - D: разряды 4 и 6 номера.
    const uint8_t dac = static_cast<uint8_t>(DacPort::SdA) + ((lo >> 4) & 1u) + ((lo >> 5) & 2u);
    g_dac_ring.push(timer_hw->timerawl * 2u, dac, data);
}

__force_inline void tap_chip_write(uint32_t raw) {
    if (is_memory_write(raw)) return;
    const uint16_t addr = addr_of(raw);
    if ((addr & kAyPortMask) == kAyPortMatch) {
        const bool select = (addr & kAyDataSelect) != 0;
        player::live::tap_push(timer_hw->timerawl * 2u, select ? player::live::TapPort::AySelect : player::live::TapPort::AyData, data_of(raw));
        player::live::ay_port_write(select, data_of(raw));
        return;
    }
    tap_dac_write(addr, data_of(raw));
    if ((addr & 0xffu) != kSaaPortLow || !player::live::g_saa_live_on) return;
    if ((PIO_DETECT->irq & (1u << IRQ_DOS)) != 0u) return;
    const auto port = (addr & kSaaAddrSelect) ? player::live::TapPort::SaaAddr : player::live::TapPort::SaaData;
    player::live::tap_push(timer_hw->timerawl * 2u, port, data_of(raw));
}

// Запись в порт 0x7FFD (A15 = 0, A1 = 0) - для триггера TR-DOS: бит 4
// выбирает ПЗУ 48, бит 5 закрывает порт до сброса машины. Оба держат флаги
// IRQ блока триггера, гасит их сброс машины. Байт целиком - навигатору
// образов: по биту 3 он знает, какой экран виден.
constexpr uint16_t kPagingPortMask = 0x8002u;
constexpr uint8_t kPagingRom48     = 0x10u;
constexpr uint8_t kPagingLock      = 0x20u;
volatile uint8_t s_port_7ffd       = 0;
// Своё ПЗУ Пентагона: бит 4 ставит paging_filter в pio2 к следующей выборке.
// Закрытия порта фильтр не знает: после него здесь флаг и слово базы
// возвращаются к значению на момент закрытия.
__force_inline void tap_paging_write(uint32_t raw) {
    if ((!s_trdos_up && s_pentagon_word == 0u) || is_memory_write(raw)) return;
    if ((addr_of(raw) & kPagingPortMask) != 0u) return;
    const uint8_t data = data_of(raw);
    bool rom48         = (data & kPagingRom48) != 0u;
    const bool locked  = (PIO_DETECT->irq & (1u << IRQ_PAGING_LOCKED)) != 0u;
    if (!locked) s_port_7ffd = data;
    if (s_pentagon_word != 0u) {
        // Открытый порт - забота фильтра: повтор отсюда опоздал бы и при
        // двух записях подряд вернул бы старую страницу.
        if (!locked) {
            s_pentagon_locked_rom48 = rom48;
            if ((data & kPagingLock) != 0u) PIO_DETECT->irq_force = 1u << IRQ_PAGING_LOCKED;
            return;
        }
        rom48                            = s_pentagon_locked_rom48;
        PIO_DETECT->txf[SM_PENTAGON_ROM] = s_pentagon_word | (rom48 ? 1u : 0u);
    } else {
        if (locked) return;
        if ((data & kPagingLock) != 0u) PIO_DETECT->irq_force = 1u << IRQ_PAGING_LOCKED;
    }
    if (rom48) {
        PIO_DETECT->irq_force = 1u << IRQ_ROM48;
    } else {
        PIO_DETECT->irq = 1u << IRQ_ROM48;
    }
}

// --- Навигатор образов (TR-DOS с ПЗУ машины) ---
//
// Таблицы страниц DivMMC в этом режиме не нужны, их 32 КБ - память
// навигатора: ПЗУ, N0, копия ПЗУ, N1 по 8 КБ. trdos_nav собирает адрес
// байта как слово << 14 | A0..A13, слово = база >> 14 | банк окна.
constexpr uint32_t kNavWordShift = 14;
static_assert(sizeof(s_pagetabs) == 4u * kZ80BankBytes, "the image navigator takes the whole page tables");
static_assert(sizeof(kNavRom) == kZ80BankBytes, "the image navigator ROM is one bank");
bool s_nav_enabled          = false;
uint32_t s_nav_word         = 0; // база >> 14, ноль - навигатора нет
volatile uint8_t s_nav_bank = 0; // банк окна 0x2000: N0 или N1

__force_inline uint8_t* nav_bank(uint32_t b) {
    return reinterpret_cast<uint8_t*>(s_pagetabs) + (2u * b + 1u) * kZ80BankBytes;
}

// Запись в окно 0x2000-0x3FFF - в банк окна, пока стоит флаг навигатора.
// Ниже окна - его ПЗУ; без флага всё это ПЗУ машины.
__force_inline void nav_memory_write(uint32_t raw) {
    const uint16_t a = addr_of(raw);
    if (a < kZ80BankBytes || (PIO_DETECT->irq & (1u << TRDOS_IRQ_NAV)) == 0u) return;
    nav_bank(s_nav_bank)[a - kZ80BankBytes] = data_of(raw);
}

// Банк окна - запись в порт банка, пока стоит флаг. Слово - в очередь
// trdos_nav, он берёт его на следующем чтении памяти; записи в окно с этого
// места идут в новый банк.
void __not_in_flash_func(nav_bank_write)(uint8_t /*port*/, uint8_t data) {
    if ((PIO_DETECT->irq & (1u << TRDOS_IRQ_NAV)) == 0u) return;
    s_nav_bank                    = data & 1u;
    PIO_DETECT->txf[SM_TRDOS_NAV] = s_nav_word | s_nav_bank;
}

void nav_set_enabled(bool on) {
    s_nav_enabled = on;
}

// ПЗУ - в обе половины, номер банка - в байт kBank банка N1: в N0 его пишет
// плата вместе со страницей обмена.
uint32_t nav_prepare() {
    if (!s_nav_enabled) return 0;
    uint8_t* const block = reinterpret_cast<uint8_t*>(s_pagetabs);
    std::memcpy(block, kNavRom, kZ80BankBytes);
    std::memcpy(block + 2u * kZ80BankBytes, kNavRom, kZ80BankBytes);
    nav_bank(1)[player::nav_page::kBank] = 1;
    port_on_write(player::nav_page::kBankPort, nav_bank_write);
    s_nav_word = addr32(block) >> kNavWordShift;
    return s_nav_word;
}

uint8_t* nav_page() {
    return s_nav_word != 0u ? nav_bank(0) : nullptr;
}

bool __not_in_flash_func(nav_open)() {
    return s_nav_word != 0u && (PIO_DETECT->irq & (1u << TRDOS_IRQ_NAV)) != 0u;
}

uint8_t port_7ffd() {
    return s_port_7ffd;
}

void __not_in_flash_func(bus_wr_isr)() {
    const uint32_t cyc0 = m33_hw->dwt_cyccnt;
    // Флаг bus_wr_ports снимается до разбора, иначе запись, пришедшая между
    // чтением очереди и снятием флага, потеряла бы пробуждение. bus_wr флага
    // не ставит: его пробуждение - непустая очередь, оно гаснет само.
    pio_interrupt_clear(PIO_WATCH, 0);

    // Без DivMMC автомат ловит только записи в порт: разбор - сразу по
    // таблице обработчиков.
    //
    // Цикл, а не по слову: между двумя записями Z80 бывает меньше времени,
    // чем вход в прерывание.
    uint32_t drained = 0;
    if (s_dma_wr_ring >= 0) {
        // Своё ПЗУ: очередь забирает канал. Слово, снятое из очереди, ложится
        // в кольцо через несколько тактов - выдержка после опустевшей очереди
        // его дожидается, иначе последняя запись ждала бы следующей.
        for (;;) {
            while (!pio_sm_is_rx_fifo_empty(PIO_WATCH, SM_BUS_WRITE)) {
                tight_loop_contents();
            }
            busy_wait_at_least_cycles(32);
            const uint32_t head = (dma_hw->ch[s_dma_wr_ring].write_addr - reinterpret_cast<uintptr_t>(s_wr_ring)) / sizeof(uint32_t) % kWrRingWords;
            if (s_wr_ring_at == head) break;
            while (s_wr_ring_at != head) {
                const uint32_t raw = s_wr_ring[s_wr_ring_at];
                s_wr_ring_at       = (s_wr_ring_at + 1u) % kWrRingWords;
                tap_chip_write(raw);
                tap_paging_write(raw);
                dispatch_port_write(port_of(raw), data_of(raw));
                ++drained;
            }
        }
    }
    while (!pio_sm_is_rx_fifo_empty(PIO_WATCH, SM_BUS_WRITE) && s_dma_wr_ring < 0) {
        const uint32_t raw = pio_sm_get(PIO_WATCH, SM_BUS_WRITE);
        tap_chip_write(raw);
        tap_paging_write(raw);
        if (s_rom_emu) {
            divmmc_on_bus_write(raw);
        } else if (is_memory_write(raw)) {
            nav_memory_write(raw);
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

    const uint32_t cyc = m33_hw->dwt_cyccnt - cyc0;
    if (cyc > s_bus_wr_cyc_max) {
        s_bus_wr_cyc_max     = cyc;
        s_bus_wr_cyc_drained = drained;
    }
}

// --- Порты на чтение ---
//
// Детектор портов рядом с детектором памяти отдаёт байт тому же rom_serve;
// циклы MREQ и IORQ у Z80 взаимно исключены, цепочки в одной его очереди
// не сталкиваются. Цепочка: детектор -> указатель из s_porttab -> триггер
// канала выдачи ответа -> байт в rom_serve.
//
// Прерывание включает setup_ports; с DivMMC - после divmmc_reset, состояние
// карты к первому чтению готово.
void __not_in_flash_func(port_rd_isr)() {
    dma_irqn_acknowledge_channel(DMA_IRQ_INDEX_PORT_RD, s_dma_port_join);
    // Порт восстанавливается из канала указателей: там адрес записи таблицы,
    // по которой только что читали. Таблица выровнена на 1024, младшие десять
    // бит - номер порта, умноженный на четыре.
    //
    // Спешить некуда: байт ушёл на шину переносом DMA задолго до входа сюда.
    // Обработчик подставляет следующий байт (у эмулятора карты - очередной
    // байт SPI).
    const uint32_t at  = dma_hw->ch[s_dma_port_ptr].read_addr;
    const uint8_t port = table_index(at);
    if (!s_rom_emu && (at & kPortTabDosBit) == 0u && s_dos_mirror[port] == 0u) {
        s_dos_rd_done(port);
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
// команде. В очередь пишут двое: этот путь (ядро, порт 0xE3 из bus_wr_isr)
// и канал слова цепочки трапов - без темпа, слово в полную очередь
// отбрасывается молча (TXOVER, счётчик detect). Поэтому ядро ждёт запаса в
// две ячейки: одну слову цепочки в полёте, одну второму заходу ядра.
//
// Нет места дольше kVariantWaitUs - машина не читает память (сброс,
// остановка, /WAIT): самое редкое чтение у Z80 - выборка раз в ~7 мкс
// (23 такта на 3.5 МГц), четыре слова уходят за ~28 мкс. Без чтений циклов
// нет и RX пуст, чистка обоих FIFO безопасна; прежний безусловный
// pio_sm_clear_fifos при работающей машине сносил из RX адрес записи
// таблицы живого цикла - байт не выдавался, машина читала мусор.
namespace {
std::atomic<uint32_t> s_variant_waits{0};  // ожидания места в очереди детектора
std::atomic<uint32_t> s_variant_clears{0}; // чистки очереди: машина не читала дольше срока
constexpr uint32_t kVariantWaitUs = 40;

void __not_in_flash_func(queue_page_base)(uint32_t variant) {
    if (pio_sm_get_tx_fifo_level(PIO_DETECT, SM_ROM_DETECT) > 2u) {
        s_variant_waits.store(s_variant_waits.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        const uint32_t t0 = time_us_32();
        while (pio_sm_get_tx_fifo_level(PIO_DETECT, SM_ROM_DETECT) > 2u) {
            if (time_us_32() - t0 > kVariantWaitUs) {
                pio_sm_clear_fifos(PIO_DETECT, SM_ROM_DETECT);
                s_variant_clears.store(s_variant_clears.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
                break;
            }
        }
    }
    pio_sm_put(PIO_DETECT, SM_ROM_DETECT, addr32(&s_pagetabs[variant][0][0]) >> PAGE_TABLE_BASE_SHIFT);
}
} // namespace

// Слово кладётся всегда, без сверки с прошлым вариантом: трапы меняют
// вариант цепочкой DMA мимо ядра, и запомненный здесь мог уже не стоять
// (CONMEM с тем же банком после трапа выхода - запись в банк, чтение -
// машины). Лишнее слово детектор снимает на ближайшем чтении памяти.
void __not_in_flash_func(rom_emu_select_pages)(uint32_t variant) {
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
        // поэтому в карте лежит адрес ячейки, сдвинутый влево: младший бит
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

// Запись таблицы портов; при DOS тот же ответ, если порт не отдан Beta Disk.
__force_inline void tab_set(uint8_t port, uint32_t e) {
    s_porttab[port] = e;
    if (s_dos_mirror[port] != 0u) bus_porttab_dos[port] = e;
}

void __not_in_flash_func(port_answer)(uint8_t port, uint8_t value) {
    // Сначала значение, потом разрешение отвечать: наоборот между двумя
    // записями поместилось бы чтение с шины и ушло бы прежнее содержимое
    // ячейки. Ноль-ответ живёт в ячейке, ноль-отказ - в таблице.
    s_portval[port] = value;
    std::atomic_signal_fence(std::memory_order_release);
    tab_set(port, addr32(&s_portval[port]));
}

// Таблица при DOS - копия обычной; дальше обе ведёт tab_set. Зовётся до
// запуска детектора.
//
// При DOS (TRDOS, своё ПЗУ с DOS_N) порты Beta Disk - его, машины или
// платы: прочий ответ платы (Kempston на #1F) столкнулся бы с ним на шине.
void dos_ports_enable() {
    std::memcpy(bus_porttab_dos, s_porttab, sizeof(s_porttab));
    std::memset(s_dos_mirror, 1, sizeof(s_dos_mirror));
    if (!s_trdos && !s_pentagon_drive_dos) return;
    for (const uint8_t p : {0x1f, 0x3f, 0x5f, 0x7f, 0xff}) {
        s_dos_mirror[p]    = 0;
        bus_porttab_dos[p] = 0;
    }
}

// Без таблицы при DOS (режим DivMMC) - ничего: детектор DivMMC её не читает.
bool port_dos_cell(uint8_t port, const uint8_t* cell) {
    if (rom_emu_enabled()) return false;
    s_dos_mirror[port]    = cell == nullptr ? 1u : 0u;
    bus_porttab_dos[port] = cell != nullptr ? addr32(cell) : s_porttab[port];
    return true;
}

void port_dos_on_read_done(PortReadFn fn) {
    s_dos_rd_done = fn;
}

namespace {

// Хуки удержания: порт Beta Disk при DOS - один на все, обычной таблицы -
// тоже один (порт он получает аргументом): пользователя у него сейчас нет,
// таблица на 256 портов стоила 1 КБ .bss.
// Удержаний с запуска: сторож сброса не верит отметкам периода, в котором
// Z80 стоял по воле платы.
volatile uint32_t s_holds = 0;
PortReadFn s_hold_dos_fn  = nullptr;
PortReadFn s_hold_fn      = nullptr;

// Склейщик держит /WAIT. Источник флага - выключить до ответа: линия
// прерывания уровнем, а ответ может дать и поток позже.
void __not_in_flash_func(port_hold_isr)() {
    hw_clear_bits(&PIO_AUDIO->inte0, 1u << (PIO_INTR_SM0_LSB + PORT_HOLD_IRQ));
    s_holds         = s_holds + 1u;
    const auto port = static_cast<uint8_t>(sio_hw->gpio_in >> PIN_A0); // адрес стоит всё удержание
    const bool dos  = (PIO_DETECT->irq & (1u << IRQ_DOS)) != 0u && s_dos_mirror[port] == 0u;
    (dos ? s_hold_dos_fn : s_hold_fn)(port);
}

// Удержание есть только у склейщика с ним (s_hold_up). Вывод - склейщику:
// уровень 0 и вход он получил при подъёме, линию тянет вниз только
// направлением.
bool hold_ready() {
    if (!s_hold_up) return false;
    static bool s_up = false;
    if (!s_up) {
        s_up = true;
        pio_gpio_init(PIO_AUDIO, PIN_WAIT_N);
        const uint irq = pio_get_irq_num(PIO_AUDIO, 0);
        irq_set_exclusive_handler(irq, port_hold_isr);
        irq_set_priority(irq, IRQ_PRIO_PORT_RD);
        hw_set_bits(&PIO_AUDIO->inte0, 1u << (PIO_INTR_SM0_LSB + PORT_HOLD_IRQ));
        irq_set_enabled(irq, true);
    }
    return true;
}

// Удержание закодировано страницей с базой 0: склейщик с удержанием
// узнаёт её по нулевой базе.
constexpr uint32_t kHoldRoute = port_page_entry(0);

} // namespace

bool port_dos_hold(uint8_t port, PortReadFn fn) {
    if (!hold_ready()) return false;
    s_hold_dos_fn         = fn;
    s_dos_mirror[port]    = 0;
    bus_porttab_dos[port] = kHoldRoute;
    return true;
}

bool port_hold(uint8_t port, PortReadFn fn) {
    if (!hold_ready()) return false;
    s_hold_fn = fn;
    tab_set(port, kHoldRoute);
    return true;
}

void __not_in_flash_func(port_hold_answer)(uint8_t v) {
    // Цикл ввода оборвал сброс машины (IORQ поднят): Z80 стоит уже на
    // выборке команды, байт ушёл бы туда - только отпустить /WAIT.
    if ((sio_hw->gpio_in & (1u << PIN_IORQ_N)) == 0u) {
        PIO_SERVE->txf[SM_SERVE] = v;
        // /WAIT отпускается, когда байт на шине: rom_serve взял его (очередь
        // пуста) и через две команды выставил.
        while (!pio_sm_is_tx_fifo_empty(PIO_SERVE, SM_SERVE)) {
            tight_loop_contents();
        }
        busy_wait_at_least_cycles(8);
    }
    PIO_AUDIO->irq = 1u << PORT_HOLD_IRQ;
    hw_set_bits(&PIO_AUDIO->inte0, 1u << (PIO_INTR_SM0_LSB + PORT_HOLD_IRQ));
}

namespace {
volatile bool s_wait_late = false;
} // namespace

bool wait_late_ready() {
    return !s_hold_up && gpio_get_function(PIN_WAIT_N) == GPIO_FUNC_SIO;
}

void __not_in_flash_func(wait_late)(bool on) {
    if (on) {
        s_holds = s_holds + 1u;
        assert_wait();
    } else {
        release_wait();
    }
    s_wait_late = on;
}

bool port_hold_active() {
    return s_wait_late || (PIO_AUDIO->irq & (1u << PORT_HOLD_IRQ)) != 0u;
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
    tab_set(port, port_page_entry(base));
}

// Снимается запись: обнуление ячейки порт не отключило бы, склейщик отдал
// бы адрес, а канал выдачи - ноль.
void port_release(uint8_t port) {
    tab_set(port, 0u);
}

// Что порт отвечает сейчас - для журнала: снят, страница или байт ячейки.
int32_t port_answer_state(uint8_t port) {
    const uint32_t e = s_porttab[port];
    if (e == 0u) return kPortReleased;
    if (port_entry_is_page(e)) return kPortPaged;
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
std::atomic<uint8_t> s_trdos_level{0};
std::atomic<uint8_t> s_trdos_state{0};
// Фронты линий хоста: пары спад/подъём для DOS_N, NMI_N, ROM_BLK_N,
// RESET_N. Липкие защёлки EDGE_LOW/EDGE_HIGH в IO_BANK0 ловят импульс
// любой длины без включения прерываний; числа - опросов, заставших
// фронт, как у очередей трапов.
std::atomic<uint32_t> s_sig_edges[8]{};
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

// Защёлки фронтов: прочитать и погасить. Запись единиц гасит только биты
// фронтов, биты уровней от записи не меняются.
void signal_edges_poll() {
    static constexpr uint8_t kPins[] = {PIN_DOS_N, PIN_NMI_N, PIN_ROM_BLK_N, PIN_RESET_N};
    for (uint32_t i = 0; i < count_of(kPins); i++) {
        io_rw_32& intr      = io_bank0_hw->intr[kPins[i] / 8u];
        const uint32_t lsb  = (kPins[i] % 8u) * 4u;
        const uint32_t fall = intr & (1u << (lsb + 2u));
        const uint32_t rise = intr & (1u << (lsb + 3u));
        if ((fall | rise) == 0u) continue;
        intr = fall | rise;
        if (fall != 0u) count_lost(s_sig_edges[i * 2u]);
        if (rise != 0u) count_lost(s_sig_edges[i * 2u + 1u]);
    }
}

// TR-DOS: вывод DOS_N и флаг DOS - для журнала.
void trdos_poll() {
    if (!s_trdos_up) return;
    s_trdos_level.store(gpio_get(PIN_DOS_N) ? 1u : 0u, std::memory_order_relaxed);
    s_trdos_state.store((PIO_DETECT->irq & (1u << IRQ_DOS)) != 0u ? 1u : 0u, std::memory_order_relaxed);
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
    // Отметки периода с удержанием /WAIT - не сброс: M1 нет по воле платы.
    static uint32_t s_holds_seen = 0;
    if (s_holds != s_holds_seen || port_hold_active()) {
        s_holds_seen = s_holds;
        marks        = false;
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

// Своё ПЗУ и TR-DOS: pio0 занят, сторожа сброса там нет. Спады M1 считает
// блок PWM (вход B), опрос - отсюда: счётчик стоит дольше kM1ResetUs -
// машина в сбросе.
constexpr uint32_t kM1ResetUs = 50;

// Начальное состояние Пентагона - DOS включён, бит 4 ноль, порт открыт:
// страница 0.
void pentagon_state_after_reset() {
    PIO_DETECT->irq                  = (1u << IRQ_ROM48) | (1u << IRQ_PAGING_LOCKED);
    PIO_DETECT->irq_force            = 1u << IRQ_DOS;
    PIO_DETECT->txf[SM_PENTAGON_ROM] = s_pentagon_word;
    s_pentagon_locked_rom48          = false;
}

void trdos_state_after_reset() {
    PIO_DETECT->irq = (1u << IRQ_ROM48) | (1u << IRQ_PAGING_LOCKED) | (1u << IRQ_DOS) | (1u << TRDOS_IRQ_NAV);
    s_port_7ffd     = 0;
    s_nav_bank      = 0;
    if (s_nav_word != 0u) pio_sm_put(PIO_DETECT, SM_TRDOS_NAV, s_nav_word);
}

void m1_reset_poll() {
    if (s_pentagon_word == 0u && !s_trdos_up) return;
    static uint16_t s_last       = 0;
    static uint32_t s_changed_us = 0;
    static bool s_in_reset       = false;
    const uint16_t now_count     = static_cast<uint16_t>(pwm_get_counter(pwm_gpio_to_slice_num(PIN_M1_N)));
    const uint32_t now           = time_us_32();
    // Под своим /WAIT M1 нет по вине платы: это не сброс.
    if (now_count != s_last || port_hold_active()) {
        s_last       = now_count;
        s_changed_us = now;
        s_in_reset   = false;
        return;
    }
    if (s_in_reset || now - s_changed_us < kM1ResetUs) return;
    s_in_reset = true;
    if (s_rom_take_on_reset) {
        s_rom_take_on_reset = false;
        gpio_put(PIN_ROM_BLK_N, 0);
    }
    trdos_release();
    count_lost(s_machine_resets);
    const bool own = gpio_get_out_level(PIN_RESET_N);
    debug_logf("signal: machine reset seen (no M1 for %" PRIu32 " us), %s, count %" PRIu32 "\n", kM1ResetUs, own ? "held by the board" : "from the machine",
               s_machine_resets.load(std::memory_order_relaxed));
}

void trdos_release() {
    // Своё ПЗУ: после сброса Пентагон стоит в DOS, страница 0.
    if (s_pentagon_word != 0u) {
        pentagon_state_after_reset();
        return;
    }
    if (s_trdos_up) {
        trdos_state_after_reset();
    } else {
        release_host_trdos();
    }
}

// Зовётся на каждом обороте витка Core1: в SRAM, без выборки из флеша.
uint32_t __not_in_flash_func(machine_resets_seen)() {
    return s_machine_resets.load(std::memory_order_relaxed);
}

void bus_queues_poll() {
    trap_stall_poll();
    signal_edges_poll();
    reset_watch_poll();
    m1_reset_poll();
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
    m.trdos           = s_trdos_up;
    m.trdos_level     = s_trdos_level.load(std::memory_order_relaxed);
    m.trdos_asserted  = s_trdos_state.load(std::memory_order_relaxed) != 0u;
    for (uint32_t i = 0; i < count_of(m.sig_edges); i++) {
        m.sig_edges[i] = s_sig_edges[i].load(std::memory_order_relaxed);
    }
    m.variant_waits      = s_variant_waits.load(std::memory_order_relaxed);
    m.variant_clears     = s_variant_clears.load(std::memory_order_relaxed);
    m.wr_isr_cyc_max     = s_bus_wr_cyc_max;
    m.wr_isr_cyc_drained = s_bus_wr_cyc_drained;
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
