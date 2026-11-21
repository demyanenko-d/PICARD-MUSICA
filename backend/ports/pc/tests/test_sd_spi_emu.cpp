// SPDX-License-Identifier: MIT
// Эмулятор SD-карты платы (DivMMC и Z-Controller) на ПК: настоящий
// sd_spi_emu.cpp, носитель - массив в памяти, хост - ведущий SPI байт за
// байтом, как DivMMC и Z-Controller. Цикл Core1 (sd_spi_task) зовётся после
// каждого байта хоста, если сценарий не держит его занятым.
//
// Каждая ветка автомата уже была отказом на плате; запись с хоста на плате
// не исполнялась ни разу - её держит только этот тест.

#include "testing.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "devices/hal/z80_ports.h"
#include "devices/hal/spi.h"
#include "devices/sd/sd_protocol.h"
#include "devices/sd/card_protocol.h"
#include "devices/sd/spi_emu.h"
#include "devices/hal/media.h"
#include "devices/storage/storage.h"
#include "devices/storage/storage_host.h"
#include "devices/storage/volumes.h"
#include "pc/fatfs_disk.h"
#include "ff.h"
#include "devices/zcontroller/zcontroller.h"
#include "devices/divmmc_card/divmmc_card.h"
#include "core/util/fnv1a.h"
#include "core/memory/track_memory.h"
#include "player/config/config_page.h"
#include "player/config/config_service.h"
#include "player/config/config_tools.h"
#include "player/config/tools_service.h"

uint32_t g_test_time_us = 0;
char g_test_log[8192];
// Крюк в часах (rp2350_stub/platform_impl.cpp): им тест вклинивается
// внутрь такта эмулятора.
extern void (*g_mono_hook)();
uint32_t g_test_spi_hz = 1; // делитель частоты линии, до первой установки - не ноль

namespace {
uint8_t g_port_rd[256];
// Записей в таблицу ответов: по ним видно, что признак носителя публикуется
// только при изменении.
uint32_t g_port_rd_writes[256];
devices::hal::PortWriteFn g_port_wr[256];
devices::hal::PortReadDoneFn g_port_rd_done[256];
} // namespace

// Запись в порт Z80 из другого файла тестов: хуки ставит то устройство,
// которое их регистрирует, а таблица хуков живёт здесь.
void test_z80_port_write(uint8_t port, uint8_t value);
void test_z80_port_write(uint8_t port, uint8_t value) {
    if (g_port_wr[port] != nullptr) g_port_wr[port](port, value);
}
bool test_z80_port_has_writer(uint8_t port);
bool test_z80_port_has_writer(uint8_t port) {
    return g_port_wr[port] != nullptr;
}
// Что плата отдаёт на чтение этого порта. 0xFF - не отвечает никто.
uint8_t test_z80_port_read_value(uint8_t port);
uint8_t test_z80_port_read_value(uint8_t port) {
    return g_port_rd[port];
}
// Чтение порта машиной: байт из таблицы ответов, затем обработчик
// "чтение состоялось", как на шине.
uint8_t test_z80_port_read(uint8_t port);
uint8_t test_z80_port_read(uint8_t port) {
    const uint8_t v = g_port_rd[port];
    if (g_port_rd_done[port] != nullptr) g_port_rd_done[port](port);
    return v;
}
// Сколько раз устройство ставило ответ на чтение порта.
uint32_t test_z80_port_set_count(uint8_t port);
uint32_t test_z80_port_set_count(uint8_t port) {
    return g_port_rd_writes[port];
}

// Порты Z80 теста (devices/hal/z80_ports.h): таблица ответов и обработчики
// в обычных массивах, тест дёргает их сам.
// Вытеснение между обменом и записью ячейки: крюк зовётся до записи,
// одноразовый, только для своего порта.
void (*g_port_set_hook)()  = nullptr;
uint8_t g_port_set_hook_on = 0;

void devices::hal::z80_port_set_read(uint8_t port, uint8_t value) {
    if (g_port_set_hook != nullptr && port == g_port_set_hook_on) {
        void (*hook)()  = g_port_set_hook;
        g_port_set_hook = nullptr;
        hook();
    }
    g_port_rd[port] = value;
    ++g_port_rd_writes[port];
}
// Страницы ответов: сколько раз ставили и снимали и что поставили в
// последний раз. Нужно тесту клавиатуры (test_hid.cpp): правило "порт 0xFE
// занимается только под нажатой клавишей" проверяется именно по числу
// постановок, а не по содержимому страницы.
uint32_t g_port_page_sets[256];
uint32_t g_port_clears[256];
const uint8_t* g_port_last_page = nullptr;

void devices::hal::z80_port_set_read_page(uint8_t port, const uint8_t* page) {
    ++g_port_page_sets[port];
    g_port_last_page = page;
}
void devices::hal::z80_port_clear_read(uint8_t port) {
    g_port_rd[port] = 0xFF;
    ++g_port_clears[port];
}
void devices::hal::z80_port_on_write(uint8_t port, PortWriteFn fn) {
    g_port_wr[port] = fn;
}
void devices::hal::z80_port_on_read_done(uint8_t port, PortReadDoneFn fn) {
    g_port_rd_done[port] = fn;
}
// Удержания /WAIT у теста нет: архив отдаёт заглушку каталога.
bool devices::hal::z80_port_hold(uint8_t, PortReadDoneFn) {
    return false;
}
void devices::hal::z80_port_hold_answer(uint8_t) {}
bool devices::hal::z80_wait_late_ready() {
    return false;
}
void devices::hal::z80_wait_late(bool) {}

namespace {
constexpr uint32_t kDiskSectors = 4096;
uint8_t g_disk[kDiskSectors][512];
uint8_t g_usb_disk[kDiskSectors][512];
uint32_t g_card_sectors   = kDiskSectors; // ёмкость пустышки: тест меняет её на ходу
uint32_t g_usb_sectors    = kDiskSectors; // своя у флешки, как у отдельного носителя
bool g_usb_present        = false;
uint32_t g_card_serial    = 0; // номер карты: другая карта той же ёмкости
uint32_t g_writes         = 0;
uint32_t g_reads          = 0;           // обращений к носителю: ими видно, заказан ли сектор вовсе
uint32_t g_fail_read_lba  = 0xFFFFFFFFu; // носитель не отдаёт этот сектор
uint32_t g_fail_write_lba = 0xFFFFFFFFu; // носитель не принимает этот сектор
bool g_fail_write_partial = false;       // и при этом успевает записать половину
bool g_task_enabled       = true;
// Обмены хоста посреди чтения сектора g_hook_lba: обработчик прерывания
// вклинивается в цикл Core1, пока тот читает носитель.
void (*g_read_hook)() = nullptr;
uint32_t g_hook_lba   = 0xFFFFFFFFu;
} // namespace

// Носители под арбитром: карта и флешка, обе пустышки. Сам арбитр
// настоящий: так проверяется и его выбор носителя по хозяину.
namespace devices::hal {
bool media_init() {
    return true;
}
bool driver_present(Medium m) {
    return m == Medium::Card || g_usb_present;
}
uint32_t driver_sector_count(Medium m) {
    return m == Medium::Usb ? g_usb_sectors : g_card_sectors;
}

// Пачкой - циклом: у пустышек выгоды нет, а путь блочного упреждения
// проверить надо. Ширина пачки у карты - как на плате (16), у флешки - 8:
// так исполняются обе ширины блока.
bool media_read_run(Medium m, uint32_t lba, uint32_t count, uint8_t* dst);

uint32_t media_run_sectors(Medium m) {
    return m == Medium::Usb ? 8u : 16u;
}

// Снимок носителей: в тесте он не кэш, а зеркало пустышек выше -
// публикуется по тем же событиям, что на плате. Тест двигает g_usb_present
// и g_card_sectors напрямую, поэтому снимок берётся заново при каждом
// обращении, а поколение растёт на любое изменение.
MediaState& media_state_slot(Medium m) {
    static MediaState s[2];
    return s[m == Medium::Usb ? 1u : 0u];
}
uint32_t g_media_generation = 0;

void media_state_publish(Medium m) {
    MediaState& st         = media_state_slot(m);
    const bool present     = driver_present(m);
    const uint32_t sectors = driver_sector_count(m);
    const uint32_t serial  = m == Medium::Card ? g_card_serial : 0u;
    if (st.present == present && st.sectors == sectors && st.serial == serial) return;
    st.present = present;
    st.sectors = sectors;
    st.serial  = serial;
    ++g_media_generation;
}

uint32_t g_media_state_calls = 0; // обращений к снимку: сверка смены носителя и чтения
const MediaState& media_state(Medium m) {
    ++g_media_state_calls;
    media_state_publish(m); // пустышки меняет сам тест, событий у него нет
    return media_state_slot(m);
}

uint32_t media_state_generation() {
    media_state_publish(Medium::Card);
    media_state_publish(Medium::Usb);
    return g_media_generation;
}

bool g_usb_busy       = false; // обмен с флешкой идёт или брошенный держит буфер
uint32_t g_usb_nested = 0;     // вложенных обращений к занятой флешке
bool media_busy(Medium m) {
    return m == Medium::Usb && g_usb_busy;
}

uint32_t g_reads_past_end = 0; // обращений за концом носителя: арбитр их делать не должен

bool media_read(Medium m, uint32_t lba, uint8_t* dst) {
    // Обмен с флешкой уже идёт: вложенный вызов получает отказ, как usb_msc.
    if (m == Medium::Usb && g_usb_busy) {
        ++g_usb_nested;
        return false;
    }
    ++g_reads;
    if (lba >= (m == Medium::Usb ? g_usb_sectors : g_card_sectors)) ++g_reads_past_end;
    if (lba >= kDiskSectors || lba == g_fail_read_lba) return false;
    auto* disk = (m == Medium::Usb) ? g_usb_disk : g_disk;
    if (m == Medium::Card && lba == g_hook_lba && g_read_hook) {
        void (*hook)() = g_read_hook;
        g_read_hook    = nullptr;
        std::memcpy(dst, disk[lba], 256);
        hook();
        std::memcpy(dst + 256, disk[lba] + 256, 256);
        return true;
    }
    std::memcpy(dst, disk[lba], 512);
    return true;
}

bool media_read_run(Medium m, uint32_t lba, uint32_t count, uint8_t* dst) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!media_read(m, lba + i, dst + i * 512)) return false;
    }
    return true;
}

bool media_write(Medium m, uint32_t lba, const uint8_t* src) {
    if (lba >= kDiskSectors) return false;
    if (lba == g_fail_write_lba) {
        // Оборванная запись: половина сектора легла, носитель ответил отказом.
        if (g_fail_write_partial) std::memcpy((m == Medium::Usb ? g_usb_disk : g_disk)[lba], src, 256);
        return false;
    }
    std::memcpy((m == Medium::Usb ? g_usb_disk : g_disk)[lba], src, 512);
    ++g_writes;
    return true;
}

void media_log_health() {}
} // namespace devices::hal

