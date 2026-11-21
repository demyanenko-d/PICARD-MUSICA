// SPDX-License-Identifier: MIT
#pragma once

// Beta Disk Пентагона 1024: ВГ93 на портах #1F/#3F/#5F/#7F, системный
// регистр #FF, четыре дисковода.
//
// Разводка как на плате Пентагона: HLD контроллера - его же READY, общий
// мотор и разрешение выбора дисковода; пока HLT (kSysHltBit) 0, вход /IP
// прижат к нулю (индекс активен).
//
// Время - такты CLK контроллера (1 МГц), у ядра - полупериоды. Все
// дисководы вращаются вместе, у каждого свой сдвиг угла. Потоки дорожек
// текущего цилиндра выбранного дисковода лежат в кэше владельца (две
// стороны по kTrackBytes); запись контроллера уходит в источник при смене
// цилиндра или дисковода и по flush.
//
// Источник может отдать цилиндр не сразу (образ на носителе): до готовности
// дорожка без потока, как у диска, который ещё не раскрутился; ВГ93 ищет
// заголовки до пяти оборотов. Готовность сообщает владелец (source_ready).

#include <cstdint>

#include "slot_drive.h"
#include "devices/bdi/vg93.h"
#include "vg93w.h"

namespace devices::bdi {

// Образ глазами дисковода.
class DiskSource {
public:
    // Потоки сторон из mask (разряд на сторону) в cells. false - ещё не
    // готов: источник сам заказал чтение, о готовности скажут source_ready.
    virtual bool cylinder(int cyl, uint8_t* const cells[2], unsigned mask) = 0;
    // Контроллер записал сторону. false - записанное некуда деть.
    virtual bool written(int cyl, int side, const uint8_t* cells) = 0;
    virtual bool write_protect() const                            = 0;

protected:
    ~DiskSource() = default;
};

class BetaDisk {
public:
    // cache - 2 * kTrackBytes байт.
    void init(uint8_t* cache);
    // Сброс машины: #FF = 0, контроллер в сбросе до записи разряда 2.
    void reset();

    // Модель до такта clk.
    void advance(uint64_t clk);
    uint64_t now() const { return vg93_.clk(); }

    // reg 0..3 - регистры ВГ93 (#1F, #3F, #5F, #7F).
    uint8_t read_reg(unsigned reg);
    void write_reg(unsigned reg, uint8_t v);
    uint8_t read_system() const { return static_cast<uint8_t>((vg93_.intrq ? kSysIntrqBit : 0) | (vg93_.drq ? kSysDrqBit : 0) | kSysOnesMask); }
    void write_system(uint8_t v);

    // Образ в дисковод.
    void insert(int drive, DiskSource* src);
    void eject(int drive);
    // Источник дисковода дочитал заказанное.
    void source_ready(int drive);
    // Образы двух дисководов местами; головки и угол остаются у дисководов.
    void swap(int a, int b);
    // Дорожки кэша - в источник. false - записанное некуда было деть.
    bool flush();
    DiskSource* source(int drive) const { return slot_[drive].src; }

    // Ядро с доведёнными ленивыми словами: регистры для осмотра снаружи.
    Vg93w& fdc() {
        vg93_.settle();
        return vg93_;
    }
    // Ядро как есть: время и ход по полупериодам.
    Vg93w& core() { return vg93_; }
    uint8_t system_reg() const { return sysreg_; }
    int cache_cyl() const { return cache_cyl_; }
    uint8_t cache_ready() const { return cache_ready_; }
    // Ожидания цилиндра от источника: сколько раз и самое долгое по часам
    // ядра, полупериоды.
    uint32_t source_waits() const { return waits_; }
    uint32_t source_wait_max() const { return wait_max_; }

private:
    struct Slot {
        DiskSource* src = nullptr;
        int cyl         = 0;
        uint32_t phase  = 0; // сдвиг угла относительно общего вращения
    };

    static uint8_t* track_for(void* user, SlotDrive& d, int cyl, int side, bool create);
    void select(int drive);
    void fill_cache();
    bool store_cache();
    void apply_drive();

    Vg93w vg93_;
    SlotDrive face_; // дисковод глазами контроллера: выбранный из четырёх
    Slot slot_[kDrives];
    uint8_t* cache_[2]   = {nullptr, nullptr};
    int cache_drive_     = -1;
    int cache_cyl_       = -1;
    uint8_t cache_ready_ = 0; // стороны кэша с потоком: от источника или записанные
    int cur_             = 0;
    uint8_t sysreg_      = 0;
    bool store_failed_   = false;
    bool waiting_        = false;
    uint32_t wait_from_  = 0;
    uint32_t waits_      = 0;
    uint32_t wait_max_   = 0;
};

} // namespace devices::bdi
