// Монтаж шины: раздача автоматов PIO, цепочки DMA, прерывания. Логика - в
// bus.cpp, развязка - bus_setup.h.
//
// Порядок разделов как в bus.cpp: плагинная шина, затем эмуляция ПЗУ и
// памяти.

#include "bus_setup.h"

#include <cstring>

#include "hardware/dma.h"
#include <iterator> // std::size

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico/platform.h"

#include "platform/log.h"
#include "divmmc.h"

// Порядок программ PIO во флеше - порядок этих двух строк.
#include "ports.pio.h"
#include "memory.pio.h"
#include "trap_join.pio.h"

namespace bus {

// Номера управляющих линий зашиты в wait gpio программ PIO: смена пина
// должна ломать сборку, а не шину.
static_assert(PIN_M1_N == Z80_M1_N_GPIO && PIN_WR_N == Z80_WR_N_GPIO && PIN_RD_N == Z80_RD_N_GPIO &&
                  PIN_IORQ_N == Z80_IORQ_N_GPIO && PIN_MREQ_N == Z80_MREQ_N_GPIO,
              "номера линий Z80 разошлись с wait gpio в ports.pio и memory.pio");

// --- Общее для обоих режимов: пины и каналы DMA ---
//
// Электрика шины одна на оба режима: плагинный и эмуляции ПЗУ отличаются
// только блоком PIO и автоматом, которому отдаются пины.

namespace {

// Управляющие сигналы (M1_N..MREQ_N) и адресная шина A0..A15 явно
// переводятся в SIO-входы без подтяжек до настройки PIO. Без этого пины
// остаются в произвольном состоянии после сброса, PIO ловит рваный цикл на
// старте и замолкает ("board not found"). PIO читает их через
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
    pio_sm_set_pindirs_with_mask(pio, sm, (1u << PIN_BUFF_OE_N), (1u << PIN_BUFF_OE_N) | (0xFFu << PIN_D0));
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
void setup_chain(uint ch_addr, uint ch_data, const volatile void* rxf, volatile void* txf, uint dreq,
                 dma_channel_transfer_size_t data_size) {
    dma_channel_config_t dc = dma_channel_get_default_config(ch_addr);
    channel_config_set_dreq(&dc, dreq);
    channel_config_set_high_priority(&dc, true);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    dma_channel_configure(ch_addr, &dc, &dma_hw->ch[ch_data].al3_read_addr_trig, rxf,
                          dma_encode_transfer_count_with_self_trigger(1), false);
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

// ports_only - без эмуляции памяти (перемычка снята): подтяжки линий цикла
// порта, как у прежних плагинных автоматов.
void setup_gpio(bool ports_only) {
    bus_inputs_to_sio();

    gpio_pull_up(PIN_RD_N);
    if (ports_only) {
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

// with_memory - ловить и записи в память ниже 0x4000 (эмуляция DivMMC).
// Без неё ветка памяти заменяется переходом к концу цикла: записи в ОЗУ
// машины прерывания не будят.
void setup_bus_wr(bool with_memory) {
    uint16_t code[sizeof(bus_wr_program_instructions) / sizeof(bus_wr_program_instructions[0])];
    for (uint32_t k = 0; k < std::size(code); ++k) code[k] = bus_wr_program_instructions[k];
    if (!with_memory) code[bus_wr_offset_mem] = static_cast<uint16_t>(pio_encode_jmp(bus_wr_offset_done));
    pio_program_t prog = bus_wr_program;
    prog.instructions = code;
    const uint off = pio_add_program(PIO_WATCH, &prog);
    pio_sm_claim(PIO_WATCH, SM_BUS_WRITE);
    bus_wr_program_init(PIO_WATCH, SM_BUS_WRITE, off);

    const uint irq = pio_get_irq_num(PIO_WATCH, 0);
    pio_set_irq0_source_enabled(PIO_WATCH, pis_interrupt0, true);
    irq_set_exclusive_handler(irq, bus_wr_isr);
    // 0x40: ниже трапов, выше чтения портов. Запись должна успеть до чтения того
    // же байта, около 1.7 мкс: ПЗУ DivMMC проверяет ОЗУ (inc (hl) / cp (hl) по
    // 0x3D00). Выше трапов она вытесняет трап, и вход по 0x0066 не срабатывает.
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

    // Склейщик в блоке звука: пины шины ему нужны только A8..A13 (база GPIO
    // 16 стоит с загрузки платы).
    const uint off_join = pio_add_program(PIO_AUDIO, &trap_join_program);
    s_sm_trap_join = pio_claim_unused_sm(PIO_AUDIO, true);
    const auto join_sm = static_cast<uint>(s_sm_trap_join);
    trap_join_program_init(PIO_AUDIO, join_sm, off_join, PIN_A8, PIN_A8 + 5u,
                           reinterpret_cast<uintptr_t>(s_trap_addr_map), reinterpret_cast<uintptr_t>(s_trap_page_map));

    s_dma_trap_in = dma_claim_unused_channel(true);
    s_dma_trap_addr = dma_claim_unused_channel(true);
    s_dma_trap_entry = dma_claim_unused_channel(true);
    s_dma_trap_word = dma_claim_unused_channel(true);
    s_dma_trap_note = dma_claim_unused_channel(true);
    const auto ch_in = static_cast<uint>(s_dma_trap_in);
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
    // Адрес записи карты -> запись -> триггер канала слова (ноль - стоп).
    setup_chain(static_cast<uint>(s_dma_trap_addr), static_cast<uint>(s_dma_trap_entry), &PIO_AUDIO->rxf[join_sm],
                &dma_hw->ch[ch_word].al3_read_addr_trig, pio_get_dreq(PIO_AUDIO, join_sm, false), DMA_SIZE_32);
    // Адрес выборки: rom_trap -> склейщик. Сам перевзводится, как адресный
    // канал цепочки.
    dma_channel_config_t dc = dma_channel_get_default_config(ch_in);
    channel_config_set_dreq(&dc, pio_get_dreq(PIO_DETECT, SM_ROM_TRAP, false));
    channel_config_set_high_priority(&dc, true);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    dma_channel_configure(ch_in, &dc, &PIO_AUDIO->txf[join_sm], &PIO_DETECT->rxf[SM_ROM_TRAP],
                          dma_encode_transfer_count_with_self_trigger(1), true);

    pio_sm_set_enabled(PIO_AUDIO, join_sm, true);
    pio_sm_set_enabled(PIO_DETECT, SM_ROM_TRAP, true);
}

void setup_ports() {
    const uint off_det = pio_add_program(PIO_WATCH, &port_detect_program);
    pio_sm_claim(PIO_WATCH, SM_PORT_DETECT);
    port_detect_program_init(PIO_WATCH, SM_PORT_DETECT, off_det, PIN_RD_N, PIN_A0,
                             reinterpret_cast<uintptr_t>(s_porttab));

    s_dma_port_addr = dma_claim_unused_channel(true);
    s_dma_port_ptr = dma_claim_unused_channel(true);
    s_dma_port_data = dma_claim_unused_channel(true);

    // Уровень 1: детектор дал &porttab[порт]; оттуда читается указатель и
    // кладётся в триггерный регистр канала выдачи ответа. Нулевой указатель
    // его не запускает (s_porttab).
    setup_chain(s_dma_port_addr, s_dma_port_ptr, &PIO_WATCH->rxf[SM_PORT_DETECT],
                &dma_hw->ch[s_dma_port_data].al3_read_addr_trig, pio_get_dreq(PIO_WATCH, SM_PORT_DETECT, false),
                DMA_SIZE_32);

    // Уровень 2 - канал выдачи ответа. Источник ему ставит и его же запускает запись в
    // al3_read_addr_trig, поэтому ни цепочки, ни своего DREQ нет: к моменту
    // записи данные готовы.
    setup_data_channel(s_dma_port_data, &PIO_SERVE->txf[SM_SERVE], DMA_SIZE_32);

    // Сигнал о состоявшемся чтении - по завершению переноса у канала выдачи
    // ответа. Фильтр "только наши порты" даёт железо: канал запускается,
    // только когда указатель не нулевой. Инструкций PIO не стоит.
    //
    // Своя линия DMA_IRQ: линию звука занимает драйвер I2S
    // (irq_set_exclusive_handler, приоритет планировщика).
    dma_irqn_set_channel_enabled(DMA_IRQ_INDEX_PORT_RD, s_dma_port_data, true);
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
// Защёлка выхода в нуле: переключение направления в выход притянуло бы
// линию вниз (запрос NMI). Сейчас NMI подаёт только кнопка.
//
// Буфер ввода включён: Errata E9 не грозит, линия подтянута машиной.
void nmi_line_to_input() {
    gpio_put(PIN_NMI_N, 0);
    gpio_set_dir(PIN_NMI_N, GPIO_IN);
    gpio_disable_pulls(PIN_NMI_N);
    gpio_set_input_enabled(PIN_NMI_N, true);
}

// Ответчик шины rom_serve с передачей OE_N: один на порты и память.
// Запускать последним, после всех, кто кладёт ему байты.
void setup_serve() {
    const uint off_serve = pio_add_program(PIO_SERVE, &rom_serve_program);
    pio_sm_claim(PIO_SERVE, SM_SERVE);
    rom_serve_program_init(PIO_SERVE, SM_SERVE, off_serve, PIN_BUFF_OE_N, PIN_RD_N, PIN_D0);

    // Сейчас side 0: OE_N в нуле, пока автомат не выставит своё; единица -
    // после замера уровня линии.
    oe_handoff(PIO_SERVE, SM_SERVE, pio_encode_sideset_opt(1, 0));
}

// Чтение памяти: детектор и rom_join, четыре
// канала двух цепочек, запуск по порядку. База детектора - из
// s_page_variant: звать после divmmc_reset.
void setup_memory_read() {
    // Путь по цепочкам DMA.
    const uint off_detect = pio_add_program(PIO_DETECT, &rom_detect_program);
    const uint off_join = pio_add_program(PIO_DETECT, &rom_join_program);

    pio_sm_claim(PIO_DETECT, SM_ROM_DETECT);
    pio_sm_claim(PIO_DETECT, SM_ROM_JOIN);

    // База - выбранного варианта: его выставил divmmc_reset.
    // ROM_BLK_N с этого места ведёт детектор: младший разряд базы варианта.
    rom_detect_program_init(PIO_DETECT, SM_ROM_DETECT, off_detect, PIN_RD_N, PIN_A8, PIN_ROM_BLK_N,
                            reinterpret_cast<uintptr_t>(&s_pagetabs[s_page_variant][0][0]));
    // ROM_BLK_N ведут оба: детектор по варианту, склейщик по странице (3Dxx).
    rom_join_program_init(PIO_DETECT, SM_ROM_JOIN, off_join, PIN_A0, PIN_ROM_BLK_N);

    dma_over_cores();

    s_dma_tab_addr = dma_claim_unused_channel(true);
    s_dma_tab_data = dma_claim_unused_channel(true);
    s_dma_byte_addr = dma_claim_unused_channel(true);
    s_dma_byte_data = dma_claim_unused_channel(true);

    // Уровень 1: детектор просит запись таблицы, ответ уходит rom_join.
    setup_chain(s_dma_tab_addr, s_dma_tab_data, &PIO_DETECT->rxf[SM_ROM_DETECT], &PIO_DETECT->txf[SM_ROM_JOIN],
                pio_get_dreq(PIO_DETECT, SM_ROM_DETECT, false), DMA_SIZE_32);
    // Уровень 2: rom_join просит байт, ответ уходит rom_serve. Байт, а не
    // слово: страницы сырые, готовые слова стоили бы 32 КБ за каждые 8.
    setup_chain(s_dma_byte_addr, s_dma_byte_data, &PIO_DETECT->rxf[SM_ROM_JOIN], &PIO_SERVE->txf[SM_SERVE],
                pio_get_dreq(PIO_DETECT, SM_ROM_JOIN, false), DMA_SIZE_8);

    // Порядок запуска, а не одновременность: автоматы в разных блоках, а
    // pio_enable_sm_mask_in_sync работает в одном блоке. Детектор с rom_join -
    // синхронно в pio0, rom_serve - после них: до этого места идёт передача
    // OE_N в PIO без глитча; запущенный раньше, он работает с ненастроенной
    // линией буфера и выводит мусор на шину. Хост в это время в сбросе.
    pio_enable_sm_mask_in_sync(PIO_DETECT, (1u << SM_ROM_DETECT) | (1u << SM_ROM_JOIN));

    pio_sm_set_enabled(PIO_SERVE, SM_SERVE, true);
}

} // namespace

extern "C" char __bss_end__;

void rom_emu_init() {
    // Карта трапов за .bss (kTrapAddrMapAt): .bss вырос до неё - стоп сразу,
    // а не порча памяти потом.
    if (reinterpret_cast<uintptr_t>(&__bss_end__) > kTrapAddrMapAt) {
        panic("rom_emu: .bss до 0x%08x заходит на карту трапов 0x%08x",
              static_cast<unsigned>(reinterpret_cast<uintptr_t>(&__bss_end__)), static_cast<unsigned>(kTrapAddrMapAt));
    }
    nmi_line_to_input();

    // Все записи - "не наша": и в вариантах, которые не заполняются.
    for (auto& v : s_pagetabs) {
        for (auto& half : v) {
            for (uint32_t& e : half) e = kPageNotOurs;
        }
    }
    // Страницы 0x0000-0x3FFF ставит divmmc вслед за портом 0xE3; память выше
    // 0x4000 - машины.
    divmmc_reset();

    setup_gpio(false);
    setup_serve();
    setup_memory_read(); // после divmmc_reset: база детектора - выбранный им вариант
    setup_bus_wr(true);
    // Трапы: подстановку меняет ядро по прерыванию.
    setup_trap();
    setup_ports();

    // 0x0000-0x1FFF - ПЗУ DivMMC, 0x2000-0x3FFF - его страница ОЗУ; блокировкой
    // ПЗУ машины распоряжается divmmc вслед за подстановкой (начальное
    // состояние - выключено, его выставил divmmc_reset()).
    debug_log("rom_emu: DivMMC -- ROM at 0x0000-0x1FFF, RAM at 0x2000-0x3FFF,"
              " machine ROM blocked while mapped\n");
}

void plugin_ports_init(player::protocol::HostProtocol& protocol) {
    s_protocol = &protocol;

    s_wr_table[PORT_DAT] = port_write_dat;
    // Порт команд хост и читает, и пишет (команды) - регистрируются оба
    // направления.
    s_wr_table[PORT_CMD] = port_write_cmd;
    // Слова ответа для наших портов; остальные записи нулевые - PIO
    // трактует ноль как "порт не наш" (jmp !y) и на шину не выходит.
    // Вооружение отдаётся протоколу: он решает, что выставить, платформа -
    // как (кодирует слова и публикует статус последним).
    protocol.set_arm(&arm_frame, nullptr, &hide_frame);
    z80_bus_pio_set_rd(PORT_CMD, player::protocol::HostProtocol::kStNone);

    // Без перемычки шину поднимает только путь портов - тот же, что в режиме
    // DivMMC: детектор, указатели с пустым триггером, общий ответчик, захват
    // записи без ветки памяти. С перемычкой его поднял rom_emu_init.
    if (!divmmc_selected()) {
        setup_gpio(true);
        setup_serve();
        dma_over_cores();
        setup_bus_wr(false);
        setup_ports();
        pio_sm_set_enabled(PIO_SERVE, SM_SERVE, true);
    }
}

} // namespace bus