namespace {

using devices::sd::SdOwner;
SdOwner g_who = SdOwner::ZController;

// Свёртка обмена для побайтной сверки: ответы эмулятора хосту (x) и байты
// драйвера на линии в обе стороны. Включает её только сценарий сверки.
bool g_wire_on  = false;
uint32_t g_wire = 0;
void wire(uint8_t b) {
    if (g_wire_on) g_wire = soundsinth::util::fnv1a(g_wire, &b, 1);
}

// Обмен байтом: около 15 мкс на опрос у Z80 3.5 МГц.
uint8_t x(uint8_t b) {
    g_test_time_us  += 15;
    const uint8_t r  = devices::sd::sd_spi_byte(g_who, b);
    if (g_task_enabled) devices::sd::sd_spi_task();
    wire(r);
    return r;
}

uint8_t cmd(uint8_t c, uint32_t arg) {
    x(0xFF);
    uint8_t f[6] = {static_cast<uint8_t>(0x40 | c), static_cast<uint8_t>(arg >> 24), static_cast<uint8_t>(arg >> 16),
                    static_cast<uint8_t>(arg >> 8), static_cast<uint8_t>(arg),       0};
    f[5]         = devices::sd::crc7(f, 5);
    for (int i = 0; i < 6; ++i)
        x(f[i]);
    for (int i = 0; i < 10; ++i) {
        const uint8_t r = x(0xFF);
        if ((r & 0x80) == 0) return r;
    }
    return 0xFF;
}

void sel(bool on) {
    devices::sd::sd_spi_select(g_who, on);
}

void fill_disk() {
    for (uint32_t s = 0; s < kDiskSectors; ++s) {
        for (uint32_t i = 0; i < 512; ++i)
            g_disk[s][i] = static_cast<uint8_t>(s * 7u + i);
    }
    g_writes             = 0;
    g_fail_read_lba      = 0xFFFFFFFFu;
    g_fail_write_lba     = 0xFFFFFFFFu;
    g_fail_write_partial = false;
    g_task_enabled       = true;
    g_read_hook          = nullptr;
    g_hook_lba           = 0xFFFFFFFFu;
    g_who                = SdOwner::ZController;
    for (uint32_t s2 = 0; s2 < kDiskSectors; ++s2)
        std::memset(g_usb_disk[s2], 0, 512);
    g_usb_present  = false;
    g_card_sectors = kDiskSectors;
    for (uint8_t o = 0; o < devices::storage::kEmulatorCount; ++o) {
        devices::storage::storage_set_emulator_medium(static_cast<SdOwner>(o), devices::hal::Medium::Card);
    }
    // Буфер блочного кэша: на плате он в PSRAM, здесь - статический. Без
    // него блочного пути нет вовсе, и проверки шли бы мимо него.
    static uint8_t s_block_cache[devices::storage::kBlockCacheBytes];
    devices::storage::storage_attach_block_cache(s_block_cache, sizeof(s_block_cache), nullptr);
    // Сбрасывает кэш секторов: содержимое пустышек только что поменялось.
    devices::storage::storage_init();
}

bool init_card() {
    sel(true);
    const uint8_t r0 = cmd(0, 0);
    uint8_t r        = 0xFF;
    for (int i = 0; i < 4 && r != 0; ++i) {
        cmd(55, 0);
        r = cmd(41, 0x40000000u);
    }
    sel(false);
    return r0 == 1 && r == 0;
}

void fresh_card() {
    fill_disk();
    devices::sd::sd_spi_emu_init();
    CHECK(init_card());
}

struct ReadRes {
    bool ok        = false;
    uint32_t polls = 0;
    bool crc_ok    = false;
    bool data_ok   = false;
};

// Опрос токена - как у драйвера на Z80: до 0x4000 раз.
ReadRes read_block(uint32_t lba) {
    uint8_t out[512];
    ReadRes rr;
    sel(true);
    if (cmd(17, lba) != 0) {
        sel(false);
        return rr;
    }
    uint8_t t = 0xFF;
    for (rr.polls = 0; rr.polls < 0x4000; ++rr.polls) {
        t = x(0xFF);
        if (t != 0xFF) break;
    }
    if (t != 0xFE) {
        sel(false);
        return rr;
    }
    for (int i = 0; i < 512; ++i)
        out[i] = x(0xFF);
    const uint8_t hi = x(0xFF), lo = x(0xFF);
    rr.crc_ok  = devices::sd::crc16(out, 512) == static_cast<uint16_t>((hi << 8) | lo);
    rr.data_ok = std::memcmp(out, g_disk[lba], 512) == 0;
    sel(false);
    x(0xFF);
    rr.ok = true;
    return rr;
}

// CMD24 и блок, без ожидания занятости; выбор остаётся. Ответ на данные
// (младшие пять бит, 0x05 - принято).
uint8_t send_block(uint32_t lba, uint8_t fill) {
    uint8_t data[512];
    std::memset(data, fill, sizeof(data));
    sel(true);
    if (cmd(24, lba) != 0) return 0xFF;
    x(0xFF);
    x(0xFE);
    for (int i = 0; i < 512; ++i)
        x(data[i]);
    const uint16_t c = devices::sd::crc16(data, 512);
    x(static_cast<uint8_t>(c >> 8));
    x(static_cast<uint8_t>(c));
    return static_cast<uint8_t>(x(0xFF) & 0x1F);
}

uint8_t write_block(uint32_t lba, uint8_t fill) {
    const uint8_t resp = send_block(lba, fill);
    for (int busy = 0; x(0xFF) == 0x00 && busy < 100000; ++busy) {
    }
    sel(false);
    x(0xFF);
    return resp;
}

bool sector_filled(uint32_t lba, uint8_t fill) {
    for (uint32_t i = 0; i < 512; ++i) {
        if (g_disk[lba][i] != fill) return false;
    }
    return true;
}

bool read_matches(const char* name, uint32_t lba) {
    const ReadRes rr = read_block(lba);
    std::printf("  %s: sector %u - read %d, data %d, CRC %d\n", name, lba, rr.ok, rr.data_ok, rr.crc_ok);
    return rr.ok && rr.data_ok && rr.crc_ok;
}

// (а) Чтение после записи отдаёт носитель - и данные, и CRC блока.
void test_read_after_write() {
    std::printf("test_sd_emu_read_after_write\n");
    // A: прочитать 100, записать 200, прочитать 100 (сектор FAT до и после записи данных).
    fresh_card();
    CHECK(read_matches("A1", 100));
    CHECK_EQ(write_block(200, 0xA5), 0x05);
    CHECK_EQ(g_writes, 1u);
    CHECK(g_disk[200][0] == 0xA5);
    CHECK(read_matches("A3", 100));
    // B: прочитать 300 (упреждение кладёт 301), записать 301, прочитать 301 и 300.
    fresh_card();
    CHECK(read_matches("B1", 300));
    CHECK_EQ(write_block(301, 0x5A), 0x05);
    CHECK(read_matches("B3", 301));
    CHECK(read_matches("B4", 300));
    // F: прочитать 900, записать 900 (правка FAT на месте), прочитать 900.
    fresh_card();
    CHECK(read_matches("F1", 900));
    CHECK_EQ(write_block(900, 0x3C), 0x05);
    CHECK(read_matches("F3", 900));
    // C: CMD18 на два блока с 400, CMD12, запись 600, чтение 402 и 401.
    fresh_card();
    sel(true);
    CHECK_EQ(cmd(18, 400), 0x00);
    for (int blk = 0; blk < 2; ++blk) {
        uint8_t t = 0xFF;
        for (int p = 0; p < 0x4000 && t == 0xFF; ++p)
            t = x(0xFF);
        for (int i = 0; i < 512 + 2; ++i)
            x(0xFF);
    }
    // (б) CMD12 посреди CMD18 - R1 0x00.
    CHECK_EQ(cmd(12, 0), 0x00);
    sel(false);
    x(0xFF);
    CHECK_EQ(write_block(600, 0x77), 0x05);
    CHECK(read_matches("C3", 402));
    CHECK(read_matches("C4", 401));
}

// Запись платы (Beta Disk, инструменты, FatFs) в сектор, который сторона уже
// упредила в слот: следующий CMD17 отдаёт записанное, а не старое из слота.
void test_board_write_drops_emulator_slot() {
    std::printf("test_sd_emu_board_write_drops_emulator_slot\n");
    fresh_card();
    CHECK(read_matches("W1", 300)); // упреждение кладёт 301 в слот
    uint8_t in[512];
    std::memset(in, 0x6B, sizeof(in));
    CHECK(devices::storage::storage_write_board(301, in));
    CHECK(g_disk[301][0] == 0x6B);
    CHECK(read_matches("W3", 301));
}

// (е) Хост снимает выбор сразу после ответа на блок и при следующем выборе
// ждёт 0xFF, как FatFs; цикл Core1 до записи ещё не дошёл. Карта занята,
// пока блок не записан, и вторая запись не затирает первую.
void test_write_reselect_busy() {
    std::printf("test_sd_emu_write_reselect_busy\n");
    fresh_card();
    g_task_enabled = false;
    CHECK_EQ(send_block(10, 0x11), 0x05);
    sel(false);
    x(0xFF);
    sel(true);
    // Цикл просыпается на 20-м опросе; готовая раньше карта получила бы
    // вторую запись, пока первая не на носителе.
    uint32_t busy = 0;
    for (; busy < 100000; ++busy) {
        if (busy == 20) g_task_enabled = true;
        if (x(0xFF) == 0xFF) break;
    }
    std::printf("  busy for %u polls after reselect\n", busy);
    CHECK(busy >= 20);
    CHECK_EQ(send_block(20, 0x22), 0x05);
    g_task_enabled = true;
    for (int i = 0; x(0xFF) == 0x00 && i < 100000; ++i) {
    }
    sel(false);
    x(0xFF);
    CHECK_EQ(g_writes, 2u);
    CHECK(sector_filled(10, 0x11));
    CHECK(sector_filled(20, 0x22));
}

// (ж) Цикл Core1 читает сектор упреждения, а хост в это время (обработчик
// прерывает цикл) обрывает CMD18 и пишет другой сектор: блок хоста и сектор
// упреждения не делят буфер.
void hook_write_30() {
    g_task_enabled = false;
    CHECK_EQ(cmd(12, 0), 0x00);
    CHECK_EQ(send_block(30, 0xC3), 0x05);
    sel(false);
    x(0xFF);
}

void test_write_during_prefetch() {
    std::printf("test_sd_emu_write_during_prefetch\n");
    fresh_card();
    sel(true);
    CHECK_EQ(cmd(18, 5), 0x00);
    // Сектор 5 отдаётся, заказ упреждения 6 цикл не берёт до конца блока.
    g_task_enabled = false;
    uint8_t t      = 0xFF;
    for (int p = 0; p < 0x4000; ++p) {
        t = x(0xFF);
        if (t != 0xFF) break;
        devices::sd::sd_spi_task();
    }
    CHECK_EQ(t, 0xFE);
    for (int i = 0; i < 512 + 2; ++i)
        x(0xFF);
    g_hook_lba  = 6;
    g_read_hook = hook_write_30;
    devices::sd::sd_spi_task();
    g_task_enabled = true;
    for (int i = 0; i < 4; ++i)
        x(0xFF);
    CHECK_EQ(g_writes, 1u);
    CHECK(sector_filled(30, 0xC3));
    CHECK(read_matches("6", 6));
    CHECK(read_matches("5", 5));
}

// Хост бросает сектор посреди его чтения с носителя и просит другой, а
// посреди чтения второго - снова первый и ждёт токен. Слот первого в это
// время пишется вторым чтением: токен уходит только после нового чтения
// первого сектора, а не с половиной чужого.
constexpr uint32_t kRaceA = 1000, kRaceB = 2000;
uint8_t g_race_out[512];
int g_race_polls = 0;
bool g_race_got  = false;
bool race_poll(int limit) {
    for (; limit > 0; --limit, ++g_race_polls) {
        if (x(0xFF) == 0xFE) {
            for (int i = 0; i < 512; ++i)
                g_race_out[i] = x(0xFF);
            return true;
        }
    }
    return false;
}
void hook_race_b() {
    sel(false);
    x(0xFF);
    sel(true);
    if (cmd(17, kRaceA) == 0) g_race_got = race_poll(2000);
}
void hook_race_a() {
    sel(false);
    x(0xFF);
    sel(true);
    cmd(17, kRaceB);
    g_hook_lba  = kRaceB;
    g_read_hook = hook_race_b;
}

void test_slot_reask_during_read() {
    std::printf("test_sd_emu_slot_reask_during_read\n");
    fresh_card();
    g_race_polls = 0;
    g_race_got   = false;
    std::memset(g_race_out, 0, sizeof(g_race_out));
    g_hook_lba  = kRaceA;
    g_read_hook = hook_race_a;
    sel(true);
    // Чтение A - отдельным витком цикла, а не внутри ответа на команду.
    g_task_enabled = false;
    CHECK_EQ(cmd(17, kRaceA), 0x00);
    g_task_enabled = true;
    devices::sd::sd_spi_task();
    const bool in_hook = g_race_got;
    if (!g_race_got) g_race_got = race_poll(0x4000);
    int bad = 0;
    for (int i = 0; i < 512; ++i)
        bad += g_race_out[i] != g_disk[kRaceA][i];
    std::printf("  re-asked sector: token %d (inside B read %d), polls %d, bytes differing %d\n", g_race_got, in_hook, g_race_polls, bad);
    CHECK(g_race_got);
    CHECK_EQ(bad, 0);
    sel(false);
    x(0xFF);
}

// Отложенная смена у стороны, которую хост не тактирует: флешку у второй
// карты DivMMC вынули и вставили, а машина её не выбирает. Первая смена
// ждёт её такта; сверка обязана ждать этого же, а не заходить в снимок
// на каждом витке цикла.
void test_deferred_media_change_idle() {
    std::printf("test_sd_emu_deferred_media_change_idle\n");
    fresh_card();
    devices::storage::storage_set_emulator_medium(SdOwner::DivMmc2, devices::hal::Medium::Usb);
    // Снимок флешки в заглушке совпадает с картой: первая смена - вынули,
    // вторая, пока первая не принята, - вставили снова.
    g_usb_present = true;
    devices::sd::sd_spi_task();
    g_usb_present = false;
    devices::sd::sd_spi_task();
    g_usb_present = true;
    devices::sd::sd_spi_task();
    const uint32_t calls0 = devices::hal::g_media_state_calls;
    for (int i = 0; i < 1000; ++i)
        devices::sd::sd_spi_task();
    const uint32_t calls = devices::hal::g_media_state_calls - calls0;
    std::printf("  media snapshot reads in 1000 idle turns: %u\n", calls);
    CHECK(calls <= 3u);
    // Такт стороны принимает первую смену (флешки нет), следующий виток -
    // отложенную вторую: флешка на месте, карта поднимается.
    g_who = SdOwner::DivMmc2;
    sel(true);
    sel(false);
    devices::sd::sd_spi_task();
    CHECK(init_card());
    g_who = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(SdOwner::DivMmc2, devices::hal::Medium::Card);
}

// Сектор приходит поздно, но в пределах терпения хоста: цикл Core1 занят
// 5000 опросов (около 75 мс).
void test_late_sector() {
    std::printf("test_sd_emu_late_sector\n");
    fresh_card();
    sel(true);
    g_task_enabled = false;
    CHECK_EQ(cmd(17, 700), 0x00);
    uint32_t polls = 0;
    uint8_t t      = 0xFF;
    for (; polls < 0x4000; ++polls) {
        if (polls == 5000) g_task_enabled = true;
        t = x(0xFF);
        if (t != 0xFF) break;
    }
    uint8_t buf[512];
    for (int i = 0; i < 512; ++i)
        buf[i] = x(0xFF);
    x(0xFF);
    x(0xFF);
    sel(false);
    x(0xFF);
    std::printf("  token 0x%02X after %u polls\n", t, polls);
    CHECK_EQ(t, 0xFE);
    CHECK(std::memcmp(buf, g_disk[700], 512) == 0);
}

// (г) Носитель не отдал сектор: хост получает 0xFF до своего предела,
// следующее чтение того же сектора проходит.
void test_media_read_failure() {
    std::printf("test_sd_emu_media_read_failure\n");
    fresh_card();
    // Счётчик стороны: без него отказ носителя виден хосту только таймаутом,
    // а в журнале платы не виден вовсе.
    const uint32_t fails_before = devices::sd::sd_spi_media_failures(g_who);
    g_fail_read_lba             = 1234;
    const ReadRes bad           = read_block(1234);
    std::printf("  medium failure: token received %d, polls %u\n", bad.ok, bad.polls);
    CHECK(!bad.ok);
    CHECK(devices::sd::sd_spi_media_failures(g_who) > fails_before);
    g_fail_read_lba = 0xFFFFFFFFu;
    CHECK(read_matches("repeat", 1234));
}

// Ответы как у настоящей карты. CMD58 до инициализации - R1 idle и OCR без
// бита "питание поднято" (настоящая карта на плате: "CMD58 0x01, window
// 40FF8000"), после ACMD41 - готова, C0FF8000. CMD13 после записи, которую
// носитель не принял, - во втором байте R2 бит общей ошибки 0x04; после
// исправной - ноль.
void test_ocr_and_status_like_real_card() {
    std::printf("test_sd_emu_ocr_and_status_like_real_card\n");
    fill_disk();
    devices::sd::sd_spi_emu_init();
    uint8_t ocr[4];
    sel(true);
    CHECK_EQ(cmd(0, 0), 0x01);
    CHECK_EQ(cmd(58, 0), 0x01);
    for (uint8_t& b : ocr)
        b = x(0xFF);
    CHECK_EQ(ocr[0], 0x40);
    CHECK_EQ(ocr[1], 0xff);
    CHECK_EQ(ocr[2], 0x80);
    sel(false);
    CHECK(init_card());
    sel(true);
    CHECK_EQ(cmd(58, 0), 0x00);
    for (uint8_t& b : ocr)
        b = x(0xFF);
    CHECK_EQ(ocr[0], 0xc0);
    sel(false);

    g_fail_write_lba = 300;
    write_block(300, 0x11);
    sel(true);
    CHECK_EQ(cmd(13, 0), 0x00);
    CHECK_EQ(x(0xFF), 0x04);
    sel(false);
    g_fail_write_lba = 0xFFFFFFFFu;
    CHECK_EQ(write_block(301, 0x22), 0x05);
    sel(true);
    CHECK_EQ(cmd(13, 0), 0x00);
    CHECK_EQ(x(0xFF), 0x00);
    sel(false);
}

// (в) Автоматы развязаны и (д) единственный писатель: Z-Controller
// выбрал карту и ушёл, не сняв выбор; DivMMC всё это время работает как ни
// в чём не бывало - у него свой автомат. Писатель при этом один:
// Z-Controller уже писал, CMD24 второго - 0x40, носитель не тронут.
void test_split_cards_and_single_writer() {
    std::printf("test_sd_emu_split_cards_and_single_writer\n");
    fresh_card();
    CHECK_EQ(write_block(50, 0x11), 0x05); // писатель - Z-Controller

    // Z-Controller выбрал карту и бросил посреди разговора.
    sel(true);
    x(0xFF);

    // DivMMC поднимает свою карту с нуля: CMD0 отвечает 0x01, как
    // настоящая после включения.
    devices::sd::sd_spi_select(SdOwner::DivMmc, true);
    uint8_t f0[6] = {0x40 | 0, 0, 0, 0, 0, 0};
    f0[5]         = devices::sd::crc7(f0, 5);
    for (int i = 0; i < 6; ++i)
        devices::sd::sd_spi_byte(SdOwner::DivMmc, f0[i]);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && (r & 0x80); ++i)
        r = devices::sd::sd_spi_byte(SdOwner::DivMmc, 0xFF);
    std::printf("  CMD0 from the second side with the other exchange abandoned -> 0x%02X\n", r);
    CHECK_EQ(r, 0x01);
    // ACMD41: запись принимается только у инициализированной карты.
    const uint8_t init_cmds[] = {55, 41};
    for (const uint8_t c : init_cmds) {
        uint8_t fa[6] = {static_cast<uint8_t>(0x40 | c), c == 41 ? uint8_t{0x40} : uint8_t{0}, 0, 0, 0, 0};
        fa[5]         = devices::sd::crc7(fa, 5);
        for (int i = 0; i < 6; ++i)
            devices::sd::sd_spi_byte(SdOwner::DivMmc, fa[i]);
        r = 0xFF;
        for (int i = 0; i < 10 && (r & 0x80); ++i)
            r = devices::sd::sd_spi_byte(SdOwner::DivMmc, 0xFF);
    }
    CHECK_EQ(r, 0x00);

    // Ни ожидания, ни отъёма: соседа для этой стороны просто нет.
    uint8_t f[6] = {0x40 | 24, 0, 0, 0x03, 0x00, 0};
    f[5]         = devices::sd::crc7(f, 5);
    for (int i = 0; i < 6; ++i)
        devices::sd::sd_spi_byte(SdOwner::DivMmc, f[i]);
    r = 0xFF;
    for (int i = 0; i < 10 && (r & 0x80); ++i)
        r = devices::sd::sd_spi_byte(SdOwner::DivMmc, 0xFF);
    std::printf("  CMD24 from the second writer -> 0x%02X, writes %u\n", r, g_writes);
    CHECK_EQ(r, 0x40);
    CHECK_EQ(g_writes, 1u);
    devices::sd::sd_spi_select(SdOwner::DivMmc, false);

