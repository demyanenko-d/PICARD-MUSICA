// SPDX-License-Identifier: MIT
// Логика шины Z80.
//
// Автоматы PIO разделены по сигналу: циклы IORQ отдельно от циклов MREQ и
// трапов.

#pragma once

#include <cstdint>

#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "pico/time.h"

#include "hw_config.h"

namespace player::protocol {
class HostProtocol;
} // namespace player::protocol

namespace bus {

// --- Пины и режим ---

constexpr uint PIN_BUFF_OE_N = 2; // 0 - данные МК на шину, 1 - с шины на МК
constexpr uint PIN_M1_N      = 3; // используется эмуляцией ПЗУ
constexpr uint PIN_WR_N      = 4;
constexpr uint PIN_RD_N      = 5;
constexpr uint PIN_IORQ_N    = 6;
constexpr uint PIN_MREQ_N    = 7;  // используется эмуляцией ПЗУ
constexpr uint PIN_A0        = 8;  // A0..A7 = GPIO 8..15
constexpr uint PIN_D0        = 24; // D0..D7 = GPIO 24..31
constexpr uint ADDR_LINES    = 16; // A0..A15
constexpr uint DATA_LINES    = 8;  // D0..D7

// Резерв: разведены, ни к чему не привязаны, с загрузки - входы. Не входят
// GPIO0 (PSRAM CS, XIP_CS1: gpio_init отнимет его у контроллера памяти,
// HardFault), GPIO35 (NMI) и GPIO40-43 (карта: MISO читается, входной
// буфер нужен).
constexpr uint RESERVED_PINS[] = {32, 33};
// Блокировка ПЗУ машины: низкий уровень гасит её ПЗУ и отдаёт окно нам.
//
// Проверено на железе: при нуле машина не грузится и процессор
// читает с шины 0xFF (ПЗУ погашено), при единице грузится как обычно.
//
// Сигнал доходит до платы через перемычку, поэтому пин нельзя держать
// плавающим входом: линия может защёлкнуться на произвольном уровне и
// глушить ПЗУ машины. С загрузки - выход в единице (блокировки нет).
//
// В режиме DivMMC пином владеет детектор чтения памяти (rom_detect): уровень
// - по чётности варианта таблицы, и склейщик rom_join - по странице (окно
// 3Dxx без подстановки).
constexpr uint PIN_ROM_BLK_N = 1;

// До передачи пина автомату: уровень через SIO.
inline void release_host_rom() {
    gpio_put(PIN_ROM_BLK_N, 1);
}

constexpr uint PIN_NMI_N   = 35;
constexpr uint PIN_RESET_N = 47;

// Признак ПЗУ TR-DOS для машины с интерфейсом Beta: ноль - страничено ПЗУ
// TR-DOS. Плата ведёт его всегда, даже когда триггер выключен: там за ним
// вход, а плавающий вход это неопределённость.
//
// При TR-DOS и своём ПЗУ с DOS_N вывод ведёт dos_drive в pio2 (окно
// 16..47), иначе SIO.
constexpr uint PIN_DOS_N = 36;

// Перемычка режима загрузки: замкнута на землю - плата поднимает
// конфигуратор. Вывод уже несёт перемычку на плате и прошивкой больше
// нигде не читается; подтяжка вверх своя, внешней на нём нет.
constexpr uint PIN_BOOT_JUMPER = 37;

// /WAIT шины Z80 - открытый сток: в выходном регистре всегда ноль, линию
// тянет вниз включение выхода, отпускает выключение. Единицу плата не
// выдаёт никогда: к ней линию тянет машина. Вывод берут Wi-Fi (через SIO;
// CTS модуля на нём не используется) и TR-DOS (автомат port_join_hold).
constexpr uint PIN_WAIT_N = 34;

inline void wait_line_init() {
    gpio_init(PIN_WAIT_N);
    gpio_disable_pulls(PIN_WAIT_N);
    gpio_put(PIN_WAIT_N, 0);
    gpio_set_dir(PIN_WAIT_N, GPIO_IN);
}

inline void assert_wait() {
    sio_hw->gpio_hi_oe_set = 1u << (PIN_WAIT_N - 32u);
}

inline void release_wait() {
    sio_hw->gpio_hi_oe_clr = 1u << (PIN_WAIT_N - 32u);
}

// Снять признак ПЗУ TR-DOS. Зовётся при сбросе машины: после сброса Beta
// стоит в исходном, и признак обязан совпасть.
inline void release_host_trdos() {
    gpio_put(PIN_DOS_N, 1);
}

// Включить или выключить эмуляцию ПЗУ: DivMMC или конфигуратор настроек.
// Ставится один раз при загрузке, из настроек, до всех, кто спрашивает
// режим: менять раскладку PIO и таблицу страниц у работающей машины
// незачем и опасно.
void rom_emu_set_enabled(bool on);
// true - эмуляция ПЗУ поднята: подставляет память и держит ПЗУ машины.
bool rom_emu_enabled();

// Включить или выключить триггер TR-DOS. Ставится там же и тогда же, что
// и DivMMC: дисковая система одна, и автоматы у них общие.
void trdos_set_enabled(bool on);
bool trdos_enabled();

// Навигатор образов по NMI в режиме TR-DOS: включает загрузка, если поднят
// Beta Disk платы. Ставится там же и тогда же, что режим шины.
void nav_set_enabled(bool on);
// Страница обмена навигатора - начало банка N0, у Z80 0x2000; nullptr -
// навигатора на шине нет.
uint8_t* nav_page();
// Меню навигатора открыто: флаг NMI до второго байта его RETN.
bool nav_open();
// Последняя запись машины в 7FFD целиком, после сброса ноль.
uint8_t port_7ffd();

// Слежение за сбросом машины: нет выборки M1 - машина в сбросе. Без
// дисковой системы и своего ПЗУ - автомат в pio0 (50 тактов Z80 на 3.5 МГц),
// с ними - счётчик M1 на PWM и опрос с Core0 (дольше 50 мкс). Ставится там
// же и тогда же, что режим шины.
void reset_watch_set_enabled(bool on);
bool reset_watch_enabled();
// Сколько сбросов машины замечено с загрузки.
uint32_t machine_resets_seen();

// Снять ПЗУ TR-DOS по сбросу машины: флаги DOS, ПЗУ 48, закрытия 7FFD и
// навигатора. Без триггера - уровень вывода через SIO.
void trdos_release();

// Режим конфигуратора настроек. Ставится до rom_emu_set_enabled и вместе с
// ним: механизм подстановки тот же, меняются образ ПЗУ и начальное
// состояние - CONMEM держится с первой выборки, чтобы машина стартовала
// прямо в наше ПЗУ.
void config_rom_set_enabled(bool on);
bool config_rom_enabled();

// Страница настроек глазами машины: 0x2000-0x3FFF, банк 0 ОЗУ DivMMC.
// Плата пишет её прямо - это её массив в SRAM.
uint8_t* config_rom_page();
uint32_t config_rom_page_bytes();

// Своё ПЗУ Пентагона: 64 КБ, страницы 0 - сервис, 1 - TR-DOS, 2 - 128,
// 3 - 48; страница = (DOS ? 0 : 2) + бит 4 порта 7FFD. Отдаётся из ОЗУ
// DivMMC, поэтому только без DivMMC. Ставится до подъёма шины, как режим
// эмуляции ПЗУ. drive_dos - вести DOS_N на машину (DISKSYS=TRDOS: порты
// дисковода машины отвечают только при нём).
inline constexpr uint32_t kPentagonRomBytes = 65536;
// Разложить образ в памяти отдачи. false - образ стёрт (одни 0xFF).
bool pentagon_rom_load(const uint8_t* image);
void pentagon_rom_set_enabled(bool on, bool drive_dos);
bool pentagon_rom_enabled();
// Блок 64 КБ на границе 64 КБ внутри ОЗУ DivMMC.
uint8_t* pentagon_rom_block();

// Единица на выводе - хост в сбросе: на плате линию инвертирует 74HCT04,
// за ним диод (сброс на шине двунаправленный).
inline void assert_host_reset() {
    gpio_put(PIN_RESET_N, 1);
}
inline void release_host_reset() {
    gpio_put(PIN_RESET_N, 0);
}

// --- Плагинная шина ---
// Порты протокола и свои порты. Путь один на оба режима: детектор порта,
// склейщик и общий ответчик rom_serve. Без DivMMC этот путь и поднимает
// шину, с ним - его поднимает эмуляция ПЗУ.

void plugin_ports_init(player::protocol::HostProtocol& protocol);

// --- Свои порты поверх шины ---
//
// Диспетчеризация по таблицам на 256 входов: добавить порт - заполнить
// ячейку, PIO, DMA и обе ISR не меняются. Так подключён эмулятор
// Z-Controller.
//
// Запись и чтение устроены по-разному. На запись обработчик зовётся
// постфактум - обычная функция. Ответ на чтение выдаёт PIO из таблицы
// готовых значений без процессора: к моменту ISR байт уже на шине.
// Значение кладётся в таблицу заранее, хук после чтения готовит
// следующее.
//
// Регистрации и ответы можно звать и до plugin_ports_init(): таблицы
// статические, init их не стирает.

using PortWriteFn = void (*)(uint8_t port, uint8_t data);

// Обработчик записи в порт. Вызывается из ISR: короткий и в SRAM
// (__not_in_flash_func).
void port_on_write(uint8_t port, PortWriteFn fn);

// Что ответить на следующее чтение порта. Значение ложится в ячейку ответа
// порта.
void port_set_read(uint8_t port, uint8_t value);

// Хук после состоявшегося чтения: здесь готовят ответ на следующее.
// Вызывается из ISR чтения - самого горячего места шины: только то, что
// укладывается в единицы микросекунд.
using PortReadFn = void (*)(uint8_t port);
void port_on_read_done(uint8_t port, PortReadFn fn);

// Счётчики с загрузки. Команды и данные считает dispatch_port_write, очередь
// записи - bus_wr_isr (оба режима). В ISR чтения счётчиков
// нет, чтобы не задерживать вход в следующее прерывание.
struct PioStats {
    uint32_t cmd_bytes = 0; // байтов в порт команд
    uint32_t dat_bytes = 0; // байтов в порт данных

