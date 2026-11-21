// SPDX-License-Identifier: MIT
#pragma once

// Beta Disk платы (DISKSYS=TRDOS): четыре дисковода, образы TRD и SCL с карты
// или флешки; вставляет их навигатор по NMI.
//
// Подъём в два шага. bdi_reserve - при загрузке: блоки PSRAM дисководов и
// навигатора. bdi_start - на Core1, после подъёма портов и после
// копирования кода BDI в банки: ядро ВГ93, порты при DOS, поток. Цилиндры
// читает с носителя bdi_poll из цикла Core1, запись идёт на те же места
// файла.
//
// Beta Disk на шине без /WAIT: ответы на чтение лежат готовыми в ячейках,
// их ведёт поток по автомату ВГ93 (Vg93).
//
// Обработчики шины: запись в порт - событие в кольцо (запись #1F снимает
// в ячейках INTRQ, #7F - DRQ); чтение статуса при INTRQ и чтение DATA при
// DRQ снимают бит в ячейках и тоже кладут событие. Поток разбирает кольцо
// по порядку, исполняет смены автомата и после каждой публикует ячейки.
// Обращение, пришедшее до публикации смены, автомат исполняет раньше неё:
// Z80 видел прежние ячейки. Смена, опубликованная позже срока больше чем
// на kLateSlack, сдвигает часы диска на опоздание: байт стоит перед Z80
// весь свой срок, как у дисковода.
//
// Пока машина стоит в навигаторе (NMI), стоит и диск: смены автомата не
// исполняются, на выходе первая из них - через байт. Чтение сектора,
// прерванное NMI, продолжается с того же байта.
//
// Время - полупериоды CLK ВГ93 (0.5 мкс): метка на шине - 32 разряда,
// поток дополняет её до 64 по своему "сейчас".

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "devices/bdi/disk.h"
#include "devices/bdi/vg93.h"
#include "devices/hal/bus_worker.h"

namespace devices::bdi {

// --- Поток: ячейки портов, кольцо событий, часы диска ---

// Порты: регистры ВГ93 #1F, #3F, #5F, #7F (номер - A6:A5), системный #FF.
inline constexpr uint8_t kBdiPorts[5] = {kPortCmd, kPortTrack, kPortSector, kPortData, kPortSystem};
inline constexpr uint32_t kHalfPerUs  = 2;

class BdiHost {
public:
    // now - время шины при включении; clock - часы шины (nullptr - время
    // захода в service).
    void init(uint64_t now, hal::ClockFn clock = nullptr);
    // Машина стоит (навигатор): диск стоит вместе с ней. nullptr - не бывает.
    void use_pause(bool (*paused)()) { paused_ = paused; }

    // Ячейка ответа порта из kBdiPorts.
    const uint8_t* cell(uint8_t port) const { return &cells_[slot(port)]; }

    // Обработчики шины.
    void bus_write(uint8_t port, uint8_t v, uint32_t stamp);
    void bus_read_done(uint8_t port, uint32_t stamp);

    void machine_reset(uint32_t stamp) { push(kReset, 0, stamp); }
    void source_ready(int drive, uint32_t stamp) { push(kReady, static_cast<uint8_t>(drive), stamp); }
    void swap_ab(uint32_t stamp) { push(kSwap, 0, stamp); }
    // Дисководу drive - источник src (nullptr - пусто). inserted() растёт,
    // когда поток его поставил.
    void insert(int drive, Disk* src, uint32_t stamp) {
        pending_[drive] = src;
        push(kInsert, static_cast<uint8_t>(drive), stamp);
    }
    uint32_t inserted() const { return inserted_.load(std::memory_order_acquire); }

    // Поток: обращения и смены автомата до now. Возврат - будильник, время шины.
    uint64_t service(uint64_t now);