    // А брошенный обмен Z-Controller цел: его фаза не тронута, команда
    // проходит как обычно.
    CHECK_EQ(cmd(13, 0), 0x00);
    sel(false);
}

// CMD25: токен 0xfc на каждый блок, 0xfd в конце. Байт 0xfe внутри данных -
// просто данные. Возвращает ответ на данные блока (младшие пять бит).
uint8_t multi_block(const uint8_t* data) {
    x(0xFF);
    x(0xFC);
    for (int i = 0; i < 512; ++i)
        x(data[i]);
    const uint16_t c = devices::sd::crc16(data, 512);
    x(static_cast<uint8_t>(c >> 8));
    x(static_cast<uint8_t>(c));
    const uint8_t resp = static_cast<uint8_t>(x(0xFF) & 0x1F);
    for (int busy = 0; x(0xFF) == 0x00 && busy < 100000; ++busy) {
    }
    return resp;
}

void test_multi_block_write() {
    std::printf("test_sd_emu_multi_block_write\n");
    fresh_card();
    uint8_t data[3][512];
    for (uint32_t k = 0; k < 3; ++k) {
        for (uint32_t i = 0; i < 512; ++i)
            data[k][i] = static_cast<uint8_t>(0x30u + k * 0x11u + i);
        data[k][100] = 0xFE; // байт токена CMD24 внутри данных
    }
    sel(true);
    CHECK_EQ(cmd(25, 200), 0x00);
    for (uint32_t k = 0; k < 3; ++k)
        CHECK_EQ(multi_block(data[k]), 0x05);
    x(0xFD);
    for (int busy = 0; x(0xFF) == 0x00 && busy < 100000; ++busy) {
    }
    sel(false);
    x(0xFF);
    for (uint32_t k = 0; k < 3; ++k)
        CHECK(std::memcmp(g_disk[200 + k], data[k], 512) == 0);
    CHECK_EQ(g_writes, 3u);

    // Второй блок за концом носителя: ответ на данные - ошибка записи,
    // носитель не тронут, CMD13 отдаёт ошибку во втором байте.
    const uint32_t last = kDiskSectors - 1;
    sel(true);
    CHECK_EQ(cmd(25, last), 0x00);
    CHECK_EQ(multi_block(data[0]), 0x05);
    CHECK_EQ(multi_block(data[1]), 0x0D);
    x(0xFD);
    for (int busy = 0; x(0xFF) == 0x00 && busy < 100000; ++busy) {
    }
    CHECK_EQ(cmd(13, 0), 0x00);
    CHECK_EQ(x(0xFF), devices::sd::kR2Error);
    sel(false);
    x(0xFF);
    CHECK(std::memcmp(g_disk[last], data[0], 512) == 0);
    CHECK_EQ(g_writes, 4u);
}

// Сторона на флешке заказывает сектор, пока идёт обмен платы с флешкой (виток
// карты пришёл из его ожидания): заказ ждёт, хост ждёт токен, а не получает
// ошибку исправной флешки; освободилась - сектор отдан.
void test_busy_medium_request_waits() {
    std::printf("test_sd_emu_busy_medium_request_waits\n");
    fill_disk();
    g_usb_present = true;
    devices::storage::storage_set_emulator_medium(SdOwner::DivMmc, devices::hal::Medium::Usb);
    devices::storage::storage_init();
    devices::sd::sd_spi_emu_init();
    g_who = SdOwner::DivMmc;
    CHECK(init_card());
    devices::hal::g_usb_busy   = true;
    devices::hal::g_usb_nested = 0;
    const ReadRes waiting      = read_block(1500);
    devices::hal::g_usb_busy   = false;
    const ReadRes answered     = read_block(1500);
    std::printf("  flash busy: token after %u polls (waits %d); free: after %u polls\n", waiting.polls, waiting.polls >= 0x4000 ? 1 : 0, answered.polls);
    std::printf("  nested exchanges with the busy flash: %u (want 0)\n", devices::hal::g_usb_nested);
    CHECK_EQ(devices::hal::g_usb_nested, 0u);
    CHECK(waiting.polls >= 0x4000u);
    CHECK(answered.polls < 0x4000u);
    g_who = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(SdOwner::DivMmc, devices::hal::Medium::Card);
}

// Замок писателя - по носителю: стороны на разных носителях пишут обе, на
// одном - вторая получает 0x40. Карту Z-Controller уже взял прошлыми
// тестами, замок держится до перезагрузки, поэтому DivMMC здесь на флешке.
void test_writer_lock_per_medium() {
    std::printf("test_sd_emu_writer_lock_per_medium\n");
    fill_disk();
    g_usb_present = true;
    const auto dm = SdOwner::DivMmc;
    const auto zc = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(dm, devices::hal::Medium::Usb);
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Card);
    devices::storage::storage_init();
    devices::sd::sd_spi_emu_init();
    g_who = SdOwner::DivMmc;
    CHECK(init_card());
    CHECK_EQ(write_block(50, 0x11), 0x05);
    g_who = SdOwner::ZController;
    CHECK(init_card());
    CHECK_EQ(write_block(51, 0x22), 0x05);
    CHECK_EQ(g_usb_disk[50][0], 0x11u);
    CHECK_EQ(g_disk[51][0], 0x22u);
    CHECK(g_disk[50][0] != 0x11);
    g_usb_present = false;
    g_who         = SdOwner::ZController;
}

// Запись за концом носителя отвергается до замка: сторона, ошибившаяся
// адресом, писателем не становится, и вторая пишет на тот же носитель.
void test_write_beyond_end_takes_no_lock() {
    std::printf("test_sd_emu_write_beyond_end_takes_no_lock\n");
    fill_disk();
    devices::sd::sd_spi_emu_init();
    g_who = SdOwner::ZController;
    CHECK(init_card());
    sel(true);
    CHECK_EQ(cmd(24, kDiskSectors), devices::sd::kR1ParamErr);
    sel(false);
    x(0xFF);
    g_who = SdOwner::DivMmc;
    CHECK(init_card());
    CHECK_EQ(write_block(10, 0x55), 0x05);
    CHECK(sector_filled(10, 0x55));
    CHECK(devices::storage::storage_writer(devices::hal::Medium::Card) == SdOwner::DivMmc);
    g_who = SdOwner::ZController;
}

// Блок карты - 16 секторов, как на плате: промах отдаёт сектор и заказывает
// блок, чтение со второй половины блока заказывает следующий.
void test_card_block_run16() {
    std::printf("test_sd_emu_card_block_run16\n");
    fill_disk();
    const auto dm = SdOwner::DivMmc;
    uint8_t out[512];
    uint32_t r0 = g_reads;
    CHECK(devices::storage::storage_emulator_read(dm, 33, out)); // промах, заказ 32..47
    CHECK_EQ(g_reads - r0, 1u);
    CHECK(std::memcmp(out, g_disk[33], 512) == 0);
    r0 = g_reads;
    devices::storage::storage_prefetch_step();
    CHECK_EQ(g_reads - r0, 16u);
    r0 = g_reads;
    CHECK(devices::storage::storage_emulator_read(dm, 40, out)); // попадание, вторая половина
    CHECK_EQ(g_reads - r0, 0u);
    CHECK(std::memcmp(out, g_disk[40], 512) == 0);
    devices::storage::storage_prefetch_step(); // следующий блок 48..63
    r0 = g_reads;
    CHECK(devices::storage::storage_emulator_read(dm, 50, out));
    CHECK_EQ(g_reads - r0, 0u);
    CHECK(std::memcmp(out, g_disk[50], 512) == 0);
}

// Запись оборвалась: что на носителе - неизвестно, блок с сектором
// сбрасывается, и следующее чтение идёт к носителю, а не отдаёт прежнее.
void test_failed_write_drops_block() {
    std::printf("test_sd_emu_failed_write_drops_block\n");
    fill_disk();
    const auto dm = SdOwner::DivMmc;
    uint8_t out[512];
    CHECK(devices::storage::storage_emulator_read(dm, 300, out));
    devices::storage::storage_prefetch_step(); // блок 288..303 в кэше
    uint8_t blk[512];
    std::memset(blk, 0x3C, sizeof(blk));
    g_fail_write_lba     = 300;
    g_fail_write_partial = true;
    CHECK(!devices::storage::storage_write(dm, 300, blk));
    g_fail_write_lba     = 0xFFFFFFFFu;
    g_fail_write_partial = false;
    CHECK(devices::storage::storage_emulator_read(dm, 300, out));
    CHECK(std::memcmp(out, g_disk[300], 512) == 0);
    CHECK_EQ(out[0], 0x3Cu);

    // То же через путь платы (FatFs): запись платы и фоновое чтение.
    CHECK(devices::storage::storage_read_background(310, out));
    devices::storage::storage_prefetch_step();
    g_fail_write_lba     = 310;
    g_fail_write_partial = true;
    CHECK(!devices::storage::storage_write_board(310, blk));
    g_fail_write_lba     = 0xFFFFFFFFu;
    g_fail_write_partial = false;
    CHECK(devices::storage::storage_read_background(310, out));
    CHECK(std::memcmp(out, g_disk[310], 512) == 0);
}

// Без блочного буфера (на плате - "block cache NO ROOM") работает
// построчный кэш: повтор сектора не идёт к носителю, запись обновляет
// строку, фоновое чтение в кэш не кладётся.
void test_line_cache_without_blocks() {
    std::printf("test_sd_emu_line_cache_without_blocks\n");
    fill_disk();
    devices::storage::storage_attach_block_cache(nullptr, 0, nullptr);
    const auto dm = SdOwner::DivMmc;
    uint8_t out[512];
    uint32_t r0 = g_reads;
    CHECK(devices::storage::storage_emulator_read(dm, 5, out));
    CHECK(devices::storage::storage_emulator_read(dm, 5, out));
    CHECK_EQ(g_reads - r0, 1u);
    uint8_t blk[512];
    std::memset(blk, 0x77, sizeof(blk));
    CHECK(devices::storage::storage_write(dm, 5, blk));
    r0 = g_reads;
    CHECK(devices::storage::storage_emulator_read(dm, 5, out));
    CHECK_EQ(g_reads - r0, 0u);
    CHECK_EQ(out[0], 0x77u);
    r0 = g_reads;
    CHECK(devices::storage::storage_read_background(6, out));
    CHECK(devices::storage::storage_read_background(6, out));
    CHECK_EQ(g_reads - r0, 2u);
    fill_disk(); // блочный буфер обратно
}

// Фоновый клиент на блочном пути пропускает хост перед каждым обращением к
// носителю: перед одиночным чтением промаха и перед наполнением блока по
// своему заказу. Попадание носитель не трогает - пропуска нет.
uint32_t g_yields = 0;
void count_yield(void*) {
    ++g_yields;
}

void test_background_yields_on_block_path() {
    std::printf("test_sd_emu_background_yields_on_block_path\n");
    fill_disk();
    devices::storage::storage_set_host_yield(&count_yield, nullptr);
    g_yields = 0;
    uint8_t out[512];
    CHECK(devices::storage::storage_read_background(65, out)); // промах
    CHECK_EQ(g_yields, 1u);
    devices::storage::storage_prefetch_step_board(); // блок 64..79 по фоновому заказу
    CHECK_EQ(g_yields, 2u);
    CHECK(devices::storage::storage_read_background(66, out)); // попадание
    CHECK_EQ(g_yields, 2u);
    // Заказ эмулятора наполняется без пропуска: хост здесь сам заказчик.
    const auto dm = SdOwner::DivMmc;
    CHECK(devices::storage::storage_emulator_read(dm, 129, out));
    devices::storage::storage_prefetch_step();
    CHECK_EQ(g_yields, 2u);
    devices::storage::storage_set_host_yield(nullptr, nullptr);
}

// Хост во время пропуска фонового заказа сам читает тот же блок: заказ
// после пропуска уже не наполняет его второй раз. Иначе блок лежит в двух
// слотах, запись правит один, чтение попадает в другой - старые данные.
// Пропуск - как виток хоста: чтение эмулятора и его шаг предвыборки.
void host_reads_ordered_block(void*) {
    uint8_t out[512];
    (void)devices::storage::storage_emulator_read(SdOwner::DivMmc, 98, out);
    devices::storage::storage_prefetch_step();
}

void test_yield_does_not_fill_block_twice() {
    std::printf("test_sd_emu_yield_does_not_fill_block_twice\n");
    fill_disk();
    uint8_t out[512];
    CHECK(devices::storage::storage_read_background(97, out)); // заказ блока 96..111
    devices::storage::storage_set_host_yield(&host_reads_ordered_block, nullptr);
    const uint32_t reads0 = g_reads;
    devices::storage::storage_prefetch_step_board();
    devices::storage::storage_set_host_yield(nullptr, nullptr);
    // Чтение хоста (сектор) и его наполнение блока - 17; второе наполнение - ещё 16.
    std::printf("  sectors read from the medium for block 96..111: %u (want 17)\n", g_reads - reads0);
    CHECK_EQ(g_reads - reads0, 17u);
    uint8_t in[512];
    std::memset(in, 0x5A, sizeof(in));
    CHECK(devices::storage::storage_write(SdOwner::DivMmc, 98, in)); // правит одну копию
    // Следующие заказы вытесняют слоты по старшинству: правленая копия
    // уходит первой, если копий две.
    for (uint32_t lba = 161; lba < 161 + 16u * 8u; lba += 16) {
        CHECK(devices::storage::storage_read_background(lba, out));
        devices::storage::storage_prefetch_step_board();
    }
    CHECK(devices::storage::storage_emulator_read(SdOwner::DivMmc, 98, out));
    std::printf("  host read during the yield, then write: sector reads back %02X (want 5A)\n", out[0]);
    CHECK_EQ(out[0], 0x5A);
}

// Носитель не принял сектор записи: отказ виден в строке sd: у стороны.
void test_write_failure_is_logged() {
    std::printf("test_sd_emu_write_failure_is_logged\n");
    fresh_card();
    g_fail_write_lba = 600;
    write_block(600, 0x44);
    g_fail_write_lba = 0xFFFFFFFFu;
    g_test_log[0]    = 0;
    devices::sd::sd_spi_log_stats();
    const char* z = std::strstr(g_test_log, "zctrl cmd=");
    CHECK(z != nullptr);
    const char* w = z ? std::strstr(z, "wfail=") : nullptr;
    CHECK(w != nullptr && std::strtoul(w + 6, nullptr, 10) >= 1ul);
}

// Флешку вынули и вставили другую: блоки и заказ упреждения прежней
// флешки не отдаются.
void test_media_swap_drops_blocks() {
    std::printf("test_sd_emu_media_swap_drops_blocks\n");
    fill_disk();
    const auto zc = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Usb);
    g_usb_present = true;
    for (uint32_t s = 0; s < 16; ++s)
        std::memset(g_usb_disk[s], 0x11, 512);
    uint8_t out[512];
    CHECK(devices::storage::storage_emulator_read(zc, 9, out)); // промах, заказ блока 8..15
    devices::storage::storage_prefetch_step();
    const uint32_t reads = g_reads;
    CHECK(devices::storage::storage_emulator_read(zc, 10, out)); // попадание в блок
    CHECK_EQ(g_reads, reads);
    CHECK_EQ(out[0], 0x11u);

    g_usb_present = false;
    CHECK(!devices::storage::storage_emulator_read(zc, 10, out));
    for (uint32_t s = 0; s < 16; ++s)
        std::memset(g_usb_disk[s], 0x22, 512);
    g_usb_present = true;
    CHECK(devices::storage::storage_emulator_read(zc, 10, out));
    CHECK_EQ(out[0], 0x22u);
    g_usb_present = false;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Card);
}

// Первый байт сектора, прочитанного CMD17; -1 - чтение не прошло.
int read_first_byte(uint32_t lba) {
    sel(true);
    if (cmd(17, lba) != 0) {
        sel(false);
        return -1;
    }
    uint8_t t = 0xFF;
    for (int polls = 0; polls < 0x4000 && t == 0xFF; ++polls)
        t = x(0xFF);
    if (t != 0xFE) {
        sel(false);
        return -1;
    }
    const uint8_t first = x(0xFF);
    for (int i = 1; i < 512 + 2; ++i)
        x(0xFF);
    sel(false);
    x(0xFF);
    return first;
}