    // Потерянные слова на записи: push noblock в ports.pio молча
    // выбрасывает слово при полной RX FIFO. Должен быть 0. Ненулевое - разбор
    // записи не успевает за процессором.
    uint32_t wr_fifo_overruns = 0;
    // Наибольшее число слов, разобранных bus_wr_isr за вызов: цикл дочитывает
    // пришедшее по ходу, поэтому бывает больше глубины FIFO (8).
    uint8_t wr_fifo_max_level = 0;
};

// Копия счётчиков, накопительных с загрузки.
PioStats bus_stats();

// --- Эмуляция ПЗУ и памяти ---
// Путь, которым прошивка подставляет машине байты из своих страниц:
// чтение по цепочкам DMA, запись через PIO, трапы, порты DivMMC.
// Работает, когда настройками выбран DivMMC.

// Поднимает эмуляцию: таблица страниц, автоматы PIO, цепочки DMA,
// блокировка ПЗУ машины. Звать до выхода машины из сброса, если сброс
// разрешён.
void rom_emu_init();

// Время последнего чтения порта состояния (time_us_32): клиент хоста читает
// его в своём цикле без перерыва, пока он в плеере.
uint32_t host_status_read_us();
// Опрос очередей шины с Core0: отметки опоздания rom_serve (сколько байтов
// он не выставил на шину, потому что цикл чтения уже кончился - опоздание
// по цепочке DMA или лишний байт) и потери в очередях трапов (RXSTALL).
// Звать часто - в FIFO отметок помещается четыре, лишние теряются. Зовёт
// log_task (Core0), счёт печатает print_bus_health.
void bus_queues_poll();

// Порты протокола: что сейчас отвечается по портам команд и данных и
// сколько байтов rom_serve не выставил.
struct GenericPortsState {
    uint32_t tab_cmd    = 0; // запись s_porttab: адрес ячейки ответа, 0 - порт не наш
    uint32_t tab_dat    = 0;
    uint8_t val_cmd     = 0;
    uint8_t val_dat     = 0;
    uint32_t serve_late = 0;
};
GenericPortsState generic_ports_state();

// Замеры шины, которые снимает Core0 (bus_queues_poll).
//   serve_late_full - опрос застал очередь отметок опоздания полной: число
//                     опозданий выдачи байта занижено.
//   *_lost          - потерянные слова в очередях трапов с push noblock:
//                     rom_trap (pio0), первый уровень и путь страницы
//                     (склейщик, pio2), маршрут и второй уровень (pio2).
//                     Потеря здесь - несработавший вход или выход DivMMC,
//                     то есть машина исполняет чужую память; по остальным
//                     счётчикам это неотличимо от нормы.
// Очереди трапов: числа - опросов, заставших флаг потери (флаги липкие,
// опрос раз в 1-10 мс), а не слов.
struct BusMeasurements {
    uint32_t serve_late_full;
    uint32_t trap_lost;   // rom_trap: push noblock в полную RX
    uint32_t join_lost;   // TXOVER склейщика: DMA писал в полную TX
    uint32_t route_lost;  // TXOVER trap_route
    uint32_t detect_lost; // TXOVER детектора: слово варианта от трапа
    uint32_t join_stall;  // RXSTALL склейщика: стоял, слово ушло позже
    uint32_t route_stall;
    bool trdos;          // триггер TR-DOS заведён
    uint8_t trdos_level; // вывод DOS_N
    bool trdos_asserted; // флаг DOS
    // Фронты линий хоста (опросов с защёлкнутым фронтом): пары спад/подъём
    // для DOS_N, NMI_N, ROM_BLK_N, RESET_N. В простое приращения нулевые;
    // ненулевое без видимой причины - ложный переход или помеха на линии.
    uint32_t sig_edges[8];
    // Очередь вариантов детектора: ожидания места ядром и чистки по сроку
    // (машина не читала память дольше kVariantWaitUs).
    uint32_t variant_waits;
    uint32_t variant_clears;
    // Худший проход bus_wr_isr: такты ядра (DWT) и слова за тот проход.
    uint32_t wr_isr_cyc_max;
    uint32_t wr_isr_cyc_drained;
};
BusMeasurements bus_measurements();

// Ответ на чтение порта, путь один на оба режима. Значение заранее лежит в
// таблице, процессор в цикле шины не участвует. Порт, которому ответ не
// ставили (или сняли port_release), не обслуживается: отвечает машина.
void port_answer(uint8_t port, uint8_t value);

// Страница ответов на 256 байт: байт выбирает старшая половина адреса.
// Выравнивание страницы - 256 байт, иначе отказ при установке. Страницу
// ведёт устройство; здесь запоминается только её адрес, поэтому
// перезаписывать её можно в любой момент - следующее чтение возьмёт новое.
void port_answer_page(uint8_t port, const uint8_t* page);

void port_release(uint8_t port);

// Порты Beta Disk: только при DOS (режимы без DivMMC). Порт отвечает байтом
// ячейки устройства, вне DOS - прежним ответом; nullptr снимает. Чтение
// такого порта при DOS зовёт один хук на все порты, мимо port_on_read_done.
bool port_dos_cell(uint8_t port, const uint8_t* cell);
void port_dos_on_read_done(PortReadFn fn);
// Чтение порта при DOS держит /WAIT, где склейщик это умеет: fn(порт) - из
// обработчика шины, пока Z80 стоит; ответ - port_hold_answer, из
// обработчика или позже. Хук один на все порты, ставит его последний вызов.
bool port_dos_hold(uint8_t port, PortReadFn fn);
// То же для порта обычной таблицы (при DOS - и для его копии); хук тоже
// один на все такие порты. Снимает удержание port_answer.
bool port_hold(uint8_t port, PortReadFn fn);
void port_hold_answer(uint8_t v);
// /WAIT через SIO, где склейщик держать не умеет (всё, кроме TR-DOS): Z80
// встаёт на следующем цикле шины. ready - вывод у SIO (WIFI=1) и
// склейщика с удержанием нет.
bool wait_late_ready();
void wait_late(bool on);
bool port_hold_active();
// Что порт отвечает сейчас, для журнала: байт ячейки 0..255, или снят
// (kPortReleased - отвечает машина), или страница (kPortPaged).
inline constexpr int32_t kPortReleased = -1;
inline constexpr int32_t kPortPaged    = -2;
int32_t port_answer_state(uint8_t port);

} // namespace bus
