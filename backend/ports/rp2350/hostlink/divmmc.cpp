// SPDX-License-Identifier: MIT
// Эмуляция DivMMC.

#include "divmmc.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator> // std::size

#include "hardware/structs/timer.h"

#include "bus_setup.h"
#include "platform/log.h"
// SOUNDSINTH_DIVMMC_TRDOS_TRAP (без него окно 3D00 выпадает молча) и
// SOUNDSINTH_DIVMMC_TRACE. Ключи нужны препроцессору ниже, поэтому явно,
// а не транзитом через divmmc.h.
#include "firmware_config.h"
#include "platform/compiler.h"

#include "devices/sd/spi_emu.h"

// Образы ПЗУ во флеше: DivMMC и конфигуратор настроек.
#include "rom_image_config.h"
#include "rom_image_divmmc.h"

namespace bus {
namespace {

// 16 страниц ОЗУ по 8 КБ в SRAM: их читает DMA на каждом цикле памяти; из
// флеша TEST128K роняет машину, из PSRAM машина теряется через несколько
// команд (промахи кэша XIP).
//
// Таблица страниц хранит базу >> 8, значит требуется выравнивание на 256,
// а не на размер банка: внутри банка страницы по 256 байт и так ложатся по
// границе.
constexpr uint32_t kBanks = PAGE_TAB_BANKS;
// Начало региона 1 (ОЗУ DivMMC, 0x2000-0x3FFF).
constexpr uint16_t kRegion1Base = 0x2000;
static_assert((kBanks & (kBanks - 1u)) == 0, "the bank number is masked out of port 0xE3");
// Своя входная секция: скаляры выше стоят перед выровненным массивом, и в
// общем лумпе .bss этого файла они стоили бы набивку до его границы.
// Выравнивание держит alignas, секция только уводит массив из лумпа.
alignas(256) __attribute__((section(".bss.divmmc_ram"))) uint8_t s_ram[kBanks][8192];
constexpr uint32_t kRamBytes = sizeof(s_ram);

bool s_by_port = false; // CONMEM, бит 7 порта 0xE3
// Подстановка по трапам выборки: трапы ставит цепочка DMA без ядра, последнее
// сработавшее слово - вход или выход - знает её канал.
__force_inline bool by_trap() {
    return rom_emu_trap_entered();
}
// Подставлено ли: по порту (CONMEM) или по трапу.
__force_inline bool mapped() {
    return s_by_port || by_trap();
}
// Разряды порта 0xE3: CONMEM, MAPRAM; младшие - номер банка.
constexpr uint8_t kE3Conmem = 0x80u;
constexpr uint8_t kE3Mapram = 0x40u;
// Банк, который MAPRAM ставит на 0x0000-0x1FFF и защищает от записи.
constexpr uint8_t kMapramBank = 3;
uint8_t s_bank                = 0;
// MAPRAM - бит 6 порта 0xE3: вместо ПЗУ DivMMC на 0x0000-0x1FFF встаёт
// страница 3, защищённая от записи.
//
// ПЗУ копирует себя в ОЗУ (40 КБ записей в логе) и дальше исполняется
// оттуда; без MAPRAM на 0x0000 отдавался бы исходный образ.
//
// Липкий: по спецификации divIDE снимается только аппаратным сбросом.
bool s_mapram = false;

uint32_t s_remaps = 0; // повторы смены подстановки из порта 0xE3 после вклинившегося трапа

// След переключений DivMMC, переживающий сброс кнопкой: память не
// обнуляется при старте, при следующей загрузке печатается, что делал
// esxDOS перед падением машины. Входы и выходы по трапам идут цепочкой DMA
// без ядра и в след не попадают.
//
// Под ключом и выключен по умолчанию: кольцо стоит 1032 байта SRAM, а его
// печать - 440 мс каждой загрузки. Выключенный не оставляет ни массива, ни
// кода: trace_event становится пустым.
#if SOUNDSINTH_DIVMMC_TRACE
enum class TraceKind : uint8_t { None, Port, Reset, Lost };
struct TraceEvent {
    uint32_t t_us;
    uint16_t addr; // значение порта 0xE3
    TraceKind kind;
    uint8_t state; // биты 0-3 банк, 4 по порту, 5 по трапу, 6 MAPRAM - после события
};
constexpr uint32_t kTraceMagic = 0x44565454u; // смена раскладки - новый ключ
// 128 событий: в след идут только записи в порт 0xE3, сбросы и потери, а
// их на загрузке образа больше сотни тысяч - и 256, и 128 покрывают
// последние доли секунды перед остановкой машины.
constexpr uint32_t kTraceLen = 128; // степень двойки
struct Trace {
    uint32_t magic;
    uint32_t head;
    TraceEvent ev[kTraceLen];
};
Trace __uninitialized_ram(s_trace);

// После смены подстановки: срок ответа шине к этому моменту уже выдержан.
__force_inline void trace_event(TraceKind kind, uint16_t addr) {
    const uint32_t h = s_trace.head;
    TraceEvent& e    = s_trace.ev[h & (kTraceLen - 1u)];
    e.t_us           = timer_hw->timerawl;
    e.addr           = addr;
    e.kind           = kind;
    e.state          = static_cast<uint8_t>(s_bank | (s_by_port ? 0x10u : 0u) | (by_trap() ? 0x20u : 0u) | (s_mapram ? 0x40u : 0u));
    s_trace.head     = h + 1u;
}
#else
enum class TraceKind : uint8_t { None, Port, Reset, Lost };
__force_inline void trace_event(TraceKind, uint16_t) {}
#endif

// Точки входа и окно выхода DivMMC. Один список на установку и на
// самопроверку: точка, добавленная только в одно место, не проверилась бы.
constexpr uint16_t kEnterTraps[] = {0x0000u, 0x0008u, 0x0038u, 0x0066u, 0x04C6u, 0x0562u};
constexpr uint16_t kExitFirst    = 0x1FF8u;
constexpr uint16_t kExitLast     = 0x1FFFu;

// Карта трапов цепочки DMA: точки входа и окно выхода - по адресу, окно BDI
// - страницей.
void build_trap_map() {
    rom_emu_trap_clear();
    for (const uint16_t a : kEnterTraps) {
        rom_emu_trap_set(a, TrapKind::Enter);
    }
    for (uint16_t a = kExitFirst; a <= kExitLast; ++a) {
        rom_emu_trap_set(a, TrapKind::Exit);
    }
#if SOUNDSINTH_DIVMMC_TRDOS_TRAP
    // Всё окно 3D00-3DFF, а не две точки входа TR-DOS, как у divIDE:
    // betadisk.sys может поставить обработчики где угодно в окне. Вход при
    // стоящей подстановке кладёт тот же вариант.
    rom_emu_trap_set(static_cast<uint16_t>(kBdiWindow), TrapKind::Enter);
#endif

    // Самопроверка двухуровневой раскладки: ошибка индекса - panic на
    // загрузке, а не неверная выборка у машины.
    uint32_t enter = 0u;
    for (const uint16_t a : kEnterTraps) {
        const uint32_t v = rom_emu_trap_resolve(a);
        if (v == 0u) panic("divmmc: enter trap 0x%04x lost in the two-level map", static_cast<unsigned>(a));
        if (enter == 0u) {
            enter = v;
        } else if (v != enter) {
            panic("divmmc: enter trap 0x%04x resolves elsewhere", static_cast<unsigned>(a));
        }
    }
    for (uint16_t a = kExitFirst; a <= kExitLast; ++a) {
        const uint32_t v = rom_emu_trap_resolve(a);
        if (v == 0u) panic("divmmc: exit trap 0x%04x lost in the two-level map", static_cast<unsigned>(a));
        if (v == enter) panic("divmmc: exit trap 0x%04x resolves to the enter word", static_cast<unsigned>(a));
    }
#if SOUNDSINTH_DIVMMC_TRDOS_TRAP
    // Окно BDI - через карту страниц: тот же вход, что у точек выше.
    if (rom_emu_trap_resolve(static_cast<uint16_t>(kBdiWindow)) != enter) {
        panic("divmmc: BDI window 0x%04x does not resolve to the enter word", static_cast<unsigned>(kBdiWindow));
    }
#endif
    // Где трапа нет, путь обязан давать ноль. 0x07F8 совпадает с окном
    // выхода по разрядам A0..A10.
    for (const uint16_t a : {0x0001u, 0x0007u, 0x0100u, 0x07F8u, 0x0FF8u, 0x1000u, 0x1FF7u}) {
        if (rom_emu_trap_resolve(a) != 0u) {
            panic("divmmc: 0x%04x traps though it must not", static_cast<unsigned>(a));
        }
    }
}

// Порт DivMMC. При OUT (n),A младший байт адреса - n, при OUT (C),A -
// регистр C: искать в младшем байте.
constexpr uint8_t kPortDivMmc = 0xe3;

// Порты карты у DivMMC:
//   0xE7 запись - бит 0 выбирает кристалл, ноль выбирает;
//   0xEB запись - байт карте;
//   0xEB чтение - байт от карты, операция запускает следующий обмен.
// Отличие от Z-Controller - номера и бит выбора (там бит 1). Разбор и
// эмуляция карты общие.
constexpr uint8_t kPortSdCtrl = 0xe7;
constexpr uint8_t kPortSdData = 0xeb;

// Выбрана ли карта. Пока не выбрана, записи в порт данных игнорируются,
// как на шине SPI.
bool s_sd_active = false;

// Копия образа ПЗУ в SRAM (оригинал - константа во флеше): цепочка DMA
// отвечает из SRAM, из флеша машина падает. Выравнивание на 256 байт
// обязательно: таблица страниц хранит базу >> 8.
//
// Возврат из ПЗУ машины по 0x3DFD (прошивка DivMMC по 0x0CD2 кладёт адрес
// 3DFD в стек и уходит в ПЗУ машины) попадает на наш C9 в том же цикле:
// M1 - разряд индекса таблицы страниц, выборка команды в окне 3D00-3DFF
// идёт из нашей памяти, чтение данных - из ПЗУ машины.
alignas(256) __attribute__((section(".bss.divmmc_rom"))) uint8_t s_rom[8192];

// Из флеша читается один memcpy, когда машина стоит в сбросе. В режиме
// конфигуратора берётся другой образ: механизм подстановки тот же, ПЗУ
// другое.
void build_rom() {
    std::memcpy(s_rom, config_rom_enabled() ? kConfigRom : kDivMmcRom, sizeof(s_rom));
}

// Номер варианта таблицы - всё состояние DivMMC одним числом:
//
//   2b+1        подстановки нет,   банк b
//   2b          подставлены,       банк b, в регионе 0 образ ПЗУ
//   2(b+16)     подставлены,       банк b, MAPRAM: в регионе 0 банк 3
//
// Чётность - ROM_BLK_N: детектор выводит младший разряд базы варианта на
// пин, и ПЗУ машины гаснет и включается в начале того же цикла, с которого
// действует вариант.
//
// Номер считается, а не хранится: на горячем пути меняется только он,
// одним словом в очередь детектора.
constexpr uint32_t variant_of(bool on, bool mapram, uint32_t bank) {
    if (!on) return 2u * bank + 1u;
    return 2u * (mapram ? kBanks + bank : bank);
}
static_assert(variant_of(true, true, kBanks - 1u) < PAGE_TAB_VARIANTS, "the variant layout");
static_assert(variant_of(false, false, kBanks - 1u) < PAGE_TAB_VARIANTS, "the variant layout");
// Нечётные варианты старше 2 * kBanks - дыры с картами трапов: выбор такого
// отдал бы детектору чужие слова как записи страниц.
static_assert(!pagetab_is_hole(variant_of(false, false, kBanks - 1u)) && !pagetab_is_hole(variant_of(true, true, kBanks - 1u)),
              "a DivMMC variant falls into a page table hole");
static_assert(variant_of(false, false, 0) % 2u == 1u && variant_of(true, false, 0) % 2u == 0u && variant_of(true, true, 0) % 2u == 0u,
              "the parity of the variant is ROM_BLK_N");

// Ветка раньше чтения s_mapram: так обработчики шины короче на команды.
__force_inline uint32_t pagetab_variant(bool on) {
    if (!on) return variant_of(false, false, s_bank);
    return variant_of(true, s_mapram, s_bank);
}

// Слова, которые кладут трапы: вход - подстановка с текущими банком и
// MAPRAM; выход - снятие, но CONMEM держит подстановку и после него.
void __not_in_flash_func(update_trap_variants)() {
    const uint32_t on = variant_of(true, s_mapram, s_bank);
    rom_emu_trap_variants(on, s_by_port ? on : variant_of(false, false, s_bank));
}

// Заполнить все варианты. Один раз, при сбросе; в работе меняется только
// номер варианта.
void build_pagetabs() {
    for (uint32_t b = 0; b < kBanks; ++b) {
        // --- Подстановки нет ---
        // Регионы за машиной, кроме одной страницы: окно BDI 0x3D00-0x3DFF
        // отдаётся нам по выборке команды и здесь (вход через 3Dxx у divIDE
        // немедленный). Чтение данных оттуда остаётся ПЗУ машины
        // (знакогенератор).
        //
        // 0x3D00 - в регионе 1, поэтому и "выключенный" вариант зависит от
        // банка.
        for (uint32_t r = 0; r < 2u; ++r) {
            rom_emu_fill_pages(variant_of(false, false, b), r, nullptr);
        }
#if SOUNDSINTH_DIVMMC_TRDOS_TRAP
        rom_emu_set_fetch_page(variant_of(false, false, b), kBdiWindow >> 8, &s_ram[b][kBdiWindow - kRegion1Base]);
#endif

        // --- Подставлены ---
        for (uint32_t m = 0; m < 2u; ++m) {
            const uint32_t v = variant_of(true, m != 0u, b);
            rom_emu_fill_pages(v, 0, m ? static_cast<const void*>(&s_ram[kMapramBank][0]) : static_cast<const void*>(s_rom));
            rom_emu_fill_pages(v, 1, &s_ram[b][0]);
        }
    }
}

void __not_in_flash_func(apply_mapping)() {
    const bool on = mapped();

    // Со следующего цикла памяти.
    wait_memory_read_end();

    // Смена подстановки - смена варианта таблицы, одна запись в FIFO
    // детектора. ПЗУ машины он гасит и включает сам, по чётности варианта.
    // Линия NMI не трогается: её подаёт кнопка.
    rom_emu_select_pages(pagetab_variant(on));
}

// Смена подстановки из порта 0xE3 (bus_wr_isr). Трап (цепочка DMA) может
// переключить её посреди: выход esxDOS пишет 0xE3 и через около 21
// такта выбирает 1FFA (ПЗУ 0x005D, 0x0D0C). Состояние читается после
// ожидания конца цикла - трап, пришедший в ожидании, уже учтён; вклинился
// после чтения - X детектора пишется заново, в обход запомненного
// варианта. Иначе состояние "снято", а таблица (и с ней ROM_BLK) "стоит" до
// следующего трапа входа: машина выходила бы в память DivMMC при погашенном
// ПЗУ.
void __not_in_flash_func(apply_mapping_from_port)(bool bank_only) {
    // Меняется только банк (подстановка и MAPRAM те же): у вариантов одной
    // группы область 0x0000-0x1FFF одна и та же, выборка следующей команды
    // оттуда смены не заметит, а чтение следующей командой из 0x2000-0x3FFF
    // (копирование между банками: out (e3),a / ld a,(hl)) обязано уже видеть
    // новый банк - запаса до него около 1 мкс на 3.5 МГц. Ожидание конца
    // цикла съедало половину. Смена подстановки ждёт, как трап.
    if (!bank_only) wait_memory_read_end();
    // Подставлено ли, с перечитыванием s_by_port из памяти: после ожидания
    // и после записи варианта, когда между чтениями мог сработать трап.
    auto mapped_fresh = [] {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return *const_cast<volatile bool*>(&s_by_port) || by_trap();
    };
    bool on = mapped_fresh();
    rom_emu_select_pages(pagetab_variant(on));
    for (;;) {
        const bool now = mapped_fresh();
        if (now == on) return;
        on = now;
        ++s_remaps;
        rom_emu_force_pages(pagetab_variant(on));
    }
}

// Ответ карты в ячейку порта данных. Ячейку пишут обработчики записи и
// чтения порта; вытесненный между обменом и записью положил бы свой, более
// ранний ответ поверх свежего. Запись повторяется, пока ячейка не совпадёт
// с ответом последнего исполненного обмена.
void __not_in_flash_func(sd_answer)(uint8_t value) {
    for (;;) {
        port_answer(kPortSdData, value);
        const uint8_t now = devices::sd::sd_spi_latch(devices::sd::SdOwner::DivMmc);
        if (now == value) return;
        value = now;
    }
}

} // namespace

// Порт данных карты - следующий байт SPI (как у Z-Controller).
void __not_in_flash_func(divmmc_on_port_read)(uint8_t port) {
    if (port != kPortSdData) {
        // Не наш порт - разбирает плагин.
        dispatch_port_read_done(port);
        return;
    }
    sd_answer(devices::sd::sd_spi_byte(devices::sd::SdOwner::DivMmc, 0xff));
}

void divmmc_reset() {
    build_trap_map();
    build_rom();

    // Эмулятор SD общий с Z-Controller; в режиме DivMMC его поднимает этот
    // сброс.
    devices::sd::sd_spi_emu_init();
    s_sd_active = false;
    devices::sd::sd_spi_select(devices::sd::SdOwner::DivMmc, false);
    // Первый ответ до обмена - 0xFF, как у незанятой шины SPI.
    port_answer(kPortSdData, 0xffu);

    std::memset(s_ram, 0, kRamBytes);
    build_pagetabs();
    // В конфигураторе CONMEM взведён с самого начала: машина обязана
    // стартовать в наше ПЗУ, а не ждать трапа выборки.
    s_by_port = config_rom_enabled();
    rom_emu_trap_reset_state();
    s_bank   = 0;
    s_mapram = false;
    update_trap_variants();
    apply_mapping();
    trace_event(TraceKind::Reset, 0);
}

void __not_in_flash_func(divmmc_on_bus_write)(uint32_t raw) {
    const uint16_t addr = addr_of(raw);
    const uint8_t data  = data_of(raw);

    // Запись в память или в порт различает разряд IORQ в слове (bus_wr
    // снимает весь порт ввода). Записи в память приходят только ниже 0x4000:
    // ОЗУ машины автомат отсекает по A14/A15, остаётся около трёх тысяч в
    // секунду.
    if (is_memory_write(raw)) {
        // Подстановка берётся из s_by_port и трапа на момент разбора, а не
        // из захваченного слова. Трап может переключить её между циклом
        // записи и разбором. В слове есть ROM_BLK_N (GPIO 1, низкий -
        // подставлены), но читается ли в нём наш же выход, не проверено.
        // Банк упорядочен с записями: 0xE3 идёт через эту же очередь.
        if (!mapped()) return;
        if (addr < kRegion1Base) return;
        // MAPRAM защищает свой банк и в регионе 1.
        if (s_mapram && s_bank == kMapramBank) return;
        s_ram[s_bank][addr - kRegion1Base] = data;
        return;
    }

    // Порты карты - только в ветке портов: запись в память с тем же младшим
    // байтом (0x20E7) иначе ушла бы эмулятору карты как байт SPI.
    const uint8_t port = port_of(raw);
    if (port == kPortSdCtrl) {
        s_sd_active = (data & 0x01u) == 0u;
        devices::sd::sd_spi_select(devices::sd::SdOwner::DivMmc, s_sd_active);
        return;
    }
    if (port == kPortSdData) {
        if (s_sd_active) sd_answer(devices::sd::sd_spi_byte(devices::sd::SdOwner::DivMmc, data));
        return;
    }

    // Чужой порт - плагину: в режиме DivMMC у него нет своих автоматов шины,
    // его порты обслуживает этот путь.
    if (port != kPortDivMmc) {
        dispatch_port_write(port, data);
        return;
    }

    // Бит 7 - CONMEM, младшие четыре - номер страницы, бит 6 - MAPRAM
    // (липкий).
    const bool was_on     = mapped();
    const bool was_mapram = s_mapram;
    s_by_port             = (data & kE3Conmem) != 0u;
    if ((data & kE3Mapram) != 0u) s_mapram = true; // липкий
    s_bank = static_cast<uint8_t>(data & (kBanks - 1u));
    // Сначала слова трапов: трап, сработавший после этой строки, кладёт уже
    // новый банк.
    update_trap_variants();
    // Таблицы не трогаются: всё состояние - номер варианта, смена - одно
    // слово в очередь детектора.
    // Подстановка и MAPRAM те же - меняется только банк, ждать конца цикла
    // не надо.
    const bool bank_only = mapped() == was_on && s_mapram == was_mapram;
    apply_mapping_from_port(bank_only);
    trace_event(TraceKind::Port, data);
}

bool divmmc_mapped() {
    return mapped();
}
uint8_t divmmc_bank() {
    return s_bank;
}

#if SOUNDSINTH_DIVMMC_TRACE
// Из bus_wr_isr (SRAM), на RXSTALL.
void __not_in_flash_func(divmmc_note_lost)(uint32_t drained) {
    trace_event(TraceKind::Lost, static_cast<uint16_t>(drained));
}
#endif
uint32_t divmmc_remaps() {
    return s_remaps;
}

void divmmc_log_stats() {
    // divmmc: подстановка (1 - ПЗУ машины закрыто), банк, адрес последней
    // выборки.
    debug_logf("divmmc: mapped %d, bank %u | fetch %08" PRIX32 " | E3 repeats %" PRIu32 " | queue peak %" PRIu32 "/8\n", static_cast<int>(divmmc_mapped()),
               static_cast<unsigned>(divmmc_bank()), last_memory_read_ptr(), divmmc_remaps(), bus_wr_peak());
    // Ноль - ни одна отметка опоздания не потеряна.
    debug_logf("bus: late queue full %" PRIu32 "\n", bus_measurements().serve_late_full);
}

#if SOUNDSINTH_DIVMMC_TRACE
void divmmc_trace_report() {
    static constexpr const char* kKindNames[] = {"?", "port E3", "reset", "LOST"};
    if (s_trace.magic == kTraceMagic) {
        const uint32_t head = s_trace.head;
        const uint32_t n    = head < kTraceLen ? head : kTraceLen;
        std::printf("divmmc: trace from the previous run - events %lu, printing the last %lu\n", static_cast<unsigned long>(head),
                    static_cast<unsigned long>(n));
        const uint32_t last_t = n ? s_trace.ev[(head - 1u) & (kTraceLen - 1u)].t_us : 0u;
        for (uint32_t i = head - n; i != head; ++i) {
            const TraceEvent& e = s_trace.ev[i & (kTraceLen - 1u)];
            const uint8_t k     = static_cast<uint8_t>(e.kind);
            std::printf("  -%lu ms %s %04X | bank %u%s%s%s\n", static_cast<unsigned long>((last_t - e.t_us) / 1000u),
                        k < std::size(kKindNames) ? kKindNames[k] : "?", static_cast<unsigned>(e.addr), static_cast<unsigned>(e.state & 0x0fu),
                        (e.state & 0x10u) ? " by port" : "", (e.state & 0x20u) ? " by trap" : "", (e.state & 0x40u) ? " MAPRAM" : "");
        }
    }
    s_trace.magic = kTraceMagic;
    s_trace.head  = 0;
}
#endif

// Страница настроек - банк 0: конфигуратор его не переключает, и первый
// банк виден машине сразу после сброса.
uint8_t* config_rom_page() {
    return &s_ram[0][0];
}

uint32_t config_rom_page_bytes() {
    return sizeof(s_ram[0]);
}

} // namespace bus