// Флешку сменили, пока её сектор лежал в слоте эмулятора: после новой
// инициализации хост читает сектор новой флешки, а не слот прежней.
void test_media_swap_drops_slots() {
    std::printf("test_sd_emu_media_swap_drops_slots\n");
    fill_disk();
    const auto zc = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Usb);
    g_usb_present = true;
    for (uint32_t s = 0; s < 32; ++s)
        std::memset(g_usb_disk[s], 0x11, 512);
    devices::storage::storage_init();
    devices::sd::sd_spi_emu_init();
    g_who = zc;
    CHECK(init_card());
    CHECK_EQ(read_first_byte(10), 0x11);
    const uint32_t reads = g_reads;
    CHECK_EQ(read_first_byte(10), 0x11); // из слота, носитель не тронут
    CHECK_EQ(g_reads, reads);

    // Смену эмулятор принимает на такте выбранной карты.
    sel(true);
    g_usb_present = false;
    for (int i = 0; i < 4; ++i)
        x(0xFF);
    for (uint32_t s = 0; s < 32; ++s)
        std::memset(g_usb_disk[s], 0x22, 512);
    g_usb_present = true;
    for (int i = 0; i < 4; ++i)
        x(0xFF);
    sel(false);
    // До новой инициализации чтение и запись сектора - idle и недопустимая
    // команда, как у настоящей карты; запись на носитель не уходит.
    const uint32_t writes = g_writes;
    sel(true);
    CHECK_EQ(cmd(17, 10), 0x05u);
    CHECK_EQ(cmd(24, 20), 0x05u);
    sel(false);
    x(0xFF);
    CHECK_EQ(g_writes, writes);
    CHECK(init_card());
    CHECK_EQ(read_first_byte(10), 0x22);

    g_usb_present = false;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Card);
}

// Флешку вынули и вставили, пока машина карту не тактировала (WC или
// esxDOS в простое): машина обязана увидеть сначала "нет носителя", затем
// носитель снова.
void test_media_swap_while_idle() {
    std::printf("test_sd_emu_media_swap_while_idle\n");
    fill_disk();
    const auto zc = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Usb);
    g_usb_present = true;
    for (uint32_t s = 0; s < 32; ++s)
        std::memset(g_usb_disk[s], 0x11, 512);
    devices::storage::storage_init();
    devices::sd::sd_spi_emu_init();
    g_who = zc;
    CHECK(init_card());
    CHECK_EQ(read_first_byte(10), 0x11);

    g_usb_present = false;
    devices::sd::sd_spi_task();
    for (uint32_t s = 0; s < 32; ++s)
        std::memset(g_usb_disk[s], 0x33, 512);
    g_usb_present = true;
    for (int i = 0; i < 4; ++i)
        devices::sd::sd_spi_task();
    // Первый такт хоста принимает "нет носителя", следующий виток готовит
    // носитель снова: инициализация проходит, а не глохнет до перезагрузки.
    CHECK(init_card());
    CHECK_EQ(read_first_byte(10), 0x33);

    g_usb_present = false;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Card);
}

// Другая карта той же ёмкости: снимок отличается только номером. Машина
// обязана пройти инициализацию заново и получить данные новой карты, а не
// кэш прежней.
void test_card_swapped_same_size() {
    std::printf("test_sd_emu_card_swapped_same_size\n");
    fill_disk();
    devices::storage::storage_init();
    devices::sd::sd_spi_emu_init();
    g_who = SdOwner::ZController;
    CHECK(init_card());
    const uint8_t old_byte = read_first_byte(10);
    std::memset(g_disk[10], 0x44, 512);
    g_card_serial = 0x1234u;
    for (int i = 0; i < 4; ++i)
        devices::sd::sd_spi_task();
    sel(true);
    x(0xFF); // такт хоста принимает смену
    CHECK_EQ(cmd(17, 10), 0x05u);
    sel(false);
    x(0xFF);
    CHECK(init_card());
    CHECK_EQ(read_first_byte(10), 0x44);
    CHECK(old_byte != 0x44);
    g_card_serial = 0;
}

// Машина через эмулятор карты переписала сектор каталога тома 0, пока
// FatFs платы держит его в окне: повторный volume_attach сбрасывает чистое
// окно, и следующее обращение видит записанное, а не копию в окне (иначе
// сброс окна вернул бы на карту старый сектор).
void build_fat12_on_card() {
    std::memset(g_disk, 0, 128 * 512);
    uint8_t* b           = g_disk[0];
    const uint8_t boot[] = {0xEB, 0x3C, 0x90, 'M', 'S', 'D', 'O', 'S', '5', '.', '0', 0x00, 0x02, 1, 1, 0, 1, 16, 0, 128, 0, 0xF8, 1, 0};
    std::memcpy(b, boot, sizeof(boot));
    b[0x26] = 0x29;
    std::memcpy(b + 0x36, "FAT12   ", 8);
    b[510]              = 0x55;
    b[511]              = 0xAA;
    const uint8_t fat[] = {0xF8, 0xFF, 0xFF, 0xFF, 0x0F}; // кластер 2 - конец цепочки
    std::memcpy(g_disk[1], fat, sizeof(fat));
    uint8_t* e = g_disk[2];
    std::memcpy(e, "TEST    TXT", 11);
    e[11] = 0x20;
    e[26] = 2;  // первый кластер
    e[28] = 10; // размер
    std::memcpy(g_disk[3], "0123456789", 10);
}

// FatFs на ПК читает через переходник ПК: ему - сектора заглушки карты.
bool card_sector(unsigned long lba, unsigned char* out) {
    if (lba >= kDiskSectors) return false;
    std::memcpy(out, g_disk[lba], 512);
    return true;
}

void test_volume_window_sees_machine_write() {
    std::printf("test_sd_emu_volume_window_sees_machine_write\n");
    fill_disk();
    build_fat12_on_card();
    devices::storage::volume_detach(devices::storage::kVolumeCard);
    platform_pc::mount_sector_reader(&card_sector);
    static FATFS fs;
    CHECK(devices::storage::volume_attach(devices::storage::kVolumeCard, &fs));
    FILINFO a{}, stale{}, fresh{};
    CHECK_EQ(f_stat("0:/TEST.TXT", &a), FR_OK);
    g_disk[2][28] = 11;                             // машина дописала байт: сектор каталога переписан
    CHECK_EQ(f_stat("0:/TEST.TXT", &stale), FR_OK); // окно ещё старое
    CHECK(devices::storage::volume_attach(devices::storage::kVolumeCard, &fs));
    CHECK_EQ(f_stat("0:/TEST.TXT", &fresh), FR_OK);
    std::printf("  TEST.TXT size %u, before attach %u, after attach %u (want 11)\n", static_cast<unsigned>(a.fsize), static_cast<unsigned>(stale.fsize),
                static_cast<unsigned>(fresh.fsize));
    CHECK_EQ(fresh.fsize, 11u);
    devices::storage::volume_detach(devices::storage::kVolumeCard);
    platform_pc::mount_sector_reader(nullptr);
    fill_disk();
}

// Карту сменили под смонтированным томом 0 (другой номер той же ёмкости):
// disk_status - STA_NOINIT, FatFs монтирует заново; сброс окна прежнего тома
// на новую карту не пишется.
void test_volume_sees_card_swap() {
    std::printf("test_sd_emu_volume_sees_card_swap\n");
    fill_disk();
    devices::storage::volume_remember_medium(0); // disk_initialize
    CHECK(devices::storage::volume_same_medium(0));
    g_card_serial = 0x4242u;
    std::printf("  swapped card: same medium %d (want 0)\n", devices::storage::volume_same_medium(0) ? 1 : 0);
    CHECK(!devices::storage::volume_same_medium(0)); // disk_status - STA_NOINIT
    devices::storage::volume_remember_medium(0);     // перемонтирование
    CHECK(devices::storage::volume_same_medium(0));
    g_card_serial = 0;
}

// Чужой носитель сменился (флешка поднялась после того, как машина уже
// прошла инициализацию карты): карта стороны та же и остаётся готовой.
// Номер карты снимается при подъёме эмулятора, иначе первая же смена
// поколения принимала карту за другую.
void test_card_kept_on_foreign_change() {
    std::printf("test_sd_emu_card_kept_on_foreign_change\n");
    fill_disk();
    g_card_serial = 0x5678u;
    devices::storage::storage_init();
    devices::sd::sd_spi_emu_init();
    g_who = SdOwner::ZController;
    CHECK(init_card());
    g_usb_present = true;
    for (int i = 0; i < 4; ++i)
        devices::sd::sd_spi_task();
    CHECK_EQ(read_first_byte(10), g_disk[10][0]);
    g_usb_present = false;
    g_card_serial = 0;
}

// --- Хост через порты карты, как драйвер sd.s ---

// Порты и значения выбора: у Z-Controller выбор битом 1 порта 0x77, у карт
// DivMMC - битами 0 и 1 порта 0xE7.
struct HostPorts {
    uint8_t data;
    uint8_t ctrl;
    uint8_t sel;
    uint8_t desel;
};
constexpr HostPorts kZcHost{devices::zcontroller::kPortData, devices::zcontroller::kPortCtrl, 0x01, 0x03};
constexpr uint8_t kZcData = kZcHost.data;
constexpr uint8_t kZcCtrl = kZcHost.ctrl;
HostPorts g_host          = kZcHost;

void zout(uint8_t port, uint8_t v) {
    g_test_time_us += 15;
    g_port_wr[port](port, v);
    devices::sd::sd_spi_task();
}

uint8_t zin(uint8_t port) {
    g_test_time_us  += 15;
    const uint8_t v  = g_port_rd[port];
    if (g_port_rd_done[port]) g_port_rd_done[port](port);
    devices::sd::sd_spi_task();
    return v;
}

void zrelease() {
    zout(g_host.ctrl, g_host.desel);
    zout(g_host.data, 0xFF);
}

// sd_cmd: снять выбор, такт, выбрать, такт, шесть байт, до десяти IN.
// CRC настоящий только у CMD0 и CMD8.
bool zcmd(uint8_t c, uint32_t arg, uint8_t* r1) {
    zout(g_host.ctrl, g_host.desel);
    zout(g_host.data, 0xFF);
    zout(g_host.ctrl, g_host.sel);
    zout(g_host.data, 0xFF);
    zout(g_host.data, static_cast<uint8_t>(0x40 | c));
    for (int sh = 24; sh >= 0; sh -= 8)
        zout(g_host.data, static_cast<uint8_t>(arg >> sh));
    zout(g_host.data, c == 0 ? 0x95 : (c == 8 ? 0x87 : 0x01));
    for (int i = 0; i < 10; ++i) {
        *r1 = zin(g_host.data);
        if ((*r1 & 0x80) == 0) return true;
    }
    return false;
}

bool zinit() {
    zout(g_host.ctrl, g_host.desel);
    for (int i = 0; i < 500; ++i)
        zout(g_host.data, 0xFF);
    uint8_t r = 0xFF;
    if (!zcmd(0, 0, &r) || r != 0x01) return false;
    if (!zcmd(8, 0x1AA, &r) || r != 0x01) return false;
    zin(g_host.data);
    zin(g_host.data);
    if (zin(g_host.data) != 0x01 || zin(g_host.data) != 0xAA) return false;
    for (int i = 0; i < 100; ++i) {
        if (!zcmd(55, 0, &r) || !zcmd(41, 0x40000000u, &r)) return false;
        if (r == 0) break;
    }
    if (r != 0) return false;
    if (!zcmd(58, 0, &r) || r != 0) return false;
    const bool ccs = (zin(g_host.data) & 0x40) != 0;
    zin(g_host.data);
    zin(g_host.data);
    zin(g_host.data);
    zrelease();
    return ccs;
}

bool ztoken() {
    for (int i = 0; i < 0x4000; ++i) {
        const uint8_t t = zin(g_host.data);
        if (t != 0xFF) return t == 0xFE;
    }
    return false;
}

bool zread_sector(uint32_t lba, uint8_t* out) {
    uint8_t r = 0xFF;
    if (!zcmd(17, lba, &r) || r != 0 || !ztoken()) {
        zrelease();
        return false;
    }
    for (int i = 0; i < 512; ++i)
        out[i] = zin(g_host.data);
    zin(g_host.data);
    zin(g_host.data);
    zrelease();
    return true;
}

// CMD18 на count секторов, каждый сверяется с носителем; CMD12, ожидание
// 0xFF, отпуск шины.
uint32_t zread_multi_bad(uint32_t lba, uint32_t count) {
    uint8_t r = 0xFF;
    if (!zcmd(18, lba, &r) || r != 0) {
        zrelease();
        return count;
    }
    uint32_t bad = 0;
    uint8_t buf[512];
    for (uint32_t s = 0; s < count; ++s) {
        if (!ztoken()) {
            bad += count - s;
            break;
        }
        for (int i = 0; i < 512; ++i)
            buf[i] = zin(g_host.data);
        zin(g_host.data);
        zin(g_host.data);
        if (std::memcmp(buf, g_disk[lba + s], 512) != 0) ++bad;
    }
    zcmd(12, 0, &r);
    for (int i = 0; i < 0x4000 && zin(g_host.data) != 0xFF; ++i) {
    }
    zrelease();
    return bad;
}

// Запись в порт данных вытеснила обработчик чтения между его обменом и
// записью ячейки: в ячейке остаётся ответ последнего обмена, а не
// более ранний ответ вытесненного.
void hook_zc_write_ff() {
    zout(kZcData, 0xFF);
}

void test_zcontroller_latch_not_overwritten() {
    std::printf("test_sd_emu_zcontroller_latch_not_overwritten\n");
    fill_disk();
    devices::sd::sd_spi_emu_init();
    devices::zcontroller::zcontroller_init();
    CHECK(zinit());
    uint8_t r = 0xFF;
    CHECK(zcmd(17, 1200, &r));
    CHECK_EQ(r, 0x00);
    CHECK(ztoken());
    // В ячейке байт 0. Второе чтение отдаёт байт 1 и обменом берёт байт 2; до
    // записи ячейки вклинивается запись, её обмен берёт байт 3.
    CHECK_EQ(zin(kZcData), g_disk[1200][0]);
    g_port_set_hook_on = kZcData;
    g_port_set_hook    = hook_zc_write_ff;
    CHECK_EQ(zin(kZcData), g_disk[1200][1]);
    CHECK_EQ(g_port_set_hook, static_cast<void (*)()>(nullptr));
    CHECK_EQ(g_port_rd[kZcData], g_disk[1200][3]);
    zrelease();
}

// То же у DivMMC: своя копия защёлки на 0xEB, основной путь esxDOS.
void hook_divmmc_write_ff() {
    zout(devices::divmmc_card::kPortData, 0xFF);
}

void test_divmmc_latch_not_overwritten() {
    std::printf("test_sd_emu_divmmc_latch_not_overwritten\n");
    fill_disk();
    devices::sd::sd_spi_emu_init();
    devices::divmmc_card::divmmc_card_init(true, false);
    constexpr uint8_t kData = devices::divmmc_card::kPortData;
    g_host                  = HostPorts{kData, devices::divmmc_card::kPortCtrl, 0xFE, 0xFF};
    CHECK(zinit());
    uint8_t r = 0xFF;
    CHECK(zcmd(17, 1300, &r));
    CHECK_EQ(r, 0x00);
    CHECK(ztoken());
    CHECK_EQ(zin(kData), g_disk[1300][0]);
    g_port_set_hook_on = kData;
    g_port_set_hook    = hook_divmmc_write_ff;
    CHECK_EQ(zin(kData), g_disk[1300][1]);
    CHECK_EQ(g_port_set_hook, static_cast<void (*)()>(nullptr));
    CHECK_EQ(g_port_rd[kData], g_disk[1300][3]);
    zrelease();
    g_host = kZcHost;
}