    // Журнал. lost - кольцо было полным; late_max - наибольшее опоздание
    // смены, late16 - смен позже срока больше чем на 16 мкс; stretch - на
    // сколько встали часы диска; busy_max - самый долгий заход потока
    // (полупериоды); outside - записи в порты вне DOS; pauses и paused -
    // остановки диска на навигатор и их сумма (полупериоды).
    struct Stats {
        uint32_t events, runs, lost, outside, late_max, late16, busy_max, pauses;
        uint64_t stretch, paused;
    };
    const Stats& stats() const { return st_; }
    void note_busy(uint64_t t) {
        if (t > st_.busy_max) st_.busy_max = static_cast<uint32_t>(t);
    }
    void note_outside() { st_.outside++; }
    // След концов команд типов II/III: команда, статус, TRK, SEC, чтения #7F.
    struct Trace {
        uint8_t cmd, st, trk, sec;
        uint16_t reads;
    };
    static constexpr uint32_t kTrace = 256;
    uint32_t trace_count() const { return trace_n_; }
    const Trace& trace(uint32_t i) const { return trace_[i % kTrace]; }
    const Vg93& vg93() const { return vg93_; }

private:
    struct Ev {
        uint32_t stamp;
        uint8_t port, v;
        volatile uint8_t ready;
    };
    static constexpr uint32_t kRing      = 64;
    static constexpr uint32_t kReserve   = 16;   // мест только записям и управлению
    static constexpr uint64_t kLateSlack = 8;    // 4 мкс
    static constexpr uint64_t kPausePoll = 2000; // 1 мс: проверка выхода из навигатора
    static constexpr uint8_t kReset      = 0;    // события - не порты Beta Disk
    static constexpr uint8_t kReady      = 1;
    static constexpr uint8_t kSwap       = 2;
    static constexpr uint8_t kInsert     = 3;
    static constexpr uint8_t kReadStatus = 4;
    static constexpr uint8_t kReadData   = 5;
    static unsigned slot(uint8_t port) { return port == kPortSystem ? 4u : (port >> 5) & 3u; }
    void push(uint8_t port, uint8_t v, uint32_t stamp, bool write = true);
    void apply(const Ev& e, uint64_t now);
    void publish(uint64_t t);
    void end_trace();

    Vg93 vg93_;
    alignas(8) uint8_t cells_[8] = {};
    Ev ring_[kRing]              = {};
    std::atomic<uint32_t> head_{0}; // занято писателями
    uint32_t tail_          = 0;    // разобрано потоком
    uint64_t off_           = 0;    // время шины минус время диска
    uint64_t last_          = 0;    // время диска последнего обращения или смены
    hal::ClockFn clock_     = nullptr;
    bool (*paused_)()       = nullptr;
    bool frozen_            = false;
    Disk* pending_[kDrives] = {};
    std::atomic<uint32_t> inserted_{0};
    Stats st_            = {};
    bool busy_           = false; // команда II/III идёт: для следа
    uint16_t reads_      = 0;
    Trace trace_[kTrace] = {};
    uint32_t trace_n_    = 0;
};

// --- Плата: подъём, образы в дисководах, журнал ---

// Образ дисковода; объект и его буферы - в PSRAM.
struct BdiFile {
    MediaDisk* image = nullptr;
    char name[16]    = {};
};
extern BdiFile g_bdi_files[kDrives];

// Ядро поднятого Beta Disk, иначе nullptr. Живёт в банках BDI.
extern BdiHost* g_bdi_host;

// take(bytes) - блок PSRAM или nullptr. false - PSRAM не хватило.
using PsramTakeFn = uint8_t* (*)(uint32_t bytes);
bool bdi_reserve(PsramTakeFn take);

// mem - свободная память банков под BdiHost. false - не поднялся.
bool bdi_start(uint8_t* mem, size_t bytes);

// Цикл Core1 с носителем: записи в файлы, заказанные цилиндры и новый
// запрос навигатора со страницы обмена. Без поднятого Beta Disk - ничего.
void bdi_poll();

// Строка line журнала "bdi: ..."; false - строки нет или Beta Disk не поднят.
// Во флеше: зовёт её цикл журнала, когда банков BDI может и не быть.
bool bdi_log_line(int line, char* out, size_t cap);

// Машину сбросили: Beta Disk в исходное, как линией RESET. Без поднятого
// Beta Disk - ничего.
void bdi_machine_reset();

// Win+F3: образы A и B местами, если вставлены оба.
void bdi_swap_ab();

// Для навигатора (цикл Core1). full - путь с префиксом тома vol.

// Имя - образ TRD или SCL (по расширению).
bool bdi_image_name(const char* name);
// Образ в дисковод; прежний извлекается. false - не образ, не открылся или
// дисковод держит прежний.
bool bdi_mount(int drive, uint8_t vol, const char* full, bool read_only);
// false - дисковод ещё держит прежний образ.
bool bdi_eject(int drive);
// Имя образа в дисководе; пустая строка - пусто.
const char* bdi_drive_name(int drive);

// Диск TR-DOS из файлов тома vol: begin, add на каждый файл в порядке
// каталога, end (where - откуда, для журнала).
enum class SetAdd : uint8_t { Ok, NoFile, Fragments };
bool bdi_set_begin(int drive, uint8_t vol);
SetAdd bdi_set_add(const char* full, const char* name, uint32_t size);
void bdi_set_end(const char* where);
} // namespace devices::bdi
