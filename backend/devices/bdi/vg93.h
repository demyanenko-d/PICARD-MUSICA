// SPDX-License-Identifier: MIT
#pragma once

// ВГ93 Beta Disk Пентагона автоматом по фазам, без микрокода. Диск
// вращается сам: байт p дорожки начинается в момент n * kRev + p * kByte,
// поэтому моменты DRQ, концов команд и импульсов индекса считаются
// формулой от угла, а поиск сектора идёт по таблице заголовков дорожки.
// Времена фаз сняты замером с Vg93w (модель кристалла по микрокоду).
//
// Разводка Пентагона: READY = HLD, дисковод выбран, пока HLD; HLT = 0
// прижимает /IP (индекс горит всегда). Только MFM.
//
// Время - полупериоды CLK (0.5 мкс). Обращения Z80 и смены автомата идут
// строго по времени: write/read в момент t не раньше прошлого и не позже
// due(); step() исполняет смену в момент due().

#include <cstdint>

#include "devices/bdi/drive.h"

namespace devices::bdi {

// --- Порты и разряды Beta Disk Пентагона со стороны Z80 ---

inline constexpr int kDrives = 4;

// Порты: регистры ВГ93 (номер регистра - A6:A5) и системный регистр.
inline constexpr uint8_t kPortCmd    = 0x1f; // запись - команда, чтение - статус
inline constexpr uint8_t kPortTrack  = 0x3f;
inline constexpr uint8_t kPortSector = 0x5f;
inline constexpr uint8_t kPortData   = 0x7f;
inline constexpr uint8_t kPortSystem = 0xff;

// Системный регистр, запись.
inline constexpr uint8_t kSysDriveMask = 0x03;
inline constexpr uint8_t kSysResetNBit = 0x04; // /MR: 0 - сброс
inline constexpr uint8_t kSysHltBit    = 0x08;
inline constexpr uint8_t kSysSideBit   = 0x10; // 0 - сторона 1
inline constexpr uint8_t kSysDdenNBit  = 0x40; // /DDEN: 0 - MFM
// Системный регистр, чтение: остальные разряды - 1.
inline constexpr uint8_t kSysIntrqBit = 0x80;
inline constexpr uint8_t kSysDrqBit   = 0x40;
inline constexpr uint8_t kSysOnesMask = 0x3f;

// Регистр статуса, команды типа II и III.
inline constexpr uint8_t kStBusyBit     = 0x01;
inline constexpr uint8_t kStDrqBit      = 0x02;
inline constexpr uint8_t kStLostBit     = 0x04;
inline constexpr uint8_t kStCrcBit      = 0x08;
inline constexpr uint8_t kStRnfBit      = 0x10;
inline constexpr uint8_t kStWrtFaultBit = 0x20; // у чтения - удалённые данные
inline constexpr uint8_t kStWprotBit    = 0x40;
inline constexpr uint8_t kStNotReadyBit = 0x80;
// Тип I.
inline constexpr uint8_t kStIndexBit   = 0x02;
inline constexpr uint8_t kStTr00Bit    = 0x04;
inline constexpr uint8_t kStSeekErrBit = 0x10;
inline constexpr uint8_t kStHeadBit    = 0x20;

// --- Автомат ---

inline constexpr uint64_t kByte  = 64; // байт MFM, 32 мкс
inline constexpr uint64_t kRev   = kTrackLen * kByte;
inline constexpr uint64_t kNever = ~uint64_t{0};

class Vg93 {
public:
    // Сброс машины: #FF = 0, контроллер в сбросе до записи разряда 2.
    void reset(uint64_t t);

    // reg 0..3 - регистры ВГ93, 4 - #FF.
    void write(unsigned reg, uint8_t v, uint64_t t);
    // Чтение с последствием: статус снимает INTRQ, данные - DRQ.
    void read(unsigned reg, uint64_t t);
    uint64_t due() const;
    void step();

    // Видимое в момент t (импульс индекса в статусе типа I - от угла).
    uint8_t status(uint64_t t) const;
    uint8_t reg(unsigned r) const { return r == 1 ? trk_ : r == 2 ? sec_ : dat_; }
    uint8_t sys() const { return static_cast<uint8_t>((intrq_ ? kSysIntrqBit : 0) | (drq_ ? kSysDrqBit : 0) | kSysOnesMask); }

    void insert(int drive, Disk* src, uint64_t t);
    void source_ready(int drive, uint64_t t);
    void swap(uint64_t t); // образы A и B местами

    // Журнал: концы команд II/III и их ошибки, ожидания цилиндра (полупериоды).
    struct Stats {
        uint32_t ends, lost, crc, rnf, waits, wait_max;
    };
    const Stats& stats() const { return st_; }
    uint8_t command() const { return cmd_; }
    int cylinder() const { return drv().cyl; }
    bool hld() const { return hld_; }

private:
    enum class Ph : uint8_t { Idle, Start, Cmp, Pulse, Search, Read, Next, WrFirst, WrCheck, Write, FmtCheck, Format, Force, End };

    Drive& drv() { return drv_[cur_]; } // выбранный дисковод
    const Drive& drv() const { return drv_[cur_]; }
    bool selected() const { return hld_ && !mr_; }
    bool spins() const { return (sys_ & kSysHltBit) != 0 && selected() && drv().disk != nullptr; }
    int side() const { return (sys_ & kSysSideBit) != 0 ? 0 : 1; }
    Track* track();
    void start(uint64_t t);
    void search(uint64_t s);
    void finish(uint64_t t, uint8_t flags);
    void end(uint64_t t);

    Drive drv_[kDrives] = {};
    int cur_            = 0;
    uint8_t sys_        = 0;
    bool mr_            = true;

    uint8_t cmd_ = 0x03, trk_ = 0, sec_ = 1, dat_ = 0;
    uint8_t flags_ = 0; // разряды статуса 2..6
    uint8_t fin_   = 0; // флаги конца команды
    bool t1_ = true, busy_ = false, intrq_ = false, drq_ = false, hld_ = false;
    bool int_index_ = false; // D4: INTRQ на каждом индексе
    bool int_now_   = false; // D8: INTRQ держится до записи другой команды
    int dir_        = 1;
    Ph ph_          = Ph::Idle;
    uint64_t now_   = 0;
    uint64_t due_   = kNever;
    uint64_t s_     = 0; // начало поиска
    uint64_t base_  = 0; // начало поля: первый A1 заголовка или индекс
    uint32_t pos_   = 0; // первый байт переноса на дорожке
    uint32_t k_ = 0, n_ = 0;
    uint16_t crc_      = 0;
    const TrackId* id_ = nullptr;
    uint64_t unload_   = kNever; // снятие HLD в простое
    uint64_t wr_t_     = kNever; // прошлая запись в регистр: защёлка DAL
    unsigned wr_reg_   = 0;
    Stats st_          = {};
};

} // namespace devices::bdi