// (е) Порт 0x57 конвейером через zcontroller.cpp: инициализация SDHC,
// одиночное чтение и окна CMD18 по 8 секторов - байт в байт с носителем;
// порт 0x77 после init - 0xFC при карте.
void test_zcontroller_ports() {
    std::printf("test_sd_emu_zcontroller_ports\n");
    fill_disk();
    // Эмулятор с чистого листа: прошлые проверки его уже поднимали, а
    // zcontroller_init поднятый не трогает.
    devices::sd::sd_spi_emu_init();
    devices::zcontroller::zcontroller_init();
    CHECK_EQ(g_port_rd[kZcCtrl], devices::zcontroller::kCtrlCardPresent);

    // Обе стороны признака носителя. Носитель виден через свой эмулятор,
    // поэтому "карты нет" изображается флешкой, которую тест выдёргивает:
    // карта в пустышке есть всегда.
    const auto zc = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Usb);
    devices::zcontroller::zcontroller_tick();
    CHECK_EQ(g_port_rd[kZcCtrl], devices::zcontroller::kCtrlNoCard);
    g_usb_present = true;
    devices::zcontroller::zcontroller_tick();
    CHECK_EQ(g_port_rd[kZcCtrl], devices::zcontroller::kCtrlCardPresent);
    g_usb_present = false;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Card);
    devices::zcontroller::zcontroller_tick();
    CHECK_EQ(g_port_rd[kZcCtrl], devices::zcontroller::kCtrlCardPresent);

    // Признак не переписывается, пока не изменился: иначе виток ядра шины
    // пишет в таблицу ответов на каждом проходе.
    const uint32_t writes = g_port_rd_writes[kZcCtrl];
    devices::zcontroller::zcontroller_tick();
    devices::zcontroller::zcontroller_tick();
    CHECK_EQ(g_port_rd_writes[kZcCtrl], writes);

    CHECK(zinit());
    uint8_t buf[512];
    CHECK(zread_sector(1000, buf));
    CHECK(std::memcmp(buf, g_disk[1000], 512) == 0);
    uint32_t bad = 0;
    for (uint32_t w = 0; w < 64; ++w)
        bad += zread_multi_bad(2000 + w * 8, 8);
    std::printf("  64 windows of 8 sectors, broken %u\n", bad);
    CHECK_EQ(bad, 0u);
    CHECK(zread_sector(3000, buf));
    CHECK(std::memcmp(buf, g_disk[3000], 512) == 0);

    // Запись через защёлку порта 0x57: ответ на данные приходит вторым IN
    // после CRC (первый отдаёт итог обмена CRC), занятость идёт холостыми
    // обменами чтения порта.
    const uint32_t writes0 = g_writes;
    uint8_t r              = 0xFF;
    CHECK(zcmd(24, 3100, &r));
    CHECK_EQ(r, 0x00);
    zout(kZcData, 0xFF);
    zout(kZcData, 0xFE);
    for (int i = 0; i < 512; ++i)
        zout(kZcData, static_cast<uint8_t>(0xA0 + (i & 0x0F)));
    zout(kZcData, 0xFF);
    zout(kZcData, 0xFF);
    const uint8_t first = zin(kZcData);
    const uint8_t resp  = zin(kZcData);
    std::printf("  write through the latch: after CRC 0x%02X, then 0x%02X\n", first, resp);
    CHECK_EQ(first, 0xFF);
    CHECK_EQ(resp & 0x1F, 0x05);
    int busy = 0;
    while (zin(kZcData) == 0x00 && busy < 100000)
        ++busy;
    zrelease();
    CHECK_EQ(g_writes, writes0 + 1u);
    CHECK_EQ(g_disk[3100][0], 0xA0u);
    CHECK_EQ(g_disk[3100][511], 0xAFu);
    CHECK(zread_sector(3100, buf));
    CHECK(std::memcmp(buf, g_disk[3100], 512) == 0);
}

// Две карты DivMMC на портах 0xE7/0xEB: каждая со своим носителем, выбор
// битами 0 и 1; выбраны обе - говорит первая; выключенная настройками
// карта для машины пустая шина.
void test_divmmc_two_cards() {
    std::printf("test_sd_emu_divmmc_two_cards\n");
    fill_disk();
    g_usb_present = true;
    for (uint32_t i = 0; i < 512; ++i)
        g_usb_disk[1000][i] = static_cast<uint8_t>(0xC0u ^ i);
    devices::storage::storage_set_emulator_medium(SdOwner::DivMmc2, devices::hal::Medium::Usb);
    devices::storage::storage_init();
    devices::sd::sd_spi_emu_init();
    devices::divmmc_card::divmmc_card_init(true, true);
    constexpr uint8_t kData = devices::divmmc_card::kPortData;
    constexpr uint8_t kCtrl = devices::divmmc_card::kPortCtrl;
    CHECK_EQ(g_port_rd[kData], 0xFFu);

    uint8_t buf[512];
    g_host = HostPorts{kData, kCtrl, 0xFE, 0xFF};
    CHECK(zinit());
    CHECK(zread_sector(1000, buf));
    CHECK(std::memcmp(buf, g_disk[1000], 512) == 0);

    g_host = HostPorts{kData, kCtrl, 0xFD, 0xFF};
    CHECK(zinit());
    CHECK(zread_sector(1000, buf));
    CHECK(std::memcmp(buf, g_usb_disk[1000], 512) == 0);

    g_host = HostPorts{kData, kCtrl, 0xFC, 0xFF};
    CHECK(zread_sector(1001, buf));
    CHECK(std::memcmp(buf, g_disk[1001], 512) == 0);

    // Вторая выключена: на её выборе машина видит 0xFF, первая работает.
    devices::sd::sd_spi_emu_init();
    devices::divmmc_card::divmmc_card_init(true, false);
    g_host = HostPorts{kData, kCtrl, 0xFD, 0xFF};
    CHECK(!zinit());
    CHECK_EQ(g_port_rd[kData], 0xFFu);
    g_host = HostPorts{kData, kCtrl, 0xFE, 0xFF};
    CHECK(zinit());
    CHECK(zread_sector(1002, buf));
    CHECK(std::memcmp(buf, g_disk[1002], 512) == 0);

    g_host        = kZcHost;
    g_usb_present = false;
}

// (е2) Повторный вход в один автомат: обработчик чтения вытеснен записью
// посреди такта. Изображается крюком в часах - эмулятор спрашивает их
// внутри разбора команды, и вложенный такт приходит ровно туда, куда на
// плате приходит вытеснение.
uint8_t g_nested_rx     = 0xFF;
uint32_t g_nested_calls = 0;

void hook_nested_byte() {
    ++g_nested_calls;
    g_nested_rx = devices::sd::sd_spi_byte(g_who, 0xFF);
}

void test_reentrant_step() {
    std::printf("test_sd_emu_reentrant_step\n");
    fresh_card();
    sel(true);
    g_nested_calls = 0;

    // Шестой байт CMD17 доводит команду до ответа, а разбор спрашивает
    // часы: там и вклинивается чужой такт.
    x(0xFF);
    uint8_t f[6] = {0x40 | 17, 0, 0, 0x02, 0xBC, 0};
    f[5]         = devices::sd::crc7(f, 5);
    for (int i = 0; i < 5; ++i)
        x(f[i]);
    g_mono_hook      = hook_nested_byte;
    const uint8_t r1 = x(f[5]);
    CHECK_EQ(g_nested_calls, 1u);
    CHECK_EQ(g_mono_hook, static_cast<void (*)()>(nullptr));

    // Такт вклинившегося ушёл в очередь, разобран сразу за своим и забрал
    // R1; вклинившемуся досталась защёлка - байт предыдущего обмена.
    // Фаза при этом цела: дальше идёт обычный токен и сектор, а не сдвиг.
    CHECK_EQ(r1, 0x00u);
    CHECK_EQ(g_nested_rx, 0xFFu);
    uint8_t t = 0xFF;
    for (int p = 0; p < 0x4000; ++p) {
        t = x(0xFF);
        if (t != 0xFF) break;
    }
    CHECK_EQ(t, 0xFE);
    uint8_t buf[512];
    for (int i = 0; i < 512; ++i)
        buf[i] = x(0xFF);
    x(0xFF);
    x(0xFF);
    CHECK(std::memcmp(buf, g_disk[700], 512) == 0);
    sel(false);
}

// (е2б) Смена носителя не рвёт идущий обмен. Поток её только готовит,
// принимает обработчик на своём такте: иначе хост, успевший опустить CS
// между проверкой и записью CSD, получил бы половину старого блока.
void hook_shrink_media() {
    g_card_sectors = 2048;
    devices::sd::sd_spi_emu_refresh_media();
}

void test_media_change_is_published() {
    std::printf("test_sd_emu_media_change_is_published\n");
    fresh_card();
    g_card_sectors = kDiskSectors;
    sel(true);
    // CMD9 отдаёт CSD; смена носителя приходит ровно на ответе команды.
    g_mono_hook = hook_shrink_media;
    // Ответа нет и блока нет: обмен оборван целиком, а не отдан
    // наполовину с несходящимся CRC.
    CHECK_EQ(cmd(9, 0), 0xFFu);
    CHECK_EQ(g_card_sectors, 2048u);
    uint8_t t = 0xFF;
    for (int p = 0; p < 64; ++p) {
        t = x(0xFF);
        if (t != 0xFF) break;
    }
    CHECK_EQ(t, 0xFFu);

    // Карта как только что вставленная: готовность снята, хост проходит
    // CMD0 и ACMD41 заново и получает уже новую ёмкость.
    sel(false);
    x(0xFF);
    CHECK(init_card());
    sel(true);
    CHECK_EQ(cmd(17, 3000), devices::sd::kR1ParamErr); // за концом сжавшейся карты
    sel(false);
    x(0xFF);
    g_card_sectors = kDiskSectors;
}

// (е3) Запись одной стороны не останавливает вторую. Заказ записи и
// признак её успеха свои у каждой: общий делал карту второй стороны
// занятой на всё время программирования - до 9.8 мс по замеру платы.
void test_write_does_not_block_other_side() {
    std::printf("test_sd_emu_write_does_not_block_other_side\n");
    fresh_card();
    // Пишет Z-Controller (он же единственный писатель в этом прогоне);
    // цикл не пускаем, заказ записи остаётся висеть.
    g_task_enabled = false;
    CHECK_EQ(send_block(400, 0x11), 0x05u);
    CHECK_EQ(x(0xFF), 0x00u); // своя сторона занята, как настоящая карта
    sel(false);

    // DivMMC в это же время обязан принять команду и отдать сектор.
    g_who = SdOwner::DivMmc;
    CHECK(init_card());
    sel(true);
    CHECK_EQ(cmd(17, 800), 0x00u);
    g_task_enabled = true;
    uint8_t t      = 0xFF;
    for (int p = 0; p < 0x4000; ++p) {
        t = x(0xFF);
        if (t != 0xFF) break;
        devices::sd::sd_spi_task();
    }
    CHECK_EQ(t, 0xFE);
    uint8_t buf[512];
    for (int i = 0; i < 512; ++i)
        buf[i] = x(0xFF);
    x(0xFF);
    x(0xFF);
    CHECK(std::memcmp(buf, g_disk[800], 512) == 0);
    sel(false);
    x(0xFF);
    CHECK(sector_filled(400, 0x11));
}

// (е4) Слот, погашенный чужой записью, не съедает заказ: сторона ждёт
// токен на тот же сектор и обязана его дождаться, а не висеть до
// таймаута хоста.
void test_write_reposts_waiting_read() {
    std::printf("test_sd_emu_write_reposts_waiting_read\n");
    fresh_card();
    // Ждёт токен DivMMC, пишет Z-Controller - единственный писатель.
    g_who = SdOwner::DivMmc;
    CHECK(init_card());
    sel(true);
    CHECK_EQ(cmd(17, 900), 0x00u);
    // Сектор приезжает, но хост токен ещё не забрал.
    devices::sd::sd_spi_task();
    g_task_enabled = false;

    // Вторая сторона пишет тот же сектор: слот гаснет под ждущим чтением.
    const SdOwner keep = g_who;
    g_who              = SdOwner::ZController;
    sel(true);
    CHECK_EQ(cmd(24, 900), 0x00u);
    x(0xFF);
    x(0xFE);
    for (int i = 0; i < 512; ++i)
        x(0x77);
    x(0x00);
    x(0x00);
    x(0xFF);
    sel(false);
    g_who          = keep;
    g_task_enabled = true;
    devices::sd::sd_spi_task(); // запись легла, слот ждущего погашен

    uint8_t t = 0xFF;
    for (int p = 0; p < 0x4000; ++p) {
        t = x(0xFF);
        if (t != 0xFF) break;
        devices::sd::sd_spi_task();
    }
    CHECK_EQ(t, 0xFE);
    uint8_t buf[512];
    for (int i = 0; i < 512; ++i)
        buf[i] = x(0xFF);
    x(0xFF);
    x(0xFF);
    // Отдан уже записанный сектор: заказ переставлен, а не отдан старый.
    CHECK_EQ(buf[0], 0x77u);
    sel(false);
    x(0xFF);
}

// (ж) Носитель стороны: чтение и запись одного хозяина идут на один и тот
// же носитель, даже когда у второй стороны он другой и флешка на месте.
void test_medium_follows_owner() {
    std::printf("test_sd_emu_medium_follows_owner\n");
    fill_disk();
    g_usb_present = true;
    const auto dm = SdOwner::DivMmc;
    const auto zc = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(dm, devices::hal::Medium::Card);
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Usb);

    uint8_t blk[512];
    std::memset(blk, 0xA5, sizeof blk);
    CHECK(devices::storage::storage_write(dm, 100, blk));
    CHECK_EQ(g_disk[100][0], 0xA5u);
    CHECK_EQ(g_usb_disk[100][0], 0x00u);

    std::memset(blk, 0x5A, sizeof blk);
    CHECK(devices::storage::storage_write(zc, 101, blk));
    CHECK_EQ(g_usb_disk[101][0], 0x5Au);
    CHECK(g_disk[101][0] != 0x5A);

    // Записанное перечитывается тем же хозяином и тем же значением.
    uint8_t out[512];
    CHECK(devices::storage::storage_emulator_read(dm, 100, out));
    CHECK_EQ(out[0], 0xA5u);
    CHECK(devices::storage::storage_emulator_read(zc, 101, out));
    CHECK_EQ(out[0], 0x5Au);

    // Сектор с одним номером на двух носителях - разные данные: кэш не
    // обязан их путать.
    CHECK(devices::storage::storage_emulator_read(zc, 100, out));
    CHECK(out[0] != 0xA5);
    g_usb_present = false;
}

// --- Драйвер карты (sd_card_spi.cpp) против эмулятора ---
//
// На том конце SPI - эмулятор, владелец Z-Controller (он же единственный
// писатель эмулятора в этом прогоне). Часы идут со скоростью линии:
// 20 мкс на байт при 400 кГц, 0.32 мкс при 25 МГц.

bool g_card_silent        = false; // карты нет: MISO всегда 0xFF
uint32_t g_corrupt_blocks = 0;     // столько следующих блоков приходят с битым битом
bool g_stuck_idle         = false; // карта в idle: CMD17, ACMD41 и CMD1 - R1 0x01
bool g_r1_override        = false;
int g_data_resp_fake      = -1;    // ответ на данные записи вместо эмулятора
bool g_busy_stuck         = false; // после ответа на данные MISO в нуле до снятия CS
bool g_busy_now           = false;
uint8_t g_prev_mosi       = 0xFF;
uint32_t g_line_ns        = 0;

// Карта старше SDHC поверх эмулятора, который отвечает как SDHC. Кадры
// команд собираются на линии и переписываются перед подачей эмулятору.
// SDSC v1: CMD8 - "нет такой команды", CCS в OCR снят, адрес CMD17/18/24/25
// в байтах (эмулятору - в секторах). MMC (ACMD41 - "нет такой команды")
// здесь не проверить: эмулятор не знает CMD1.
bool g_card_sdsc        = false;
bool g_cmd10_fails      = false; // CMD10 (CID) - "нет такой команды"
int g_fail_cmd          = -1;    // эта команда - "нет такой команды", до эмулятора не доходит
bool g_cmd8_bad_echo    = false; // CMD8 возвращает чужой проверочный узор
uint8_t g_frame[6]      = {};
int g_frame_n           = -1;
int g_fake_r1           = -1; // ответ вместо эмулятора на следующий опрос
int g_ocr_stage         = 0;  // 1 - ждём R1 CMD58, 2 - следующий байт OCR[0]
uint32_t g_seen_arg[64] = {}; // последний аргумент драйвера по номеру команды
int g_cmd6_fn           = -1; // CMD6 отвечает здесь: функция группы 1 в блоке состояния
uint8_t g_stream[80]    = {};
int g_stream_n          = 0;
int g_stream_pos        = 0;

