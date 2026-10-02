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

#include "devices/hal/z80_ports.h"
#include "devices/hal/spi.h"
#include "devices/sd/sd_protocol.h"
#include "devices/sd/card_protocol.h"
#include "devices/sd/spi_emu.h"
#include "devices/hal/media.h"
#include "devices/storage/storage.h"
#include "devices/storage/storage_host.h"
#include "devices/zcontroller/zcontroller.h"

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

// Порты Z80 теста (devices/hal/z80_ports.h): таблица ответов и обработчики
// в обычных массивах, тест дёргает их сам.
void devices::hal::z80_port_set_read(uint8_t port, uint8_t value) {
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

namespace {
constexpr uint32_t kDiskSectors = 4096;
uint8_t g_disk[kDiskSectors][512];
uint8_t g_usb_disk[kDiskSectors][512];
uint32_t g_card_sectors   = kDiskSectors; // ёмкость пустышки: тест меняет её на ходу
bool g_usb_present        = false;
uint32_t g_writes         = 0;
uint32_t g_reads          = 0;           // обращений к носителю: ими видно, заказан ли сектор вовсе
uint32_t g_fail_read_lba  = 0xFFFFFFFFu; // носитель не отдаёт этот сектор
uint32_t g_fail_write_lba = 0xFFFFFFFFu; // носитель не принимает этот сектор
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
bool media_present(Medium m) {
    return m == Medium::Card || g_usb_present;
}
uint32_t media_sector_count(Medium) {
    return g_card_sectors;
}

// Пачкой - циклом: у пустышек выгоды нет, а путь блочного упреждения
// проверить надо, и проверяется он на флешке (media_run_sectors ниже).
bool media_read_run(Medium m, uint32_t lba, uint32_t count, uint8_t* dst);

uint32_t media_run_sectors(Medium) {
    return 8u;
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
    const bool present     = media_present(m);
    const uint32_t sectors = media_sector_count(m);
    if (st.present == present && st.sectors == sectors) return;
    st.present = present;
    st.sectors = sectors;
    ++g_media_generation;
}

const MediaState& media_state(Medium m) {
    media_state_publish(m); // пустышки меняет сам тест, событий у него нет
    return media_state_slot(m);
}

uint32_t media_state_generation() {
    media_state_publish(Medium::Card);
    media_state_publish(Medium::Usb);
    return g_media_generation;
}

bool media_read(Medium m, uint32_t lba, uint8_t* dst) {
    ++g_reads;
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
    if (lba >= kDiskSectors || lba == g_fail_write_lba) return false;
    std::memcpy((m == Medium::Usb ? g_usb_disk : g_disk)[lba], src, 512);
    ++g_writes;
    return true;
}

void media_log_health() {}
} // namespace devices::hal

namespace {

using devices::sd::SdOwner;
SdOwner g_who = SdOwner::ZController;

// Обмен байтом: около 15 мкс на опрос у Z80 3.5 МГц.
uint8_t x(uint8_t b) {
    g_test_time_us  += 15;
    const uint8_t r  = devices::sd::sd_spi_byte(g_who, b);
    if (g_task_enabled) devices::sd::sd_spi_task();
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
    g_writes         = 0;
    g_fail_read_lba  = 0xFFFFFFFFu;
    g_fail_write_lba = 0xFFFFFFFFu;
    g_task_enabled   = true;
    g_read_hook      = nullptr;
    g_hook_lba       = 0xFFFFFFFFu;
    g_who            = SdOwner::ZController;
    for (uint32_t s2 = 0; s2 < kDiskSectors; ++s2)
        std::memset(g_usb_disk[s2], 0, 512);
    g_usb_present  = false;
    g_card_sectors = kDiskSectors;
    for (uint8_t o = 0; o < devices::storage::kEmulatorCount; ++o) {
        devices::storage::storage_set_emulator_medium(o, devices::hal::Medium::Card);
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

// --- Хост через порты Z-Controller, как драйвер sd.s ---

constexpr uint8_t kZcData = devices::zcontroller::kPortData;
constexpr uint8_t kZcCtrl = devices::zcontroller::kPortCtrl;

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
    zout(kZcCtrl, 0x03);
    zout(kZcData, 0xFF);
}

// sd_cmd: снять выбор, такт, выбрать, такт, шесть байт, до десяти IN.
// CRC настоящий только у CMD0 и CMD8.
bool zcmd(uint8_t c, uint32_t arg, uint8_t* r1) {
    zout(kZcCtrl, 0x03);
    zout(kZcData, 0xFF);
    zout(kZcCtrl, 0x01);
    zout(kZcData, 0xFF);
    zout(kZcData, static_cast<uint8_t>(0x40 | c));
    for (int sh = 24; sh >= 0; sh -= 8)
        zout(kZcData, static_cast<uint8_t>(arg >> sh));
    zout(kZcData, c == 0 ? 0x95 : (c == 8 ? 0x87 : 0x01));
    for (int i = 0; i < 10; ++i) {
        *r1 = zin(kZcData);
        if ((*r1 & 0x80) == 0) return true;
    }
    return false;
}

bool zinit() {
    zout(kZcCtrl, 0x03);
    for (int i = 0; i < 500; ++i)
        zout(kZcData, 0xFF);
    uint8_t r = 0xFF;
    if (!zcmd(0, 0, &r) || r != 0x01) return false;
    if (!zcmd(8, 0x1AA, &r) || r != 0x01) return false;
    zin(kZcData);
    zin(kZcData);
    if (zin(kZcData) != 0x01 || zin(kZcData) != 0xAA) return false;
    for (int i = 0; i < 100; ++i) {
        if (!zcmd(55, 0, &r) || !zcmd(41, 0x40000000u, &r)) return false;
        if (r == 0) break;
    }
    if (r != 0) return false;
    if (!zcmd(58, 0, &r) || r != 0) return false;
    const bool ccs = (zin(kZcData) & 0x40) != 0;
    zin(kZcData);
    zin(kZcData);
    zin(kZcData);
    zrelease();
    return ccs;
}

bool ztoken() {
    for (int i = 0; i < 0x4000; ++i) {
        const uint8_t t = zin(kZcData);
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
        out[i] = zin(kZcData);
    zin(kZcData);
    zin(kZcData);
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
            buf[i] = zin(kZcData);
        zin(kZcData);
        zin(kZcData);
        if (std::memcmp(buf, g_disk[lba + s], 512) != 0) ++bad;
    }
    zcmd(12, 0, &r);
    for (int i = 0; i < 0x4000 && zin(kZcData) != 0xFF; ++i) {
    }
    zrelease();
    return bad;
}

// (е) Порт 0x57 конвейером через zcontroller.cpp: инициализация SDHC,
// одиночное чтение и окна CMD18 по 8 секторов - байт в байт с носителем;
// порт 0x77 после init - 0xFC при карте.
void test_zcontroller_ports() {
    std::printf("test_sd_emu_zcontroller_ports\n");
    fill_disk();
    devices::zcontroller::zcontroller_init(false); // эмулятор поднимает он сам: DivMMC в тесте нет
    CHECK_EQ(g_port_rd[kZcCtrl], devices::zcontroller::kCtrlCardPresent);

    // Обе стороны признака носителя. Носитель виден через свой эмулятор,
    // поэтому "карты нет" изображается флешкой, которую тест выдёргивает:
    // карта в пустышке есть всегда.
    const auto zc = static_cast<uint8_t>(SdOwner::ZController);
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
    const auto dm = static_cast<uint8_t>(SdOwner::DivMmc);
    const auto zc = static_cast<uint8_t>(SdOwner::ZController);
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
uint8_t g_prev_mosi       = 0xFF;
uint32_t g_line_ns        = 0;

uint8_t line_byte(uint8_t out) {
    g_line_ns      += static_cast<uint32_t>(8000000000ull / g_test_spi_hz);
    g_test_time_us += g_line_ns / 1000u;
    g_line_ns      %= 1000u;
    if (g_card_silent) return 0xFF;
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
}

void run_sd_spi_emu_tests() {
    test_sectors_from_csd();
    test_card_edges();
    test_read_after_write();
    test_write_reselect_busy();
    test_write_during_prefetch();
    test_late_sector();
    test_media_read_failure();
    test_ocr_and_status_like_real_card();
    test_split_cards_and_single_writer();
    test_zcontroller_ports();
    test_reentrant_step();
    test_media_change_is_published();
    test_write_does_not_block_other_side();
    test_write_reposts_waiting_read();
    test_medium_follows_owner();
    test_card_driver();
}
