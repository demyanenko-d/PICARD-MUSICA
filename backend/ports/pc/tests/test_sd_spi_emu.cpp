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
#include "devices/storage/storage_host.h"
#include "devices/zcontroller/zcontroller.h"

uint32_t g_test_time_us = 0;
char g_test_log[8192];
uint32_t g_test_spi_hz = 1;  // делитель частоты линии, до первой установки - не ноль

namespace {
uint8_t g_port_rd[256];
devices::hal::PortWriteFn g_port_wr[256];
devices::hal::PortReadDoneFn g_port_rd_done[256];
} // namespace

// Порты Z80 теста (devices/hal/z80_ports.h): таблица ответов и обработчики
// в обычных массивах, тест дёргает их сам.
void devices::hal::z80_port_set_read(uint8_t port, uint8_t value) { g_port_rd[port] = value; }
void devices::hal::z80_port_on_write(uint8_t port, PortWriteFn fn) { g_port_wr[port] = fn; }
void devices::hal::z80_port_on_read_done(uint8_t port, PortReadDoneFn fn) { g_port_rd_done[port] = fn; }

namespace {
constexpr uint32_t kDiskSectors = 4096;
uint8_t g_disk[kDiskSectors][512];
uint32_t g_writes = 0;
uint32_t g_fail_read_lba = 0xFFFFFFFFu;   // носитель не отдаёт этот сектор
uint32_t g_fail_write_lba = 0xFFFFFFFFu;  // носитель не принимает этот сектор
bool g_task_enabled = true;
// Обмены хоста посреди чтения сектора g_hook_lba: обработчик прерывания
// вклинивается в цикл Core1, пока тот читает носитель.
void (*g_read_hook)() = nullptr;
uint32_t g_hook_lba = 0xFFFFFFFFu;
} // namespace

namespace devices::storage {
bool storage_init() { return true; }
bool storage_present() { return true; }
uint32_t storage_sector_count() { return kDiskSectors; }
bool storage_read(uint32_t lba, uint8_t* dst) {
    if (lba >= kDiskSectors || lba == g_fail_read_lba) return false;
    if (lba == g_hook_lba && g_read_hook) {
        void (*hook)() = g_read_hook;
        g_read_hook = nullptr;
        std::memcpy(dst, g_disk[lba], 256);
        hook();
        std::memcpy(dst + 256, g_disk[lba] + 256, 256);
        return true;
    }
    std::memcpy(dst, g_disk[lba], 512);
    return true;
}
bool storage_write(uint32_t lba, const uint8_t* src) {
    if (lba >= kDiskSectors || lba == g_fail_write_lba) return false;
    std::memcpy(g_disk[lba], src, 512);
    ++g_writes;
    return true;
}
} // namespace devices::storage