uint8_t old_card_byte(uint8_t out, bool* handled) {
    *handled = true;
    if (g_frame_n < 0 && g_prev_mosi == 0xFF && (out & 0xC0) == 0x40) g_frame_n = 0;
    if (g_frame_n >= 0) {
        g_prev_mosi          = out;
        g_frame[g_frame_n++] = out;
        if (g_frame_n < 6) return 0xFF;
        g_frame_n         = -1;
        const uint8_t cmd = g_frame[0] & 0x3F;
        uint32_t arg      = (uint32_t{g_frame[1]} << 24) | (uint32_t{g_frame[2]} << 16) | (uint32_t{g_frame[3]} << 8) | g_frame[4];
        g_seen_arg[cmd]   = arg;
        if (cmd == 6 && g_cmd6_fn >= 0) {
            // R1, токен, 64 байта состояния, CRC16.
            uint8_t* st = g_stream + 2;
            std::memset(g_stream, 0, sizeof(g_stream));
            g_stream[1]      = 0xFE;
            st[16]           = static_cast<uint8_t>(g_cmd6_fn);
            const uint16_t c = devices::sd::crc16(st, 64);
            g_stream[66]     = static_cast<uint8_t>(c >> 8);
            g_stream[67]     = static_cast<uint8_t>(c);
            g_stream_n       = 68;
            g_stream_pos     = 0;
            return 0xFF;
        }
        if ((g_cmd10_fails && cmd == 10) || cmd == g_fail_cmd) {
            g_fake_r1 = 0x05;
            return 0xFF;
        }
        if (g_card_sdsc && cmd == 8) {
            g_fake_r1 = 0x05;
            return 0xFF;
        }
        if (g_card_sdsc && (cmd == 17 || cmd == 18 || cmd == 24 || cmd == 25)) arg /= 512u;
        if (g_card_sdsc && cmd == 58) g_ocr_stage = 1;
        if (g_cmd8_bad_echo && cmd == 8) arg ^= 0x55u;
        uint8_t f[6] = {g_frame[0], static_cast<uint8_t>(arg >> 24), static_cast<uint8_t>(arg >> 16), static_cast<uint8_t>(arg >> 8), static_cast<uint8_t>(arg),
                        0};
        f[5]         = devices::sd::crc7(f, 5);
        uint8_t in   = 0xFF;
        for (int i = 0; i < 6; ++i) {
            in = devices::sd::sd_spi_byte(SdOwner::ZController, f[i]);
            devices::sd::sd_spi_task();
        }
        return in;
    }
    if (g_stream_pos < g_stream_n) {
        g_prev_mosi = out;
        return g_stream[g_stream_pos++];
    }
    if (g_fake_r1 >= 0) {
        g_prev_mosi     = out;
        const uint8_t v = static_cast<uint8_t>(g_fake_r1);
        g_fake_r1       = -1;
        return v;
    }
    *handled = false;
    return 0;
}

uint8_t old_card_fix(uint8_t in) {
    if (g_ocr_stage == 2) {
        in          = static_cast<uint8_t>(in & ~0x40u);
        g_ocr_stage = 0;
    } else if (g_ocr_stage == 1 && (in & 0x80) == 0) {
        g_ocr_stage = 2;
    }
    return in;
}

uint8_t line_byte_raw(uint8_t out) {
    g_line_ns      += static_cast<uint32_t>(8000000000ull / g_test_spi_hz);
    g_test_time_us += g_line_ns / 1000u;
    g_line_ns      %= 1000u;
    if (g_card_silent) return 0xFF;
    if (g_busy_now) return 0x00;
    if (g_card_sdsc || g_cmd10_fails || g_fail_cmd >= 0 || g_cmd8_bad_echo || g_cmd6_fn >= 0) {
        bool handled    = false;
        const uint8_t v = old_card_byte(out, &handled);
        if (handled) return v;
        g_prev_mosi      = out;
        const uint8_t in = devices::sd::sd_spi_byte(SdOwner::ZController, out);
        devices::sd::sd_spi_task();
        return old_card_fix(in);
    }
    if (g_stuck_idle && g_prev_mosi == 0xFF && (out == (0x40 | 17) || out == (0x40 | 41) || out == (0x40 | 1))) {
        g_r1_override = true;
    }
    g_prev_mosi = out;
    uint8_t in  = devices::sd::sd_spi_byte(SdOwner::ZController, out);
    devices::sd::sd_spi_task();
    if (g_r1_override && (in & 0x80) == 0) {
        g_r1_override = false;
        in            = 0x01;
    }
    // Ответ на данные записи: младшие пять бит 0x05, у R1 таких в этом пути нет.
    if (in != 0xFF && (in & 0x1F) == 0x05) {
        if (g_data_resp_fake >= 0) {
            in               = static_cast<uint8_t>(g_data_resp_fake);
            g_data_resp_fake = -1;
        } else if (g_busy_stuck) {
            g_busy_now = true;
        }
    }
    return in;
}

uint8_t line_byte(uint8_t out) {
    const uint8_t in = line_byte_raw(out);
    wire(out);
    wire(in);
    return in;
}

unsigned long log_num(const char* key) {
    const char* p = std::strstr(g_test_log, key);
    return p ? std::strtoul(p + std::strlen(key), nullptr, 10) : 0xFFFFFFFFul;
}

// Счётчики драйвера - из его же строки "sd: носитель".
struct Health {
    unsigned long reinit, reinit_failed, r1, token, crc, crc_retry, slow, rejects;
};

Health health() {
    g_test_log[0] = 0;
    devices::sd::sd_card_log_health();
    Health h;
    h.reinit        = log_num("reinits ");
    h.reinit_failed = log_num("failed ");
    h.r1            = log_num("r1 errors ");
    h.token         = log_num("token ");
    h.crc           = log_num(", crc ");
    h.crc_retry     = log_num("bad crc ");
    h.slow          = log_num("over 100 ms ");
    h.rejects       = log_num("rejected by reinit window ");
    return h;
}

bool card_read_ok(uint32_t lba) {
    uint8_t buf[512];
    return devices::sd::sd_card_read_sector(lba, buf) && std::memcmp(buf, g_disk[lba], 512) == 0;
}

// Обслуживание хоста, пока драйвер ждёт карту. Заодно лезет к карте сам:
// такое вложенное чтение обязано отказать, иначе его команда ушла бы
// посреди чужой при опущенном CS.
uint32_t g_service_calls     = 0;
uint32_t g_service_nested_ok = 0;
void count_service(void*) {
    ++g_service_calls;
    uint8_t buf[512];
    if (devices::sd::sd_card_read_sector(1, buf)) ++g_service_nested_ok;
}

uint32_t timed_read(uint32_t lba, bool* ok) {
    const uint32_t t0 = g_test_time_us;
    *ok               = card_read_ok(lba);
    return g_test_time_us - t0;
}

// (ж) Инициализация, чтение и запись; битый бит на линии; сектор без токена
// и окно переинициализации; карта, застрявшая в idle.
void test_card_driver() {
    std::printf("test_sd_card_driver_vs_emu\n");
    fill_disk();
    devices::sd::sd_spi_emu_init();

    // Карты нет: CMD0 без idle, причина в логе. Печатает не рукопожатие, а
    // отдельная функция: на стеке Core1 буфер строки ей не по карману.
    g_card_silent = true;
    g_test_log[0] = 0;
    CHECK(!devices::sd::sd_card_init());
    CHECK(g_test_log[0] == 0);
    devices::sd::sd_card_log_handshake();
    CHECK(std::strstr(g_test_log, "no card") != nullptr);
    g_card_silent = false;

    // SDHC, число секторов - круг build_csd -> sectors_from_csd.
    CHECK(devices::sd::sd_card_init());
    CHECK(devices::sd::sd_card_info().kind == devices::sd::SdKind::Sdhc);
    CHECK_EQ(devices::sd::sd_card_info().sector_count, kDiskSectors);
    // CID прочитан: им драйвер отличает другую карту той же ёмкости.
    const uint32_t serial = devices::sd::sd_card_info().serial;
    CHECK(serial != 0u);
    // Повторный подъём, CMD10 не отвечает: номер прежний, иначе машине
    // объявилась бы смена носителя.
    g_cmd10_fails = true;
    CHECK(devices::sd::sd_card_init());
    g_cmd10_fails = false;
    CHECK_EQ(devices::sd::sd_card_info().serial, serial);
    uint32_t bad = 0;
    for (uint32_t lba = 0; lba < 64; ++lba)
        bad += card_read_ok(lba) ? 0u : 1u;
    CHECK_EQ(bad, 0u);
    uint8_t w[512];
    for (uint32_t i = 0; i < 512; ++i)
        w[i] = static_cast<uint8_t>(i ^ 0x5A);
    CHECK(devices::sd::sd_card_write_sector(77, w));
    CHECK(std::memcmp(g_disk[77], w, 512) == 0);
    CHECK(card_read_ok(77));

    // Пачка CMD18 - ей на плате читаются все блоки. Данные те же, что на
    // носителе; битый блок бросает пачку, а карта после CMD12 отвечает.
    {
        static uint8_t run[16 * 512];
        CHECK(devices::sd::sd_card_read_run(200, 16, run));
        CHECK(std::memcmp(run, g_disk[200], sizeof(run)) == 0);
        const Health r0  = health();
        g_corrupt_blocks = 1;
        CHECK(!devices::sd::sd_card_read_run(300, 16, run));
        g_corrupt_blocks = 0;
        CHECK_EQ(health().crc - r0.crc, 1ul);
        CHECK(card_read_ok(320));
        CHECK(devices::sd::sd_card_read_run(400, 16, run));
        CHECK(std::memcmp(run, g_disk[400], sizeof(run)) == 0);
    }

    // Битый бит на линии: один блок - повтор снимает, три подряд - отказ.
    const Health h0  = health();
    g_corrupt_blocks = 1;
    CHECK(card_read_ok(5));
    g_corrupt_blocks = 3;
    CHECK(!card_read_ok(6));
    const Health h1 = health();
    CHECK_EQ(h1.crc - h0.crc, 1ul);
    CHECK_EQ(h1.crc_retry - h0.crc_retry, 4ul);

    // Карта приняла CMD17 и не прислала токен: токен 200 мс, переинициализация,
    // ещё 200 мс. Повтор сразу - 200 мс и отказ окна; исправный сектор и
    // запись следом - отказ окна без обращения к карте.
    bool ok            = true;
    g_fail_read_lba    = 90;
    const uint32_t dt1 = timed_read(90, &ok);
    CHECK(!ok);
    const uint32_t dt2 = timed_read(90, &ok);
    CHECK(!ok);
    g_fail_read_lba    = 0xFFFFFFFFu;
    const uint32_t dt3 = timed_read(91, &ok);
    CHECK(!ok);
    CHECK(!devices::sd::sd_card_write_sector(92, w));
    std::printf("  sector without a token %u us, retry %u us, a good one after it %u us\n", dt1, dt2, dt3);
    CHECK(dt1 >= 400000u && dt1 < 450000u);
    CHECK(dt2 >= 200000u && dt2 < 250000u);
    CHECK_EQ(dt3, 0u);
    const Health h2 = health();
    CHECK_EQ(h2.token - h1.token, 3ul);
    CHECK_EQ(h2.reinit - h1.reinit, 1ul);
    CHECK_EQ(h2.rejects - h1.rejects, 3ul);
    CHECK_EQ(h2.slow - h1.slow, 2ul);

    // Через секунду - переинициализация, чтение верно.
    g_test_time_us += 1000000u;
    CHECK(card_read_ok(91));
    const Health h3 = health();
    CHECK_EQ(h3.reinit - h2.reinit, 1ul);
    CHECK_EQ(h3.reinit_failed, h2.reinit_failed);

    // Карта застряла в idle: попытка около 4 с (ACMD41 дважды по 1.5 с, CMD1
    // 1 с), интервал от конца попытки удваивается. Цикл Core1 - чтение раз в
    // 1 мс, 60 с поддельного времени.
    g_test_time_us       += 1100000u;
    g_stuck_idle          = true;
    const uint32_t start  = g_test_time_us;
    uint32_t in_init = 0, attempts = 0, last_end = 0;
    uint32_t gaps_ms[8] = {0};
    bool attempts_4s    = true;
    while (g_test_time_us - start < 60000000u) {
        const uint32_t t0 = g_test_time_us;
        const uint32_t dt = timed_read(10, &ok);
        CHECK(!ok);
        if (dt > 1000000u) {
            if (attempts > 0 && attempts <= 8) gaps_ms[attempts - 1] = (t0 - last_end + 500u) / 1000u;
            if (dt < 4000000u || dt > 4300000u) attempts_4s = false;
            ++attempts;
            in_init  += dt;
            last_end  = g_test_time_us;
        }
        g_test_time_us += 1000u;
    }
    g_stuck_idle = false;
    std::printf("  stuck card: attempts %u in 60 s, %u%% of it in init, gaps %u %u %u %u ms\n", attempts, in_init / 600000u, gaps_ms[0], gaps_ms[1], gaps_ms[2],
                gaps_ms[3]);
    CHECK(attempts_4s);
    CHECK_EQ(attempts, 5u);
    CHECK_EQ(gaps_ms[0], 2000u);
    CHECK_EQ(gaps_ms[1], 4000u);
    CHECK_EQ(gaps_ms[2], 8000u);
    CHECK_EQ(gaps_ms[3], 16000u);

    // Карта ожила: после интервала - переинициализация, чтение верно.
    g_test_time_us += 33000000u;
    CHECK(card_read_ok(12));

    // Ожидание обслуживается: сектор без токена - 200 мс токена,
    // переинициализация и ещё 200 мс, всё это время хост должен получать
    // своё. Опрос токена идёт байтами по 0.32 мкс, поэтому число вызовов
    // задаёт не цикл, а порог в миллисекунду.
    devices::sd::sd_card_set_wait_service(&count_service, nullptr);
    g_service_calls     = 0;
    g_service_nested_ok = 0;
    // Кэши арбитра - долой: сектор 93 мог остаться в блоке от прежних
    // чтений, и тогда отказ носителя до эмулятора просто не дойдёт.
    devices::storage::storage_init();
    g_fail_read_lba       = 93;
    const uint32_t svc_t0 = g_test_time_us;
    CHECK(!card_read_ok(93));
    const uint32_t svc_us = g_test_time_us - svc_t0;
    g_fail_read_lba       = 0xFFFFFFFFu;
    devices::sd::sd_card_set_wait_service(nullptr, nullptr);
    std::printf("  service while waiting: %u calls in %u us, nested reads that went through %u\n", g_service_calls, svc_us, g_service_nested_ok);
    CHECK_EQ(g_service_nested_ok, 0ul);
    // Не чаще срока обслуживания (запас - на каждый отдельный цикл
    // ожидания, у них свой отсчёт) и не реже: ожидание не должно молчать.
    CHECK(g_service_calls <= svc_us / devices::sd::kCardServicePeriodUs + 8u);
    CHECK(g_service_calls >= svc_us / (devices::sd::kCardServicePeriodUs * 2u));
}

// Строка "sd: sector write": отказы по веткам и худшая длительность.
struct WriteStats {
    unsigned long r1, data, busy, max_us;
};

unsigned long num_in(const char* text, const char* key) {
    const char* p = std::strstr(text, key);
    return p ? std::strtoul(p + std::strlen(key), nullptr, 10) : 0xFFFFFFFFul;
}

WriteStats write_stats() {
    g_test_log[0] = 0;
    devices::sd::sd_card_log_health();
    const char* w = std::strstr(g_test_log, "sector write:");
    WriteStats s{0xFFFFFFFFul, 0xFFFFFFFFul, 0xFFFFFFFFul, 0xFFFFFFFFul};
    if (w == nullptr) return s;
    s.r1     = num_in(w, "write r1 ");
    s.data   = num_in(w, ", data ");
    s.busy   = num_in(w, ", busy ");
    s.max_us = num_in(w, "max ");
    return s;
}

