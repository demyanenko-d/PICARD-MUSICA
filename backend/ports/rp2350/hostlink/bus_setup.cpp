// SPDX-License-Identifier: MIT
// Монтаж шины: раздача автоматов PIO, цепочки DMA, прерывания. Логика - в
// bus.cpp, развязка - bus_setup.h.
//
// Порядок разделов как в bus.cpp: плагинная шина, затем эмуляция ПЗУ и
// памяти.

#include "bus_setup.h"

#include <cinttypes>

#include <cstring>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include <iterator> // std::size

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico.h"

#include "platform/log.h"
#include "divmmc.h"

// Порядок программ PIO во флеше - порядок этих строк.
#include "ports.pio.h"
#include "memory.pio.h"
#include "trap_join.pio.h"
#include "reset_watch.pio.h"
#include "trdos.pio.h"
// Только для счёта команд ниже: программа звука грузится в i2s_sink.
#include "i2s_pio.pio.h"

namespace bus {

// Память команд PIO - 32 на блок. Лишняя команда даёт panic в
// pio_add_program, и не на загрузке: i2s_out грузится последним, на
// первом треке, и только в части режимов. Счёт - по режимам и блокам.
template <size_t... N>
constexpr size_t pio_words([[maybe_unused]] const uint16_t (&... p)[N]) {
    return (N + ... + 0u);
}
inline constexpr size_t kPioWords = 32;
// DivMMC: pio0 - сборка адреса ПЗУ и трапы; pio1 - захват записей, детектор
// портов, ответчик; pio2 - склейщики трапов и портов и звук.
static_assert(pio_words(rom_detect_program_instructions, rom_join_program_instructions, rom_trap_program_instructions) <= kPioWords,
              "PIO0 instruction memory overflows in the DivMMC mode");
static_assert(pio_words(bus_wr_program_instructions, port_detect_program_instructions, rom_serve_program_instructions) <= kPioWords,
              "PIO1 instruction memory overflows");
static_assert(pio_words(trap_join_program_instructions, trap_route_program_instructions, port_join_program_instructions, i2s_out_program_instructions) <=
                  kPioWords,
              "PIO2 instruction memory overflows in the DivMMC mode");
// Без DivMMC: pio0 - триггер TR-DOS; pio2 - склейщик портов, привод TR-DOS и
// звук.
static_assert(pio_words(trdos_detect_program_instructions, reset_watch_program_instructions) <= kPioWords,
              "PIO0 instruction memory overflows in the TR-DOS mode");
static_assert(pio_words(port_join_program_instructions, trdos_drive_program_instructions, i2s_out_program_instructions) <= kPioWords,
              "PIO2 instruction memory overflows in the TR-DOS mode");

// Номера управляющих линий зашиты в wait gpio программ PIO: смена пина
// должна ломать сборку, а не шину.
static_assert(PIN_M1_N == Z80_M1_N_GPIO && PIN_WR_N == Z80_WR_N_GPIO && PIN_RD_N == Z80_RD_N_GPIO && PIN_IORQ_N == Z80_IORQ_N_GPIO &&
                  PIN_MREQ_N == Z80_MREQ_N_GPIO,
              "the Z80 line numbers disagree with the wait gpio in ports.pio and memory.pio");
// Сдвиги снимков пинов в rom_detect и bus_wr считаются от A0 и A8.
static_assert(PIN_A0 == A0_GPIO && PIN_A8 == A8_GPIO, "the address pins disagree with A0_GPIO and A8_GPIO in ports.pio and memory.pio");
// Окно выводов блока, который ведёт DOS_N, - 16..47.
static_assert(PIN_DOS_N >= 16u && PIN_DOS_N <= 47u, "DOS_N is outside the pin window of the audio block");

// --- Общее для обоих режимов: пины и каналы DMA ---
//
// Электрика шины одна на оба режима: плагинный и эмуляции ПЗУ отличаются
// только блоком PIO и автоматом, которому отдаются пины.

namespace {

// Управляющие сигналы (M1_N..MREQ_N) и адресная шина A0..A15 явно
// переводятся в SIO-входы без подтяжек до настройки PIO. Без этого пины
// остаются в произвольном состоянии после сброса, PIO ловит рваный цикл на
// старте и замолкает - плата не отвечает хосту. PIO читает их через
// границу блока: для входа пин не обязан принадлежать этому PIO. Подтяжки
// нужных линий ставит вызывающий.
void bus_inputs_to_sio() {
    for (uint pin = PIN_M1_N; pin <= PIN_MREQ_N; ++pin) {
        gpio_init(pin);
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
    }
    for (uint pin = PIN_A0; pin < PIN_A0 + ADDR_LINES; ++pin) {
        gpio_init(pin);
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
    }
}

// D0..D7 отдаются блоку, который выставляет байт, пока входами.
//
// Ток 12 мА и быстрый фронт, и у OE_N: с умолчанием (4 мА, медленный) уровень
// не успевает к защёлкиванию, и Z80 изредка берёт 0xFF.
void data_pins_to_pio(PIO pio) {
    for (uint pin = PIN_D0; pin < PIN_D0 + DATA_LINES; ++pin) {
        pio_gpio_init(pio, pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }
    gpio_set_drive_strength(PIN_BUFF_OE_N, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(PIN_BUFF_OE_N, GPIO_SLEW_RATE_FAST);
}

// Передача OE_N от SIO к PIO без глитча: иначе в момент передачи pindir
// PIO на мгновение вход, OE_N плавает и может открыть буфер данных на шину
// при старте. Звать, когда OE_N - выход SIO в единице.
//
// 1. Выходной регистр side-set PIO - значение из side_set (кодировка
//    side-set своя у каждой программы), пока пин ещё SIO.
// 2. PINDIR[OE_N] = выход, пока пин ещё SIO.
// 3. Только теперь пин передаётся PIO: уровень из шага 1 без глитча.
void oe_handoff(PIO pio, uint sm, uint side_set) {
    pio_sm_exec_wait_blocking(pio, sm, pio_encode_nop() | side_set);
    pio_sm_set_pindirs_with_mask(pio, sm, (1u << PIN_BUFF_OE_N), (1u << PIN_BUFF_OE_N) | (0xffu << PIN_D0));
    pio_gpio_init(pio, PIN_BUFF_OE_N);
}

// Выдающий канал цепочки: DREQ нет (его запускает цепочка или запись в
// триггерный регистр), сам на себя - без цепочки. Высокий приоритет: каналы
// шины в критическом окне ответа Z80 (нет /WAIT, срок около 825 нс), а
// движок DMA делят с каналами I2S, которые льют непрерывно. У звука есть
// буферизация, у шины нет.
void setup_data_channel(uint ch, volatile void* txf, dma_channel_transfer_size_t data_size) {
    dma_channel_config_t dc = dma_channel_get_default_config(ch);
    channel_config_set_dreq(&dc, DREQ_FORCE);
    channel_config_set_high_priority(&dc, true);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_transfer_data_size(&dc, data_size);
    channel_config_set_chain_to(&dc, ch);
    dma_channel_configure(ch, &dc, txf, nullptr, 1, false);
}

// Пара каналов, адресный и выдающий. Адресный по DREQ автомата читает из
// его RX FIFO адрес записи таблицы и пишет его выдающему; сам перевзводится
// (TRIGGER_SELF). Выдающий стоит до следующего запуска, иначе он выстрелил
// бы вторым словом по отработанному read_addr, и получатель уехал бы на
// цикл вперёд.
//
// Выдающего запускает сама запись адреса в его al3_read_addr_trig. Ноль в
// триггерный регистр канал не запускает, но в read_addr записывается.
void setup_addr_channel(uint ch_addr, volatile void* dest, const volatile void* rxf, uint dreq) {
    dma_channel_config_t dc = dma_channel_get_default_config(ch_addr);
    channel_config_set_dreq(&dc, dreq);
    channel_config_set_high_priority(&dc, true);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    dma_channel_configure(ch_addr, &dc, dest, rxf, dma_encode_transfer_count_with_self_trigger(1), false);
}

// Канал "очередь одного автомата -> очередь другого". Перевзводится сам:
// получателей у него нет, а работает он всю жизнь платы.
void setup_fifo_channel(uint ch, volatile void* txf, const volatile void* rxf, uint dreq) {
    dma_channel_config_t dc = dma_channel_get_default_config(ch);
    channel_config_set_dreq(&dc, dreq);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    dma_channel_configure(ch, &dc, txf, rxf, dma_encode_transfer_count_with_self_trigger(1), false);
    dma_channel_start(ch);
}

void setup_chain(uint ch_addr, uint ch_data, const volatile void* rxf, volatile void* txf, uint dreq, dma_channel_transfer_size_t data_size) {
    setup_addr_channel(ch_addr, &dma_hw->ch[ch_data].al3_read_addr_trig, rxf, dreq);
    setup_data_channel(ch_data, txf, data_size);
    dma_channel_start(ch_addr);
}

// DMA выше обоих ядер на шинной матрице: high_priority каналов решает только
// очередь внутри DMA, к SRAM движок идёт по кругу с ядрами. У ядер срока нет,
// у цепочки шины - 570-825 нс без /WAIT.
void dma_over_cores() {
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
}

// --- Эмуляция ПЗУ ---

// Последовательность подъёма шины. Шину поднимает одна из двух, и от неё
// же зависят подтяжки, ветка памяти захвата записи и хозяин канала выдачи
// байта:
//   RomEmulation - эмуляция ПЗУ (DivMMC или конфигуратор), rom_emu_init;
//   PortsOnly    - только порты, без эмуляции памяти (plugin_ports_init).
enum class BringUp : uint8_t { RomEmulation, PortsOnly };

// PortsOnly - ещё подтяжки линий цикла порта.
void setup_gpio(BringUp mode) {
    bus_inputs_to_sio();

    gpio_pull_up(PIN_RD_N);
    if (mode == BringUp::PortsOnly) {
        gpio_pull_up(PIN_IORQ_N);
        gpio_pull_up(PIN_WR_N);
    } else {
        gpio_pull_up(PIN_MREQ_N);
    }

    // OE_N - SIO в единице, пока автомат не настроен; передача пина PIO -
    // oe_handoff в rom_emu_init.
    gpio_init(PIN_BUFF_OE_N);
    gpio_put(PIN_BUFF_OE_N, 1);
    gpio_set_dir(PIN_BUFF_OE_N, GPIO_OUT);

    data_pins_to_pio(PIO_SERVE);
}

} // namespace

namespace {

// RomEmulation - ловить и записи в память ниже 0x4000. PortsOnly - ветка
// памяти заменяется переходом к концу цикла: записи в ОЗУ машины
// прерывания не будят.
void setup_bus_wr(BringUp mode) {
    uint16_t code[sizeof(bus_wr_program_instructions) / sizeof(bus_wr_program_instructions[0])];
    for (uint32_t k = 0; k < std::size(code); ++k) {
        code[k] = bus_wr_program_instructions[k];
    }
    if (mode == BringUp::PortsOnly) code[bus_wr_offset_mem] = static_cast<uint16_t>(pio_encode_jmp(bus_wr_offset_done));
    pio_program_t prog = bus_wr_program;
    prog.instructions  = code;
    const uint off     = pio_add_program(PIO_WATCH, &prog);
    pio_sm_claim(PIO_WATCH, SM_BUS_WRITE);
    bus_wr_program_init(PIO_WATCH, SM_BUS_WRITE, off);

    const uint irq = pio_get_irq_num(PIO_WATCH, 0);
    pio_set_irq0_source_enabled(PIO_WATCH, pis_interrupt0, true);
    irq_set_exclusive_handler(irq, bus_wr_isr);
    // 0x40: выше чтения портов. Запись должна успеть до чтения того же байта,
    // около 1.7 мкс: ПЗУ DivMMC проверяет ОЗУ (inc (hl) / cp (hl) по 0x3D00).
    irq_set_priority(irq, IRQ_PRIO_BUS_WR);
    irq_set_enabled(irq, true);

    pio_sm_set_enabled(PIO_WATCH, SM_BUS_WRITE, true);
}

void setup_trap() {
    // rom_trap в pio0 рядом с детектором чтения (8 команд с отсевом A14/A15):
    // адрес выборки ниже 0x4000 - в очередь, её забирает канал, не ядро.
    const uint off = pio_add_program(PIO_DETECT, &rom_trap_program);
    pio_sm_claim(PIO_DETECT, SM_ROM_TRAP);
    rom_trap_program_init(PIO_DETECT, SM_ROM_TRAP, off, PIN_A0);

    // Оба склейщика в блоке звука: пины шины им нужны только A8..A13 (база
    // GPIO 16 стоит с загрузки платы). Места там хватает именно в режиме
    // DivMMC: trdos_drive заводится только без него - дисковая система одна.
    const uint off_join = pio_add_program(PIO_AUDIO, &trap_join_program);
    s_sm_trap_join      = static_cast<int>(pio_claim_unused_sm(PIO_AUDIO, true));
    const auto join_sm  = static_cast<uint>(s_sm_trap_join);
    trap_join_program_init(PIO_AUDIO, join_sm, off_join, PIN_A8, PIN_A8 + 5u, reinterpret_cast<uintptr_t>(s_trap_lo),
                           reinterpret_cast<uintptr_t>(trap_page_map()));

    // Разборщик маршрута и второй уровень: читает запись первого уровня и
    // либо дописывает её старшими битами адреса, либо отдаёт как есть.
    const uint off_route = pio_add_program(PIO_AUDIO, &trap_route_program);
    s_sm_trap_route      = static_cast<int>(pio_claim_unused_sm(PIO_AUDIO, true));
    const auto route_sm  = static_cast<uint>(s_sm_trap_route);
    trap_route_program_init(PIO_AUDIO, route_sm, off_route, PIN_A8);

    s_dma_trap_in      = dma_claim_unused_channel(true);
    s_dma_trap_addr    = dma_claim_unused_channel(true);
    s_dma_trap_entry   = dma_claim_unused_channel(true);
    s_dma_trap_addr2   = dma_claim_unused_channel(true);
    s_dma_trap_entry2  = dma_claim_unused_channel(true);
    s_dma_trap_word    = dma_claim_unused_channel(true);
    s_dma_trap_note    = dma_claim_unused_channel(true);
    const auto ch_in   = static_cast<uint>(s_dma_trap_in);
    const auto ch_word = static_cast<uint>(s_dma_trap_word);

    // Слово варианта - в очередь детектора; до первого трапа стоит на слове
    // "выйти" (подстановки по трапу нет).
    setup_data_channel(ch_word, &PIO_DETECT->txf[SM_ROM_DETECT], DMA_SIZE_32);
    // Отметка сработавшего трапа: по окончании слова его адрес (read_addr
    // канала слова, без приращения) - в s_trap_last. Пустой триггер канал
    // слова не запускает, отметку тоже.
    const auto ch_note = static_cast<uint>(s_dma_trap_note);
    setup_data_channel(ch_note, &s_trap_last, DMA_SIZE_32);
    dma_channel_set_read_addr(ch_note, &dma_hw->ch[ch_word].read_addr, false);
    {
        dma_channel_config_t wc = dma_get_channel_config(ch_word);
        channel_config_set_chain_to(&wc, ch_note);
        dma_channel_set_config(ch_word, &wc, false);
    }
    // Второй уровень: разборщик -> указатель на слово -> триггер канала
    // слова (ноль - стоп). Ставится раньше первого: первый кладёт в очередь
    // разборщика, и к тому времени тот должен быть обслужен.
    setup_chain(static_cast<uint>(s_dma_trap_addr2), static_cast<uint>(s_dma_trap_entry2), &PIO_AUDIO->rxf[route_sm], &dma_hw->ch[ch_word].al3_read_addr_trig,
                pio_get_dreq(PIO_AUDIO, route_sm, false), DMA_SIZE_32);
    // Первый уровень: склейщик -> запись карты -> очередь разборщика.
    setup_chain(static_cast<uint>(s_dma_trap_addr), static_cast<uint>(s_dma_trap_entry), &PIO_AUDIO->rxf[join_sm], &PIO_AUDIO->txf[route_sm],
                pio_get_dreq(PIO_AUDIO, join_sm, false), DMA_SIZE_32);
    // Адрес выборки: rom_trap -> склейщик. Устроен как адресный канал
    // цепочки (высокий приоритет, сам перевзводится), запускается сразу.
    setup_addr_channel(ch_in, &PIO_AUDIO->txf[join_sm], &PIO_DETECT->rxf[SM_ROM_TRAP], pio_get_dreq(PIO_DETECT, SM_ROM_TRAP, false));
    dma_channel_start(ch_in);

    pio_sm_set_enabled(PIO_AUDIO, join_sm, true);
    pio_sm_set_enabled(PIO_AUDIO, route_sm, true);
    pio_sm_set_enabled(PIO_DETECT, SM_ROM_TRAP, true);
}

void setup_ports(BringUp mode) {
    const uint off_det = pio_add_program(PIO_WATCH, &port_detect_program);
    pio_sm_claim(PIO_WATCH, SM_PORT_DETECT);
    port_detect_program_init(PIO_WATCH, SM_PORT_DETECT, off_det, PIN_RD_N, PIN_A0, reinterpret_cast<uintptr_t>(s_porttab));

    // Склейщик - в блоке звука: из шины ему нужны только A8..A15, а они в
    // окне базы GPIO 16, которую ставит i2s_sink_set_block_base до этого
    // места. Запускает склейщик очередь, а не шина, линии управления ему не
    // нужны - в блоках с базой 0 места под программу не осталось.
    const uint off_join = pio_add_program(PIO_AUDIO, &port_join_program);
    s_sm_port_join      = static_cast<int>(pio_claim_unused_sm(PIO_AUDIO, true));
    const uint join_sm  = static_cast<uint>(s_sm_port_join);
    port_join_program_init(PIO_AUDIO, join_sm, off_join, PIN_A8);

    s_dma_port_addr = dma_claim_unused_channel(true);
    s_dma_port_ptr  = dma_claim_unused_channel(true);
    s_dma_port_join = dma_claim_unused_channel(true);

    // Канал выдачи байта - общий с чтением памяти: циклы MREQ и IORQ у Z80
    // взаимно исключены, и обе цепочки уже льют в одну очередь rom_serve.
    // С эмуляцией ПЗУ канал завёл setup_memory_read; без неё чтения памяти
    // нет, канал берётся свой.
    if (mode == BringUp::PortsOnly) {
        s_dma_byte_data = dma_claim_unused_channel(true);
        setup_data_channel(s_dma_byte_data, &PIO_SERVE->txf[SM_SERVE], DMA_SIZE_8);
    }

    // Уровень 1: детектор дал &porttab[порт]; оттуда читается запись и
    // уходит склейщику. Нулевую он отбрасывает сам - так решается "порт не
    // наш".
    setup_chain(s_dma_port_addr, s_dma_port_ptr, &PIO_WATCH->rxf[SM_PORT_DETECT], &PIO_AUDIO->txf[join_sm], pio_get_dreq(PIO_WATCH, SM_PORT_DETECT, false),
                DMA_SIZE_32);

    // Уровень 2: склейщик дал адрес ячейки, запись в триггерный регистр
    // запускает выдачу байта.
    setup_addr_channel(s_dma_port_join, &dma_hw->ch[s_dma_byte_data].al3_read_addr_trig, &PIO_AUDIO->rxf[join_sm], pio_get_dreq(PIO_AUDIO, join_sm, false));
    dma_channel_start(s_dma_port_join);
    pio_sm_set_enabled(PIO_AUDIO, join_sm, true);

    // Сигнал о состоявшемся чтении - по завершению переноса у канала
    // адреса. Фильтр "только наши порты" даёт железо: склейщик отдаёт адрес,
    // только когда запись не нулевая. Канал выдачи байта для этого не
    // годится: с DivMMC он отвечает и на чтения памяти.
    //
    // Своя линия DMA_IRQ: линию звука занимает драйвер I2S
    // (irq_set_exclusive_handler, приоритет планировщика).
    dma_irqn_set_channel_enabled(DMA_IRQ_INDEX_PORT_RD, s_dma_port_join, true);
    irq_set_exclusive_handler(DMA_IRQ_NUM(DMA_IRQ_INDEX_PORT_RD), port_rd_isr);
    // Самый низкий из трёх: следующее чтение порта карты Z80 сделает не
    // раньше чем через несколько команд.
    irq_set_priority(DMA_IRQ_NUM(DMA_IRQ_INDEX_PORT_RD), IRQ_PRIO_PORT_RD);
    irq_set_enabled(DMA_IRQ_NUM(DMA_IRQ_INDEX_PORT_RD), true);

    pio_sm_set_enabled(PIO_WATCH, SM_PORT_DETECT, true);
}

} // namespace

namespace {

// Линия NMI - вход. Линия монтажная, её ведёт машина; постоянный выход
// работал бы навстречу, а выход в единицу при нажатой кнопке (она замыкает
// линию на землю) - это короткое замыкание. /NMI у Z80 срабатывает по
// фронту и защёлкивается в процессоре, держать линию вверху не нужно.
//
// Защёлка выхода в нуле: переключение направления в выход притягивает
// линию вниз, и это и есть запрос NMI. Так его подаёт клавиатура; кнопка
// замыкает ту же линию сама.
//
// Буфер ввода включён: Errata E9 не грозит, линия подтянута машиной.
void nmi_line_to_input() {
    gpio_put(PIN_NMI_N, 0);
    gpio_set_dir(PIN_NMI_N, GPIO_IN);
    gpio_disable_pulls(PIN_NMI_N);
    gpio_set_input_enabled(PIN_NMI_N, true);
}

// Окно включения TR-DOS: выборка команды по адресу 0x3Dxx. Значимый только
// старший байт, младший автомат не разбирает.
constexpr uint8_t kTrDosWindowHi = 0x3d;

// Триггер TR-DOS. Решает автомат в pio0, ведёт вывод автомат в pio2,
// решение переносит один канал DMA из очереди в очередь.
//
// Блоки разные: M1, RD и адрес видит только блок с окном
// выводов 0..31, а до DOS_N достаёт только блок с окном 16..47. Через SIO
// нельзя - он на шине IOPORT, у ядер, DMA туда не ходит.
//
// В этом режиме DivMMC выключен, и pio0 свободен целиком; в pio2 рядом со
// звуком и склейщиком портов место есть.
//
// Канал обычного приоритета: у цепочек шины есть срок до защёлкивания
// байта Z80, у этого - нет.
void setup_trdos() {
    const uint off_detect = pio_add_program(PIO_DETECT, &trdos_detect_program);
    pio_sm_claim(PIO_DETECT, SM_TRDOS_DETECT);
    // После сброса машины ПЗУ 128: флага ПЗУ 48 нет, порт открыт.
    pio_interrupt_clear(PIO_DETECT, IRQ_ROM48);
    pio_interrupt_clear(PIO_DETECT, IRQ_PAGING_LOCKED);
    trdos_detect_program_init(PIO_DETECT, SM_TRDOS_DETECT, off_detect, PIN_A0, PIN_M1_N, kTrDosWindowHi, IRQ_ROM48);
    s_trdos_detect_offset = static_cast<int>(off_detect);

    // Окно выводов блока - общее с звуком, и значение у них одно.
    pio_set_gpio_base(PIO_AUDIO, 16);
    const uint off_drive = pio_add_program(PIO_AUDIO, &trdos_drive_program);
    s_sm_trdos_drive     = static_cast<int>(pio_claim_unused_sm(PIO_AUDIO, true));
    const uint drive_sm  = static_cast<uint>(s_sm_trdos_drive);
    trdos_drive_program_init(PIO_AUDIO, drive_sm, off_drive, PIN_DOS_N);

    setup_fifo_channel(dma_claim_unused_channel(true), &PIO_AUDIO->txf[drive_sm], &PIO_DETECT->rxf[SM_TRDOS_DETECT],
                       pio_get_dreq(PIO_DETECT, SM_TRDOS_DETECT, false));

    pio_sm_set_enabled(PIO_AUDIO, drive_sm, true);
    pio_sm_set_enabled(PIO_DETECT, SM_TRDOS_DETECT, true);
}

// Слежение за сбросом машины в pio0: там видны M1 (окно выводов 0..31), а с
// выключенным DivMMC блок свободен, кроме триггера TR-DOS. Нет места -
// строка в журнал, слежения нет.
void setup_reset_watch() {
    if (!pio_can_add_program(PIO_DETECT, &reset_watch_program)) {
        debug_log("bus: no room in pio0 for the machine reset watch\n");
        return;
    }
    const uint off   = pio_add_program(PIO_DETECT, &reset_watch_program);
    s_sm_reset_watch = static_cast<int>(pio_claim_unused_sm(PIO_DETECT, true));
    const auto sm    = static_cast<uint>(s_sm_reset_watch);
    // 50 тактов Z80 на 3.5 МГц, проход - два такта автомата.
    constexpr uint64_t kZ80Hz     = 3500000u;
    constexpr uint64_t kZ80Cycles = 50u;
    const auto passes             = static_cast<uint32_t>(static_cast<uint64_t>(clock_get_hz(clk_sys)) * kZ80Cycles / kZ80Hz / 2u);
    reset_watch_program_init(PIO_DETECT, sm, off, PIN_M1_N, passes);
    pio_sm_set_enabled(PIO_DETECT, sm, true);
    debug_logf("bus: machine reset watch on, pio0 sm %u, %" PRIu32 " passes\n", sm, passes);
}

// Ответчик шины rom_serve с передачей OE_N: один на порты и память.
// Запускать последним, после всех, кто кладёт ему байты.
void setup_serve() {
    const uint off_serve = pio_add_program(PIO_SERVE, &rom_serve_program);
    pio_sm_claim(PIO_SERVE, SM_SERVE);
    rom_serve_program_init(PIO_SERVE, SM_SERVE, off_serve, PIN_BUFF_OE_N, PIN_RD_N, PIN_D0);

    // Покой - единица, шина к МК: ноль автомат выставляет сам на время
    // выдачи байта. Отданный нулём, буфер стоит открытым наружу до первой
    // выдачи, и без DivMMC ждать её не от кого - машина не может прочитать
    // своё ПЗУ и не доходит до наших портов.
    oe_handoff(PIO_SERVE, SM_SERVE, pio_encode_sideset_opt(1, 1));
}

// Чтение памяти: детектор и rom_join, четыре
// канала двух цепочек, запуск по порядку. База детектора - из
// s_page_variant: звать после divmmc_reset.
void setup_memory_read() {
    // Путь по цепочкам DMA.
    const uint off_detect = pio_add_program(PIO_DETECT, &rom_detect_program);
    const uint off_join   = pio_add_program(PIO_DETECT, &rom_join_program);

    pio_sm_claim(PIO_DETECT, SM_ROM_DETECT);
    pio_sm_claim(PIO_DETECT, SM_ROM_JOIN);

    // База - выбранного варианта: его выставил divmmc_reset.
    // ROM_BLK_N с этого места ведёт детектор: младший разряд базы варианта.
    rom_detect_program_init(PIO_DETECT, SM_ROM_DETECT, off_detect, PIN_RD_N, PIN_A8, PIN_ROM_BLK_N,
                            reinterpret_cast<uintptr_t>(&s_pagetabs[s_page_variant][0][0]));
    // ROM_BLK_N ведут оба: детектор по варианту, склейщик по странице (3Dxx).
    rom_join_program_init(PIO_DETECT, SM_ROM_JOIN, off_join, PIN_A0, PIN_ROM_BLK_N);

    dma_over_cores();

    s_dma_tab_addr  = dma_claim_unused_channel(true);
    s_dma_tab_data  = dma_claim_unused_channel(true);
    s_dma_byte_addr = dma_claim_unused_channel(true);
    s_dma_byte_data = dma_claim_unused_channel(true);

    // Уровень 1: детектор просит запись таблицы, ответ уходит rom_join.
    setup_chain(s_dma_tab_addr, s_dma_tab_data, &PIO_DETECT->rxf[SM_ROM_DETECT], &PIO_DETECT->txf[SM_ROM_JOIN], pio_get_dreq(PIO_DETECT, SM_ROM_DETECT, false),
                DMA_SIZE_32);
    // Уровень 2: rom_join просит байт, ответ уходит rom_serve. Байт, а не
    // слово: страницы сырые, готовые слова стоили бы 32 КБ за каждые 8.
    setup_chain(s_dma_byte_addr, s_dma_byte_data, &PIO_DETECT->rxf[SM_ROM_JOIN], &PIO_SERVE->txf[SM_SERVE], pio_get_dreq(PIO_DETECT, SM_ROM_JOIN, false),
                DMA_SIZE_8);

    // Порядок запуска, а не одновременность: автоматы в разных блоках, а
    // pio_enable_sm_mask_in_sync работает в одном блоке. Детектор с rom_join -
    // синхронно в pio0, rom_serve - после них: до этого места идёт передача
    // OE_N в PIO без глитча; запущенный раньше, он работает с ненастроенной
    // линией буфера и выводит мусор на шину. Хост в это время в сбросе.
    pio_enable_sm_mask_in_sync(PIO_DETECT, (1u << SM_ROM_DETECT) | (1u << SM_ROM_JOIN));

    pio_sm_set_enabled(PIO_SERVE, SM_SERVE, true);
}

} // namespace

// Выравнивания обязательны: склейщики собирают адрес склейкой "база или
// индекс со сдвигом на 2", а не сложением.
//
// Первый уровень - 1024: восемь бит индекса (A0..A7) плюс два бита
// масштаба. Подтаблицы - 128: пять бит (A8..A12) плюс два; они лежат в
// дырах таблицы страниц, выровненных на 512, по четыре строки на дыру.
alignas(kTrapLoEntries * sizeof(uint32_t)) uint32_t s_trap_lo_storage[kTrapLoEntries];

void rom_emu_init() {
    // Выравнивание карт держит alignas, но склейщики молча перепутали бы
    // адреса, сойди оно: проверяется здесь, а не на шине.
    if ((reinterpret_cast<uintptr_t>(s_trap_lo) & (kTrapLoEntries * sizeof(uint32_t) - 1u)) != 0u) {
        panic("rom_emu: trap level 1 at 0x%08x is not aligned to 1 KB", static_cast<unsigned>(reinterpret_cast<uintptr_t>(s_trap_lo)));
    }
    if ((reinterpret_cast<uintptr_t>(trap_sub(0)) & (kTrapSubEntries * sizeof(uint32_t) - 1u)) != 0u) {
        panic("rom_emu: trap sub-tables at 0x%08x are not aligned to 128 B", static_cast<unsigned>(reinterpret_cast<uintptr_t>(trap_sub(0))));
    }
    nmi_line_to_input();

    // Все записи - "не наша": и в вариантах, которые не заполняются. Дыры
    // не трогаются - в них карты трапов.
    for (uint32_t vi = 0; vi < PAGE_TAB_VARIANTS; ++vi) {
        if (pagetab_is_hole(vi)) continue;
        for (auto& half : s_pagetabs[vi]) {
            for (uint32_t& e : half) {
                e = kPageNotOurs;
            }
        }
    }
    // Страницы 0x0000-0x3FFF ставит divmmc вслед за портом 0xE3; память выше
    // 0x4000 - машины.
    divmmc_reset();

    setup_gpio(BringUp::RomEmulation);
    setup_serve();
    setup_memory_read(); // после divmmc_reset: база детектора - выбранный им вариант
    setup_bus_wr(BringUp::RomEmulation);
    setup_trap();
    setup_ports(BringUp::RomEmulation);

    // 0x0000-0x1FFF - ПЗУ DivMMC, 0x2000-0x3FFF - его страница ОЗУ; блокировкой
    // ПЗУ машины распоряжается divmmc вслед за подстановкой (начальное
    // состояние - выключено, его выставил divmmc_reset()).
    debug_log("rom_emu: DivMMC -- ROM at 0x0000-0x1FFF, RAM at 0x2000-0x3FFF,"
              " machine ROM blocked while mapped\n");
}

namespace {
// Без эмуляции ПЗУ шину поднимает только путь портов - тот же, что с ней:
// детектор, указатели с пустым триггером, общий ответчик, захват записи
// без ветки памяти. Ответчик включается последним.
void bring_up_ports_only() {
    setup_gpio(BringUp::PortsOnly);
    setup_serve();
    dma_over_cores();
    setup_bus_wr(BringUp::PortsOnly);
    setup_ports(BringUp::PortsOnly);
    pio_sm_set_enabled(PIO_SERVE, SM_SERVE, true);
    // После эмуляции ПЗУ: там pio0 занят детектором и сборкой адреса,
    // и триггеру в нём места нет. Вместе они и не нужны - дисковая
    // система одна.
    if (trdos_enabled()) setup_trdos();
    if (reset_watch_enabled()) setup_reset_watch();
}
} // namespace

void plugin_ports_init(player::protocol::HostProtocol& protocol) {
    s_protocol = &protocol;

    s_wr_table[PORT_DAT] = port_write_dat;
    // Порт команд хост и читает, и пишет (команды) - регистрируются оба
    // направления.
    s_wr_table[PORT_CMD] = port_write_cmd;
    // Ответы - в s_porttab; нулевая запись - порт не наш, склейщик её
    // отбрасывает.
    // Вооружение отдаётся протоколу: он решает, что выставить, платформа -
    // как (кодирует слова и публикует статус последним).
    protocol.set_arm(&arm_frame, nullptr, &hide_frame);
    port_set_read(PORT_CMD, player::protocol::HostProtocol::kStNone);

    // С эмуляцией ПЗУ путь портов поднял rom_emu_init.
    if (!rom_emu_enabled()) bring_up_ports_only();
}

} // namespace bus