namespace {

using devices::sd::SdOwner;
SdOwner g_who = SdOwner::ZController;

// Обмен байтом: около 15 мкс на опрос у Z80 3.5 МГц.
uint8_t x(uint8_t b) {
    g_test_time_us += 15;
    const uint8_t r = devices::sd::sd_spi_byte(g_who, b);
    if (g_task_enabled) devices::sd::sd_spi_task();
    return r;
}

uint8_t cmd(uint8_t c, uint32_t arg) {
    x(0xFF);
    uint8_t f[6] = {static_cast<uint8_t>(0x40 | c), static_cast<uint8_t>(arg >> 24), static_cast<uint8_t>(arg >> 16),
                    static_cast<uint8_t>(arg >> 8), static_cast<uint8_t>(arg), 0};
    f[5] = devices::sd::crc7(f, 5);
    for (int i = 0; i < 6; ++i) x(f[i]);
    for (int i = 0; i < 10; ++i) {
        const uint8_t r = x(0xFF);
        if ((r & 0x80) == 0) return r;
    }
    return 0xFF;
}

void sel(bool on) { devices::sd::sd_spi_select(g_who, on); }

void fill_disk() {
    for (uint32_t s = 0; s < kDiskSectors; ++s) {
        for (uint32_t i = 0; i < 512; ++i) g_disk[s][i] = static_cast<uint8_t>(s * 7u + i);
    }
    g_writes = 0;
    g_fail_read_lba = 0xFFFFFFFFu;
    g_fail_write_lba = 0xFFFFFFFFu;
    g_task_enabled = true;
    g_read_hook = nullptr;
    g_hook_lba = 0xFFFFFFFFu;
    g_who = SdOwner::ZController;
}

bool init_card() {
    sel(true);
    const uint8_t r0 = cmd(0, 0);
    uint8_t r = 0xFF;
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
    bool ok = false;
    uint32_t polls = 0;
    bool crc_ok = false;
    bool data_ok = false;
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
    for (int i = 0; i < 512; ++i) out[i] = x(0xFF);
    const uint8_t hi = x(0xFF), lo = x(0xFF);
    rr.crc_ok = devices::sd::crc16(out, 512) == static_cast<uint16_t>((hi << 8) | lo);
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
    for (int i = 0; i < 512; ++i) x(data[i]);
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
    std::printf("  %s: сектор %u - чтение %d, данные %d, CRC %d\n", name, lba, rr.ok, rr.data_ok, rr.crc_ok);
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
        for (int p = 0; p < 0x4000 && t == 0xFF; ++p) t = x(0xFF);
        for (int i = 0; i < 512 + 2; ++i) x(0xFF);
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
    std::printf("  занята %u опросов после повторного выбора\n", busy);
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
    uint8_t t = 0xFF;
    for (int p = 0; p < 0x4000; ++p) {
        t = x(0xFF);
        if (t != 0xFF) break;
        devices::sd::sd_spi_task();
    }
    CHECK_EQ(t, 0xFE);
    for (int i = 0; i < 512 + 2; ++i) x(0xFF);
    g_hook_lba = 6;
    g_read_hook = hook_write_30;
    devices::sd::sd_spi_task();
    g_task_enabled = true;
    for (int i = 0; i < 4; ++i) x(0xFF);
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
    uint8_t t = 0xFF;
    for (; polls < 0x4000; ++polls) {
        if (polls == 5000) g_task_enabled = true;
        t = x(0xFF);
        if (t != 0xFF) break;
    }
    uint8_t buf[512];
    for (int i = 0; i < 512; ++i) buf[i] = x(0xFF);
    x(0xFF);
    x(0xFF);
    sel(false);
    x(0xFF);
    std::printf("  токен 0x%02X после %u опросов\n", t, polls);
    CHECK_EQ(t, 0xFE);
    CHECK(std::memcmp(buf, g_disk[700], 512) == 0);
}

// (г) Носитель не отдал сектор: хост получает 0xFF до своего предела,
// следующее чтение того же сектора проходит.
void test_media_read_failure() {
    std::printf("test_sd_emu_media_read_failure\n");
    fresh_card();
    g_fail_read_lba = 1234;
    const ReadRes bad = read_block(1234);
    std::printf("  отказ носителя: токен получен %d, опросов %u\n", bad.ok, bad.polls);
    CHECK(!bad.ok);
    g_fail_read_lba = 0xFFFFFFFFu;
    CHECK(read_matches("повтор", 1234));
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
    for (uint8_t& b : ocr) b = x(0xFF);
    CHECK_EQ(ocr[0], 0x40);
    CHECK_EQ(ocr[1], 0xff);
    CHECK_EQ(ocr[2], 0x80);
    sel(false);
    CHECK(init_card());
    sel(true);
    CHECK_EQ(cmd(58, 0), 0x00);
    for (uint8_t& b : ocr) b = x(0xFF);
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

// (в) Арбитр и (д) единственный писатель: Z-Controller выбрал карту и ушёл;
// чужой байт при занятой шине - 0xFF; через 150 мс DivMMC отнимает шину;
// писатель - один (Z-Controller уже писал), CMD24 второго - 0x40, носитель
// не тронут.
void test_arbiter_and_single_writer() {
    std::printf("test_sd_emu_arbiter_and_single_writer\n");
    fresh_card();
    CHECK_EQ(write_block(50, 0x11), 0x05);   // писатель - Z-Controller
    sel(true);
    x(0xFF);
    devices::sd::sd_spi_select(SdOwner::DivMmc, true);
    CHECK_EQ(devices::sd::sd_spi_byte(SdOwner::DivMmc, 0xFF), 0xFF);
    // Снятие выбора чужим владельца не меняет: обмен Z-Controller идёт.
    devices::sd::sd_spi_select(SdOwner::DivMmc, false);
    CHECK_EQ(cmd(13, 0), 0x00);
    x(0xFF);
    const uint32_t steals_before = devices::sd::sd_spi_owner_steals();
    g_test_time_us += 150000;
    devices::sd::sd_spi_select(SdOwner::DivMmc, true);
    CHECK_EQ(devices::sd::sd_spi_owner_steals(), steals_before + 1u);
    devices::sd::sd_spi_byte(SdOwner::DivMmc, 0xFF);
    uint8_t f[6] = {0x40 | 24, 0, 0, 0x03, 0x00, 0};
    f[5] = devices::sd::crc7(f, 5);
    for (int i = 0; i < 6; ++i) devices::sd::sd_spi_byte(SdOwner::DivMmc, f[i]);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && (r & 0x80); ++i) r = devices::sd::sd_spi_byte(SdOwner::DivMmc, 0xFF);
    std::printf("  CMD24 второго писателя -> 0x%02X, записей %u\n", r, g_writes);
    CHECK_EQ(r, 0x40);
    CHECK_EQ(g_writes, 1u);
    devices::sd::sd_spi_select(SdOwner::DivMmc, false);
}

// --- Хост через порты Z-Controller, как драйвер sd.s ---

constexpr uint8_t kZcData = 0x57;
constexpr uint8_t kZcCtrl = 0x77;

void zout(uint8_t port, uint8_t v) {
    g_test_time_us += 15;
    g_port_wr[port](port, v);
    devices::sd::sd_spi_task();
}

uint8_t zin(uint8_t port) {
    g_test_time_us += 15;
    const uint8_t v = g_port_rd[port];
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
    for (int sh = 24; sh >= 0; sh -= 8) zout(kZcData, static_cast<uint8_t>(arg >> sh));
    zout(kZcData, c == 0 ? 0x95 : (c == 8 ? 0x87 : 0x01));
    for (int i = 0; i < 10; ++i) {
        *r1 = zin(kZcData);
        if ((*r1 & 0x80) == 0) return true;
    }
    return false;
}

bool zinit() {
    zout(kZcCtrl, 0x03);
    for (int i = 0; i < 500; ++i) zout(kZcData, 0xFF);
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
    for (int i = 0; i < 512; ++i) out[i] = zin(kZcData);
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
        for (int i = 0; i < 512; ++i) buf[i] = zin(kZcData);
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
    CHECK_EQ(g_port_rd[kZcCtrl], 0xFC);
    CHECK(zinit());
    uint8_t buf[512];
    CHECK(zread_sector(1000, buf));
    CHECK(std::memcmp(buf, g_disk[1000], 512) == 0);
    uint32_t bad = 0;
    for (uint32_t w = 0; w < 64; ++w) bad += zread_multi_bad(2000 + w * 8, 8);
    std::printf("  64 окна по 8 секторов, битых %u\n", bad);
    CHECK_EQ(bad, 0u);
    CHECK(zread_sector(3000, buf));
    CHECK(std::memcmp(buf, g_disk[3000], 512) == 0);
}

// --- Драйвер карты (sd_card_spi.cpp) против эмулятора ---
//
// На том конце SPI - эмулятор, владелец Z-Controller (он же единственный
// писатель эмулятора в этом прогоне). Часы идут со скоростью линии:
// 20 мкс на байт при 400 кГц, 0.32 мкс при 25 МГц.

bool g_card_silent = false;      // карты нет: MISO всегда 0xFF
uint32_t g_corrupt_blocks = 0;   // столько следующих блоков приходят с битым битом
bool g_stuck_idle = false;       // карта в idle: CMD17, ACMD41 и CMD1 - R1 0x01
bool g_r1_override = false;
uint8_t g_prev_mosi = 0xFF;
uint32_t g_line_ns = 0;

uint8_t line_byte(uint8_t out) {
    g_line_ns += static_cast<uint32_t>(8000000000ull / g_test_spi_hz);
    g_test_time_us += g_line_ns / 1000u;
    g_line_ns %= 1000u;
    if (g_card_silent) return 0xFF;
    if (g_stuck_idle && g_prev_mosi == 0xFF && (out == (0x40 | 17) || out == (0x40 | 41) || out == (0x40 | 1))) {
        g_r1_override = true;
    }
    g_prev_mosi = out;
    uint8_t in = devices::sd::sd_spi_byte(SdOwner::ZController, out);
    devices::sd::sd_spi_task();
    if (g_r1_override && (in & 0x80) == 0) {
        g_r1_override = false;
        in = 0x01;
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
    h.reinit = log_num("переинициализаций ");
    h.reinit_failed = log_num("неудачных ");
    h.r1 = log_num("сбои r1 ");
    h.token = log_num("токен ");
    h.crc = log_num(", crc ");
    h.crc_retry = log_num("плохих crc ");
    h.slow = log_num("дольше 100 мс ");
    h.rejects = log_num("отклонено окном переинициализации ");
    return h;
}

bool card_read_ok(uint32_t lba) {
    uint8_t buf[512];
    return devices::sd::sd_card_read_sector(lba, buf) && std::memcmp(buf, g_disk[lba], 512) == 0;
}

// Обслуживание хоста, пока драйвер ждёт карту. Заодно лезет к карте сам:
// такое вложенное чтение обязано отказать, иначе его команда ушла бы
// посреди чужой при опущенном CS.
uint32_t g_service_calls = 0;
uint32_t g_service_nested_ok = 0;
void count_service(void*) {
    ++g_service_calls;
    uint8_t buf[512];
    if (devices::sd::sd_card_read_sector(1, buf)) ++g_service_nested_ok;
}

uint32_t timed_read(uint32_t lba, bool* ok) {
    const uint32_t t0 = g_test_time_us;
    *ok = card_read_ok(lba);
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
    for (uint32_t lba = 0; lba < 64; ++lba) bad += card_read_ok(lba) ? 0u : 1u;
    CHECK_EQ(bad, 0u);
    uint8_t w[512];
    for (uint32_t i = 0; i < 512; ++i) w[i] = static_cast<uint8_t>(i ^ 0x5A);
    CHECK(devices::sd::sd_card_write_sector(77, w));
    CHECK(std::memcmp(g_disk[77], w, 512) == 0);
    CHECK(card_read_ok(77));

    // Битый бит на линии: один блок - повтор снимает, три подряд - отказ.
    const Health h0 = health();
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
    bool ok = true;
    g_fail_read_lba = 90;
    const uint32_t dt1 = timed_read(90, &ok);
    CHECK(!ok);
    const uint32_t dt2 = timed_read(90, &ok);
    CHECK(!ok);
    g_fail_read_lba = 0xFFFFFFFFu;
    const uint32_t dt3 = timed_read(91, &ok);
    CHECK(!ok);
    CHECK(!devices::sd::sd_card_write_sector(92, w));
    std::printf("  сектор без токена %u мкс, повтор %u мкс, исправный следом %u мкс\n", dt1, dt2, dt3);
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
    g_test_time_us += 1100000u;
    g_stuck_idle = true;
    const uint32_t start = g_test_time_us;
    uint32_t in_init = 0, attempts = 0, last_end = 0;
    uint32_t gaps_ms[8] = {0};
    bool attempts_4s = true;
    while (g_test_time_us - start < 60000000u) {
        const uint32_t t0 = g_test_time_us;
        const uint32_t dt = timed_read(10, &ok);
        CHECK(!ok);
        if (dt > 1000000u) {
            if (attempts > 0 && attempts <= 8) gaps_ms[attempts - 1] = (t0 - last_end + 500u) / 1000u;
            if (dt < 4000000u || dt > 4300000u) attempts_4s = false;
            ++attempts;
            in_init += dt;
            last_end = g_test_time_us;
        }
        g_test_time_us += 1000u;
    }
    g_stuck_idle = false;
    std::printf("  застрявшая карта: попыток %u за 60 с, в инициализации %u%%, промежутки %u %u %u %u мс\n",
                attempts, in_init / 600000u, gaps_ms[0], gaps_ms[1], gaps_ms[2], gaps_ms[3]);
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
    g_service_calls = 0;
    g_service_nested_ok = 0;
    g_fail_read_lba = 93;
    const uint32_t svc_t0 = g_test_time_us;
    CHECK(!card_read_ok(93));
    const uint32_t svc_us = g_test_time_us - svc_t0;
    g_fail_read_lba = 0xFFFFFFFFu;
    devices::sd::sd_card_set_wait_service(nullptr, nullptr);
    std::printf("  обслуживание в ожидании: %u вызовов за %u мкс, вложенных чтений прошло %u\n", g_service_calls,
                svc_us, g_service_nested_ok);
    CHECK_EQ(g_service_nested_ok, 0ul);
    // Не чаще раза в миллисекунду (запас - на каждый отдельный цикл
    // ожидания, у них свой отсчёт) и не реже: ожидание не должно молчать.
    CHECK(g_service_calls <= svc_us / 1000u + 8u);
    CHECK(g_service_calls >= svc_us / 2000u);
}

} // namespace

// Шина SPI теста (devices/hal/spi.h): на том конце эмулятор карты, байты
// гоняет line_byte. Частота хранится, чтобы часы шли со скоростью линии.
uint32_t devices::hal::spi_open(uint32_t hz) { return spi_set_hz(hz); }

uint32_t devices::hal::spi_set_hz(uint32_t hz) {
    g_test_spi_hz = hz;
    return hz;
}

void devices::hal::spi_select(bool on) { devices::sd::sd_spi_select(SdOwner::ZController, on); }

uint8_t devices::hal::spi_xfer(uint8_t out) { return line_byte(out); }

void devices::hal::spi_read(uint8_t idle, uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst[i] = line_byte(idle);
    if (n == 512 && g_corrupt_blocks > 0) {
        --g_corrupt_blocks;
        dst[100] ^= 0x10;
    }
}

void devices::hal::spi_write(const uint8_t* src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) line_byte(src[i]);
}

void run_sd_spi_emu_tests() {
    test_read_after_write();
    test_write_reselect_busy();
    test_write_during_prefetch();
    test_late_sector();
    test_media_read_failure();
    test_ocr_and_status_like_real_card();
    test_arbiter_and_single_writer();
    test_zcontroller_ports();
    test_card_driver();
}