// Отказы записи на карту, все три ветки: CMD24 без R1 0, ответ на данные
// не "принято", занятость дольше 500 мс. Каждая - false и свой счётчик,
// следующее чтение проходит.
void test_card_write_failures() {
    std::printf("test_sd_card_write_failures\n");
    g_test_time_us += 120000000u; // окно переинициализации прошлых тестов - позади
    fill_disk();
    devices::sd::sd_spi_emu_init();
    CHECK(devices::sd::sd_card_init());
    uint8_t w[512];
    for (uint32_t i = 0; i < 512; ++i)
        w[i] = static_cast<uint8_t>(i ^ 0xA5);
    const WriteStats s0 = write_stats();

    g_fail_cmd = 24;
    CHECK(!devices::sd::sd_card_write_sector(150, w));
    g_fail_cmd          = -1;
    const WriteStats s1 = write_stats();
    CHECK_EQ(s1.r1 - s0.r1, 1ul);
    CHECK(card_read_ok(151));

    g_data_resp_fake = 0x0B; // CRC не сошёлся
    CHECK(!devices::sd::sd_card_write_sector(152, w));
    CHECK_EQ(g_data_resp_fake, -1);
    const WriteStats s2 = write_stats();
    CHECK_EQ(s2.data - s1.data, 1ul);
    CHECK(card_read_ok(153));

    g_busy_stuck = true;
    CHECK(!devices::sd::sd_card_write_sector(154, w));
    g_busy_stuck        = false;
    const WriteStats s3 = write_stats();
    std::printf("  write failures r1 %lu, data %lu, busy %lu, max %lu us\n", s3.r1 - s0.r1, s3.data - s0.data, s3.busy - s0.busy, s3.max_us);
    CHECK_EQ(s3.busy - s2.busy, 1ul);
    CHECK(s3.max_us >= 500000ul && s3.max_us < 510000ul);
    CHECK(card_read_ok(155));
    CHECK_EQ(s3.r1 - s0.r1, 1ul);
    CHECK_EQ(s3.data - s0.data, 1ul);
}

// Частота выше 25 МГц - через High Speed (CMD6). Карта приняла функцию 1
// группы 1 - частота по запросу, не умеет (0x0F) - 25 МГц. Подъём карты
// заново снимает High Speed, и CMD6 уходит снова.
void test_card_high_speed() {
    std::printf("test_sd_card_high_speed\n");
    g_test_time_us += 120000000u;
    fill_disk();
    devices::sd::sd_spi_emu_init();
    CHECK(devices::sd::sd_card_init());

    g_cmd6_fn     = 0x0F;
    g_seen_arg[6] = 0;
    CHECK_EQ(devices::sd::sd_card_set_speed(37500000u), 25000000u);
    CHECK_EQ(g_seen_arg[6], 0x80FFFFF1u);
    CHECK_EQ(g_test_spi_hz, 25000000u);

    g_cmd6_fn = 0x01;
    CHECK_EQ(devices::sd::sd_card_set_speed(37500000u), 37500000u);
    CHECK_EQ(g_test_spi_hz, 37500000u);
    CHECK(card_read_ok(400));

    // Сбой чтения: переинициализация на 400 кГц, затем снова CMD6.
    g_seen_arg[6]   = 0;
    g_fail_read_lba = 1500;
    CHECK(!card_read_ok(1500));
    g_fail_read_lba = 0xFFFFFFFFu;
    std::printf("  after reinit: CMD6 arg %08X, SCK %u Hz\n", g_seen_arg[6], g_test_spi_hz);
    CHECK_EQ(g_seen_arg[6], 0x80FFFFF1u);
    CHECK_EQ(g_test_spi_hz, 37500000u);
    g_test_time_us += 2000000u;
    CHECK(card_read_ok(402));

    g_cmd6_fn = -1;
    CHECK_EQ(devices::sd::sd_card_set_speed(25000000u), 25000000u);
}

// Тест частоты карты из TOOLS целиком, на модели карты. В первой строке
// (12.5 МГц) одно чтение не проходит: строка с ошибкой, а эталон данных
// берётся с первой чистой строки - старшие частоты "NO ERRORS", а не
// "DATA DIFFERS". 37.5 МГц без High Speed - "NO HIGH SPEED".
void test_card_speed_tool() {
    std::printf("test_sd_card_speed_tool\n");
    g_test_time_us += 120000000u;
    fill_disk();
    devices::sd::sd_spi_emu_init();
    CHECK(devices::sd::sd_card_init());
    using namespace soundsinth::config;
    static uint8_t page[kConfigPageBytes];
    tool_area_reset(page);
    ToolScreen scr(page);
    ToolArea& a = scr.area();
    static uint8_t pool[16384];
    soundsinth::memory::Arena arena;
    arena.base     = pool;
    arena.capacity = sizeof(pool);
    arena.floor    = sizeof(pool);
    soundsinth::memory::TrackScratch scratch{&arena};
    a.tool                        = static_cast<uint8_t>(ToolId::CardSpeed);
    a.keys[a.req % kToolKeySlots] = kToolKeyOpen;
    a.req                         = static_cast<uint8_t>(a.req + 1u);
    player::config::tools_serve(page, scratch);
    g_corrupt_blocks = 3; // три битых блока подряд - отказ одного чтения
    uint32_t turns   = 0;
    while (scr.state() == ToolState::Busy && turns < 100000u) {
        player::config::tools_serve(page, scratch);
        ++turns;
    }
    g_corrupt_blocks = 0;
    auto row         = [&](uint32_t i) { return std::string(a.text[1u + i] + 1, kToolTextCols); };
    for (uint32_t i = 0; i < 5; ++i)
        std::printf("  %s\n", row(i).c_str());
    CHECK(scr.state() == ToolState::Ready);
    CHECK(row(0).find("ERRORS") != std::string::npos && row(0).find("NO ERRORS") == std::string::npos);
    for (uint32_t i = 1; i < 4; ++i)
        CHECK(row(i).find("NO ERRORS") != std::string::npos);
    CHECK(row(4).find("NO HIGH SPEED") != std::string::npos);
    CHECK_EQ(g_test_spi_hz, soundsinth::config::kSdSpeedHz[player::config::settings().sd_speed]);
}

} // namespace

// Шина SPI теста (devices/hal/spi.h): на том конце эмулятор карты, байты
// гоняет line_byte. Частота хранится, чтобы часы шли со скоростью линии.
uint32_t devices::hal::spi_open(uint32_t hz) {
    return spi_set_hz(hz);
}

uint32_t devices::hal::spi_set_hz(uint32_t hz) {
    g_test_spi_hz = hz;
    return hz;
}

void devices::hal::spi_select(bool on) {
    wire(on ? 0x5E : 0x5D);
    if (!on) g_busy_now = false;
    devices::sd::sd_spi_select(SdOwner::ZController, on);
}

uint8_t devices::hal::spi_xfer(uint8_t out) {
    return line_byte(out);
}

void devices::hal::spi_read(uint8_t idle, uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = line_byte(idle);
    if (n == 512 && g_corrupt_blocks > 0) {
        --g_corrupt_blocks;
        dst[100] ^= 0x10;
    }
}

void devices::hal::spi_write(const uint8_t* src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        line_byte(src[i]);
}

// Ёмкость по CSD таблицей. Эмулятор платы всегда SDHC, поэтому ветка v1
// (SDSC) настоящими обменами не проверяется ни разу, а ошибка в ней читает
// не то место на настоящей карте SDSC - то есть отдаёт чужие данные под
// видом своих, а не отказывает.
// Карта старше SDHC: SDSC v1, адрес в байтах.
// Ошибка в ветке SDSC отдаёт чужие данные под видом своих, а не отказывает.
void test_card_old_kinds() {
    std::printf("test_sd_card_sdsc\n");
    // Окно переинициализации прошлого теста - позади.
    g_test_time_us += 120000000u;
    fill_disk();
    devices::sd::sd_spi_emu_init();
    g_card_sdsc = true;
    CHECK(devices::sd::sd_card_init());
    CHECK(devices::sd::sd_card_info().kind == devices::sd::SdKind::Sdsc);
    CHECK_EQ(g_seen_arg[16], 512u);
    CHECK(card_read_ok(3));
    CHECK_EQ(g_seen_arg[17], 3u * 512u);
    static uint8_t run[16 * 512];
    CHECK(devices::sd::sd_card_read_run(200, 16, run));
    CHECK_EQ(g_seen_arg[18], 200u * 512u);
    CHECK(std::memcmp(run, g_disk[200], sizeof(run)) == 0);
    uint8_t w[512];
    std::memset(w, 0x6B, sizeof(w));
    CHECK(devices::sd::sd_card_write_sector(50, w));
    CHECK_EQ(g_seen_arg[24], 50u * 512u);
    CHECK(std::memcmp(g_disk[50], w, 512) == 0);
    g_card_sdsc = false;
}

void test_sectors_from_csd() {
    std::printf("test_sd_sectors_from_csd\n");
    uint8_t csd[16] = {};

    // CSD v2, c_size = 0x00F3BF -> (62399+1) * 1024 = 63897600 секторов
    // (32 ГБ, обычная SDHC-карта).
    csd[0] = 0x40; // structure = 1
    csd[7] = 0x00;
    csd[8] = 0xF3;
    csd[9] = 0xBF;
    CHECK_EQ(devices::sd::sectors_from_csd(csd), 63897600u);

    // CSD v1: read_bl_len = 9 (512 Б), c_size = 3751, c_size_mult = 5.
    // (3751+1) << 7 = 480256 секторов, около 234 МБ.
    std::memset(csd, 0, sizeof(csd));
    csd[0]  = 0x00; // structure = 0
    csd[5]  = 0x09; // read_bl_len
    csd[6]  = static_cast<uint8_t>((3751u >> 10) & 0x03u);
    csd[7]  = static_cast<uint8_t>((3751u >> 2) & 0xffu);
    csd[8]  = static_cast<uint8_t>((3751u & 0x03u) << 6);
    csd[9]  = static_cast<uint8_t>((5u >> 1) & 0x03u);
    csd[10] = static_cast<uint8_t>((5u & 1u) << 7);
    CHECK_EQ(devices::sd::sectors_from_csd(csd), 480256u);

    // read_bl_len = 10 (1024 Б) - те же поля дают вдвое больше секторов.
    csd[5] = 0x0A;
    CHECK_EQ(devices::sd::sectors_from_csd(csd), 960512u);

    // read_bl_len вне 9..11 - поля несогласованы, размер не определить.
    csd[5] = 0x08;
    CHECK_EQ(devices::sd::sectors_from_csd(csd), 0u);
    csd[5] = 0x0C;
    CHECK_EQ(devices::sd::sectors_from_csd(csd), 0u);

    // Неизвестная версия CSD - ноль, а не догадка.
    csd[5] = 0x09;
    csd[0] = 0x80; // structure = 2
    CHECK_EQ(devices::sd::sectors_from_csd(csd), 0u);
    csd[0] = 0xC0; // structure = 3
    CHECK_EQ(devices::sd::sectors_from_csd(csd), 0u);
}

// Края носителя. Тест читал только середину диска, и три ветки автомата не
// исполнялись ни разу: отказ по номеру за ёмкостью, отсутствие упреждения
// за последним сектором и потолок номера в заказе.
void test_card_edges() {
    std::printf("test_sd_emu_card_edges\n");
    fresh_card();

    // Последний сектор читается как любой другой.
    const ReadRes last = read_block(kDiskSectors - 1u);
    CHECK(last.ok);
    CHECK(last.crc_ok);
    CHECK(last.data_ok);

    // Упреждения за ним быть не должно: сектора kDiskSectors не существует,
    // и обращаться к носителю незачем. Витки цикла даём после чтения -
    // упреждение берётся именно в свободное время.
    const uint32_t reads_before = g_reads;
    for (uint32_t i = 0; i < 8; ++i)
        devices::sd::sd_spi_task();
    CHECK_EQ(g_reads, reads_before);

    // Номер за ёмкостью - R1 с признаком неверного параметра и ни одного
    // такта данных.
    sel(true);
    CHECK_EQ(cmd(17, kDiskSectors), 0x40u);
    CHECK_EQ(x(0xFF), 0xFFu);
    CHECK_EQ(x(0xFF), 0xFFu);
    sel(false);
    x(0xFF);

    // Автомат после отказа жив: следующее обычное чтение проходит.
    CHECK(read_matches("after the failure by number", 500));

    // Тёплый блок у конца носителя: чтение со второй его половины не
    // заказывает блок за концом. Флешка с ёмкостью не кратной блоку - хвост
    // короче блока читается посекторно, без пачки за конец.
    const uint32_t past0 = devices::hal::g_reads_past_end;
    CHECK(read_matches("block before the end", kDiskSectors - 16u));
    CHECK(read_matches("second half of the last block", kDiskSectors - 8u));
    CHECK(read_matches("last sector", kDiskSectors - 1u));
    for (uint32_t i = 0; i < 8; ++i)
        devices::sd::sd_spi_task();
    CHECK_EQ(devices::hal::g_reads_past_end, past0);

    // Потолок номера в заказе: 28 разрядов, 128 ГБ. Сектор за ним на карте
    // есть, но заказ его не вместит - неверный параметр, а не ожидание
    // токена, и ни одного обращения к носителю.
    g_card_sectors = 0x10000010u;
    devices::sd::sd_spi_emu_refresh_media();
    CHECK(init_card());
    const uint32_t reads_ceiling = g_reads;
    sel(true);
    CHECK_EQ(cmd(17, 0x10000000u), 0x40u);
    CHECK_EQ(cmd(24, 0x10000000u), 0x40u);
    sel(false);
    x(0xFF);
    CHECK_EQ(g_reads, reads_ceiling);
    g_card_sectors = kDiskSectors;
    devices::sd::sd_spi_emu_refresh_media();
}

// --- Сверка побайтно с прежним кодом ---
//
// Сценарии проходят все фазы автомата, драйвера и кэшей; каждый байт обмена
// сворачивается в FNV-1a. Числа сняты с кода до упрощения: правка, которая
// меняет хоть один байт ответа или запроса, меняет и свёртку. Отметки
// времени в след и в свёртку не идут - они зависят от прошлых тестов.

uint8_t wait_token() {
    uint8_t t = 0xFF;
    for (int p = 0; p < 0x4000 && t == 0xFF; ++p)
        t = x(0xFF);
    return t;
}

void clock_bytes(int n, uint8_t b) {
    for (int i = 0; i < n; ++i)
        x(b);
}

void wait_not_busy() {
    for (int i = 0; x(0xFF) == 0x00 && i < 100000; ++i) {
    }
}

// Блок записи: токен, данные, CRC, ответ на данные.
void host_block(uint8_t token, uint8_t seed) {
    x(0xFF);
    x(token);
    for (int i = 0; i < 512; ++i)
        x(static_cast<uint8_t>(seed + i * 3));
    clock_bytes(2, 0xFF);
    x(0xFF);
}

// Число из строки журнала после ключа key; key не найден - 0xFFFFFFFF.
unsigned long num_after(const char* text, const char* key) {
    const char* p = text != nullptr ? std::strstr(text, key) : nullptr;
    return p ? std::strtoul(p + std::strlen(key), nullptr, 10) : 0xFFFFFFFFul;
}

// Свёртка строк следа без отметки времени ("  N ms ").
uint32_t trace_hash(const char* log) {
    uint32_t h = soundsinth::util::kFnvSeed;
    for (const char* line = log; *line != 0;) {
        const char* end = std::strchr(line, '\n');
        const size_t n  = end ? static_cast<size_t>(end - line) : std::strlen(line);
        const char* ms  = std::strstr(line, " ms ");
        const char* p   = (ms != nullptr && ms < line + n) ? ms + 4 : line;
        h               = soundsinth::util::fnv1a(h, reinterpret_cast<const uint8_t*>(p), static_cast<size_t>(line + n - p));
        line            = end ? end + 1 : line + n;
    }
    return h;
}

void test_emu_wire_golden() {
    std::printf("test_sd_emu_wire_golden\n");
    fill_disk();
    devices::sd::sd_spi_emu_init();
    g_who         = SdOwner::DivMmc;
    g_test_log[0] = 0;
    devices::sd::sd_spi_log_stats();
    const char* dm0           = std::strstr(g_test_log, "| divmmc");
    const unsigned long cmds0 = num_after(dm0, "cmd=");
    const unsigned long rd0   = num_after(dm0, "rd=");
    const unsigned long wf0   = num_after(dm0, "wfail=");
    const uint32_t writes0    = g_writes;
    g_wire                    = soundsinth::util::kFnvSeed;
    g_wire_on                 = true;

    sel(true);
    cmd(17, 5); // до инициализации
    cmd(24, 5);
    cmd(0, 0);
    cmd(8, 0x1AA);
    clock_bytes(4, 0xFF);
    cmd(58, 0);
    clock_bytes(4, 0xFF);
    cmd(55, 0);
    cmd(41, 0x40000000u);
    cmd(58, 0);
    clock_bytes(4, 0xFF);
    cmd(9, 0);
    wait_token();
    clock_bytes(18, 0xFF);
    cmd(10, 0);
    wait_token();
    clock_bytes(18, 0xFF);
    cmd(13, 0);
    clock_bytes(1, 0xFF);
    cmd(16, 512);
    cmd(16, 1024);
    cmd(59, 0);
    cmd(5, 0);
    cmd(55, 0);
    cmd(13, 0); // ACMD13 разбирается как CMD13
    clock_bytes(1, 0xFF);
    cmd(17, kDiskSectors);
    clock_bytes(2, 0xFF);
    // Одиночные подряд: второй - из упреждения.
    for (uint32_t lba = 100; lba < 102; ++lba) {
        cmd(17, lba);
        wait_token();
        clock_bytes(514, 0xFF);
    }
    // Пачка: два блока целиком, CMD12 посреди третьего.
    cmd(18, 200);
    for (int b = 0; b < 2; ++b) {
        wait_token();
        clock_bytes(514, 0xFF);
    }
    wait_token();
    clock_bytes(100, 0xFF);
    cmd(12, 0);
    clock_bytes(4, 0xFF);
    // Запись одиночная и статус.
    cmd(24, 300);
    host_block(0xFE, 0x10);
    wait_not_busy();
    cmd(13, 0);
    clock_bytes(1, 0xFF);
    // Пачка записи, третий блок за концом носителя.
    cmd(25, kDiskSectors - 2u);
    for (uint8_t b = 0; b < 3; ++b) {
        host_block(0xFC, static_cast<uint8_t>(0x40 + b));
        wait_not_busy();
    }
    x(0xFD);
    wait_not_busy();
    cmd(13, 0);
    clock_bytes(1, 0xFF);
    cmd(24, kDiskSectors);
    // Снятие выбора посреди чтения.
    cmd(17, 400);
    wait_token();
    clock_bytes(10, 0xFF);
    sel(false);
    x(0xFF);
    sel(true);
    cmd(17, 401);
    wait_token();
    clock_bytes(514, 0xFF);
    // Повторный выбор под незаписанным блоком: занята, пока цикл стоит.
    g_task_enabled = false;
    cmd(24, 310);
    host_block(0xFE, 0x5A);
    sel(false);
    x(0xFF);
    sel(true);
    clock_bytes(20, 0xFF);
    g_task_enabled = true;
    wait_not_busy();
    // Застревание: после CMD24 байты без токена.
    cmd(24, 320);
    clock_bytes(4200, 0x00);
    x(0xFD);
    sel(false);
    x(0xFF);
    // Смена носителя: ёмкость меньше, карта снова не готова.
    g_card_sectors = 2048;
    devices::sd::sd_spi_task();
    sel(true);
    cmd(17, 100);
    cmd(0, 0);
    cmd(55, 0);
    cmd(41, 0x40000000u);
    cmd(9, 0);
    wait_token();
    clock_bytes(18, 0xFF);
    cmd(17, 3000);
    clock_bytes(2, 0xFF);
    sel(false);
    x(0xFF);
    // Вторая сторона: свой автомат с нуля, писатель носителя - первая.
    g_who = SdOwner::ZController;
    sel(true);
    cmd(17, 50);
    sel(false);
    CHECK(init_card());
    sel(true);
    cmd(18, 60);
    wait_token();
    clock_bytes(514, 0xFF);
    wait_token();
    clock_bytes(50, 0xFF);
    sel(false);
    x(0xFF);
    sel(true);
    cmd(24, 60);
    sel(false);
    wire(devices::sd::sd_spi_latch(SdOwner::ZController));
    wire(devices::sd::sd_spi_latch(SdOwner::DivMmc));
    g_wire_on = false;
    g_who     = SdOwner::ZController;

    std::printf("  wire hash %08X, writes %u\n", g_wire, g_writes - writes0);
    CHECK_EQ(g_wire, 0xDAFF5422u);
    CHECK_EQ(g_writes - writes0, 4u);

    g_test_log[0] = 0;
    devices::sd::sd_spi_log_trace_if_new();
    const uint32_t th = trace_hash(g_test_log);
    std::printf("  trace hash %08X\n", th);
    CHECK_EQ(th, 0xEC16ADC7u);
    g_test_log[0] = 0;
    devices::sd::sd_spi_log_stats();
    const char* dm = std::strstr(g_test_log, "| divmmc");
    CHECK(dm != nullptr);
    if (dm != nullptr) {
        std::printf("  divmmc cmd +%lu rd +%lu wfail +%lu last %lu\n", num_after(dm, "cmd=") - cmds0, num_after(dm, "rd=") - rd0, num_after(dm, "wfail=") - wf0,
                    num_after(dm, "last="));
        CHECK_EQ(num_after(dm, "cmd=") - cmds0, 37ul);
        CHECK_EQ(num_after(dm, "rd=") - rd0, 9ul);
        CHECK_EQ(num_after(dm, "wfail=") - wf0, 0ul);
        CHECK_EQ(num_after(dm, "last="), 17ul);
    }
    g_card_sectors = kDiskSectors;
}

// Драйвер карты: рукопожатие, чтение одиночное и пачкой, запись, битые
// блоки и повтор, сектор без токена с переинициализацией, частота выше
// 25 МГц, отказ на каждом шаге рукопожатия. Свёртка - запросы драйвера и
// ответы линии; строки журнала - целиком.
void test_card_driver_wire_golden() {
    std::printf("test_sd_card_driver_wire_golden\n");
    g_test_time_us += 120000000u; // окна переинициализации прошлых тестов позади
    fill_disk();
    devices::sd::sd_spi_emu_init();
    const uint32_t faults0   = devices::sd::sd_card_faults();
    g_wire                   = soundsinth::util::kFnvSeed;
    g_wire_on                = true;
    uint32_t logs            = soundsinth::util::kFnvSeed;
    const auto log_handshake = [&logs]() {
        g_test_log[0] = 0;
        devices::sd::sd_card_log_handshake();
        logs = soundsinth::util::fnv1a(logs, reinterpret_cast<const uint8_t*>(g_test_log), std::strlen(g_test_log));
    };

    CHECK(devices::sd::sd_card_init());
    log_handshake();
    uint8_t buf[512];
    for (uint32_t lba = 0; lba < 3; ++lba)
        CHECK(card_read_ok(lba));
    static uint8_t run[16 * 512];
    CHECK(devices::sd::sd_card_read_run(64, 16, run));
    CHECK(std::memcmp(run, g_disk[64], sizeof(run)) == 0);
    std::memset(buf, 0x3D, sizeof(buf));
    CHECK(devices::sd::sd_card_write_sector(77, buf));
    CHECK(card_read_ok(77));
    g_corrupt_blocks = 1;
    CHECK(card_read_ok(5));
    g_corrupt_blocks = 3;
    CHECK(!card_read_ok(6));
    g_corrupt_blocks = 1;
    CHECK(!devices::sd::sd_card_read_run(300, 16, run));
    g_corrupt_blocks = 0;
    CHECK_EQ(devices::sd::sd_card_set_speed(50000000u), 25000000u);
    CHECK_EQ(devices::sd::sd_card_set_speed(25000000u), 25000000u);
    g_fail_read_lba = 90;
    CHECK(!card_read_ok(90));
    CHECK(!devices::sd::sd_card_read_run(88, 4, run));
    g_fail_read_lba  = 0xFFFFFFFFu;
    g_test_time_us  += 2000000u;
    CHECK(card_read_ok(90));

    // Отказ на каждом шаге рукопожатия.
    g_cmd8_bad_echo = true;
    CHECK(!devices::sd::sd_card_init());
    g_cmd8_bad_echo = false;
    log_handshake();
    g_fail_cmd = 58;
    CHECK(!devices::sd::sd_card_init());
    log_handshake();
    g_fail_cmd  = 16;
    g_card_sdsc = true;
    CHECK(!devices::sd::sd_card_init());
    g_card_sdsc = false;
    log_handshake();
    g_fail_cmd = 9;
    CHECK(!devices::sd::sd_card_init());
    log_handshake();
    g_fail_cmd = -1;
    CHECK(devices::sd::sd_card_init());
    CHECK(card_read_ok(1));
    g_wire_on = false;

    const uint32_t faults = devices::sd::sd_card_faults() - faults0;
    std::printf("  wire hash %08X, log hash %08X, faults +%u\n", g_wire, logs, faults);
    CHECK_EQ(g_wire, 0xD09DB7B1u);
    CHECK_EQ(logs, 0x06F6A1BDu);
    CHECK_EQ(faults, 11u);
}

// Кэши арбитра: попадания и промахи по сторонам, блоки и упреждение,
// хвост носителя короче блока, построчный кэш без блочного буфера.
struct CacheStats {
    unsigned long v[9];
};

CacheStats cache_stats() {
    g_test_log[0] = 0;
    devices::storage::storage_log_health();
    CacheStats s;
    s.v[0] = num_after(g_test_log, "board ");
    s.v[1] = num_after(std::strstr(g_test_log, "board "), "/");
    s.v[2] = num_after(g_test_log, "divmmc ");
    s.v[3] = num_after(std::strstr(g_test_log, "divmmc "), "/");
    s.v[4] = num_after(g_test_log, "zctrl ");
    s.v[5] = num_after(std::strstr(g_test_log, "zctrl "), "/");
    s.v[6] = num_after(g_test_log, "served from block ");
    s.v[7] = num_after(g_test_log, ", filled ");
    s.v[8] = num_after(g_test_log, "ahead ");
    return s;
}

void test_storage_cache_golden() {
    std::printf("test_storage_cache_golden\n");
    fill_disk();
    g_usb_present = true;
    const auto dm = SdOwner::DivMmc;
    const auto zc = SdOwner::ZController;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Usb);
    devices::storage::storage_init();
    const CacheStats s0 = cache_stats();
    const uint32_t r0   = g_reads;
    uint32_t h          = soundsinth::util::kFnvSeed;
    uint8_t out[512];
    const auto rd = [&h, &out](bool ok) {
        const uint8_t b[2] = {static_cast<uint8_t>(ok ? 1 : 0), out[0]};
        h                  = soundsinth::util::fnv1a(h, b, 2);
    };

    // Карта, блок 16: промах, упреждение, попадания, следующий блок.
    rd(devices::storage::storage_emulator_read(dm, 33, out));
    devices::storage::storage_prefetch_step();
    for (uint32_t lba = 34; lba < 48; ++lba)
        rd(devices::storage::storage_emulator_read(dm, lba, out));
    devices::storage::storage_prefetch_step();
    rd(devices::storage::storage_emulator_read(dm, 48, out));
    // Флешка, блок 8: блок с базой ноль не заказывается.
    rd(devices::storage::storage_emulator_read(zc, 3, out));
    devices::storage::storage_prefetch_step();
    rd(devices::storage::storage_emulator_read(zc, 4, out));
    rd(devices::storage::storage_emulator_read(zc, 9, out));
    devices::storage::storage_prefetch_step();
    rd(devices::storage::storage_emulator_read(zc, 10, out));
    rd(devices::storage::storage_emulator_read(zc, 12, out));
    devices::storage::storage_prefetch_step();
    rd(devices::storage::storage_emulator_read(zc, 17, out));
    // Плата фоном, запись платы в лежащий блок.
    rd(devices::storage::storage_read_background(500, out));
    devices::storage::storage_prefetch_step_board();
    rd(devices::storage::storage_read_background(501, out));
    uint8_t blk[512];
    std::memset(blk, 0x6E, sizeof(blk));
    CHECK(devices::storage::storage_write_board(502, blk));
    rd(devices::storage::storage_read_background(502, out));
    rd(devices::storage::storage_tool_read(devices::hal::Medium::Usb, 20, out));
    CHECK(devices::storage::storage_tool_write(devices::hal::Medium::Usb, 21, blk));
    rd(devices::storage::storage_tool_read(devices::hal::Medium::Usb, 21, out));
    // Записанное отдаёт блок 16..23, а не прежние данные: у флешки теста
    // блок 8 секторов, и блок 8..15 сектор 21 не держит.
    CHECK_EQ(out[0], 0x6Eu);
    // Хвост: последний блок не целиком на носителе.
    g_card_sectors = 4090;
    rd(devices::storage::storage_emulator_read(dm, 4081, out));
    devices::storage::storage_prefetch_step();
    rd(devices::storage::storage_emulator_read(dm, 4082, out));
    rd(devices::storage::storage_emulator_read(dm, 4090, out));
    g_card_sectors = kDiskSectors;
    // Без блочного буфера.
    devices::storage::storage_attach_block_cache(nullptr, 0, nullptr);
    rd(devices::storage::storage_emulator_read(dm, 5, out));
    rd(devices::storage::storage_emulator_read(dm, 5, out));
    rd(devices::storage::storage_emulator_read(zc, 5, out));
    rd(devices::storage::storage_read_background(6, out));
    rd(devices::storage::storage_read_background(6, out));
    rd(devices::storage::storage_emulator_read(dm, 6, out));

    const CacheStats s1 = cache_stats();
    std::printf("  data hash %08X, reads %u, deltas", h, g_reads - r0);
    for (int i = 0; i < 9; ++i)
        std::printf(" %lu", s1.v[i] - s0.v[i]);
    std::printf("\n");
    CHECK_EQ(h, 0x9DFA1FA6u);
    CHECK_EQ(g_reads - r0, 76u);
    // board, divmmc, zctrl (попадания/промахи); из блока, наполнено, заранее.
    const unsigned long want[9] = {4, 3, 16, 5, 3, 4, 22, 5, 5};
    for (int i = 0; i < 9; ++i)
        CHECK_EQ(s1.v[i] - s0.v[i], want[i]);
    g_usb_present = false;
    devices::storage::storage_set_emulator_medium(zc, devices::hal::Medium::Card);
    fill_disk();
}

void run_sd_spi_emu_tests() {
    test_sectors_from_csd();
    test_card_edges();
    test_read_after_write();
    test_board_write_drops_emulator_slot();
    test_write_reselect_busy();
    test_write_during_prefetch();
    test_slot_reask_during_read();
    test_deferred_media_change_idle();
    test_late_sector();
    test_media_read_failure();
    test_ocr_and_status_like_real_card();
    test_split_cards_and_single_writer();
    test_multi_block_write();
    test_writer_lock_per_medium();
    test_busy_medium_request_waits();
    test_write_beyond_end_takes_no_lock();
    test_media_swap_drops_blocks();
    test_media_swap_drops_slots();
    test_media_swap_while_idle();
    test_card_swapped_same_size();
    test_volume_sees_card_swap();
    test_volume_window_sees_machine_write();
    test_card_kept_on_foreign_change();
    test_card_block_run16();
    test_failed_write_drops_block();
    test_line_cache_without_blocks();
    test_background_yields_on_block_path();
    test_yield_does_not_fill_block_twice();
    test_write_failure_is_logged();
    test_zcontroller_ports();
    test_divmmc_two_cards();
    test_zcontroller_latch_not_overwritten();
    test_divmmc_latch_not_overwritten();
    test_reentrant_step();
    test_media_change_is_published();
    test_write_does_not_block_other_side();
    test_write_reposts_waiting_read();
    test_medium_follows_owner();
    test_card_driver();
    test_card_old_kinds();
    test_emu_wire_golden();
    test_card_driver_wire_golden();
    test_card_write_failures();
    test_card_high_speed();
    test_card_speed_tool();
    test_storage_cache_golden();
}
