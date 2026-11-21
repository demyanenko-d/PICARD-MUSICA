// SPDX-License-Identifier: MIT
#pragma once

// Диск Beta Disk. Код - в банках BDI.
//
// Дорожка MFM для ВГ93: байты по 32 мкс от импульса индекса и разряд метки
// на байт (A1 и C2 без синхроимпульса). Заголовки собраны таблицей: поиску
// сектора не нужно идти по байтам.
//
// Набор файлов носителя как диск TR-DOS: пределы каталога и диска, место
// файла на диске. Общее у навигатора (что можно отметить) и образа (сборка).
//
// Образ TRD или SCL на носителе (MediaDisk), в память целиком не грузится: цилиндр
// (32 сектора) читается по заказу дисковода, запись идёт в те же байты
// файла, FAT и длина не меняются. Третий вид - набор файлов носителя как
// диск TR-DOS только для чтения (open_set, add, seal).
//
// Сектор v = (цилиндр * 2 + сторона) * 16 + (номер - 1). Данные диска -
// участки (Run): сектора подряд из одного файла. У TRD участок один, с
// v = 0; у SCL один, с v = 16; у набора - по участку на файл. Каталог SCL и
// набора собирается в памяти. Чему нет места в файле (каталог, хвост
// короткого TRD, дорожка не по разметке) - в памяти до извлечения.
//
// Цилиндров 80; у диска длиннее 640 КБ - сколько занято, до 128: номер
// дорожки в каталоге TR-DOS - байт, 256 дорожек по 16 секторов - 1 МБ.
//
// Дорожки строит и разбирает цикл Core1 (service); автомат ВГ93 -
// прерывание на том же ядре - читает и пишет их на месте (cylinder,
// written, их код в банках BDI). У каждого общего слова один писатель:
// объект в PSRAM, CAS там не завершается.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "devices/hal/media.h"

namespace devices::bdi {

// --- Набор файлов TR-DOS ---

inline constexpr uint32_t kSetFiles    = 128;           // каталог: 8 секторов по 16 записей
inline constexpr uint32_t kFileSectors = 255;           // длина файла в записи каталога - байт
inline constexpr uint32_t kSetSectors  = 256 * 16 - 16; // номер дорожки в записи - байт; дорожка 0 - каталог
inline constexpr uint32_t kHobetaHead  = 17;            // заголовок Hobeta: запись каталога и сумма

// Hobeta - по расширению: первый знак '$'.
inline bool hobeta(const char* name, uint32_t size) {
    const char* const dot = std::strrchr(name, '.');
    return dot != nullptr && dot[1] == '$' && size >= kHobetaHead;
}

// Секторов файла на диске; заголовок Hobeta на диск не идёт.
inline uint32_t set_sectors(const char* name, uint32_t size) {
    if (hobeta(name, size)) size -= kHobetaHead;
    return size / 256u + (size % 256u != 0 ? 1u : 0u);
}

// --- Дорожка MFM ---

// CRC-16 CCITT (полином 0x1021), с начальным значением crc.
uint16_t crc16(uint16_t crc, const uint8_t* p, size_t n);
// Три A1 перед меткой поля: CRC поля считается с них. Без inline: копия в
// каждой единице трансляции, у кода банков BDI - в банках, а не во флеше.
constexpr uint8_t kA1x3[3] = {0xa1, 0xa1, 0xa1};

inline constexpr uint8_t kBadIdCrc   = 1; // в поле ID записан инвертированный CRC
inline constexpr uint8_t kBadDataCrc = 2; // в поле данных записан инвертированный CRC

struct SectorSpec {
    uint8_t c, h, r, n;
    bool deleted;        // метка данных F8 вместо FB
    const uint8_t* data; // 128 << (n & 3) байт; nullptr - заголовок без поля данных
    uint8_t bad_crc;     // kBadIdCrc | kBadDataCrc
};

inline constexpr uint32_t kTrackLen = 6250; // байтов за оборот при 300 об/мин
inline constexpr int kTrackIds      = 32;   // заголовков с запасом на мусор
inline constexpr uint16_t kNoDam    = 0xffff;

struct TrackId {
    uint16_t a1;  // первый A1 заголовка, байт от индекса
    uint16_t dam; // метка данных F8..FB; kNoDam - поля данных нет
    uint8_t c, h, r, n;
    uint8_t mark; // байт метки данных
    bool id_ok, data_ok;
};

struct Track {
    uint8_t data[kTrackLen];
    uint8_t marks[(kTrackLen + 7) / 8];
    uint8_t nid;
    TrackId id[kTrackIds];

    bool mark(uint32_t p) const { return ((marks[p >> 3] >> (p & 7)) & 1u) != 0; }
    void put(uint32_t p, uint8_t v, bool m) {
        data[p] = v;
        if (m) {
            marks[p >> 3] = static_cast<uint8_t>(marks[p >> 3] | (1u << (p & 7)));
        } else {
            marks[p >> 3] = static_cast<uint8_t>(marks[p >> 3] & ~(1u << (p & 7)));
        }
    }
    uint8_t at(uint32_t p) const { return data[p % kTrackLen]; }
};

// Дорожка IBM: GAP4a 80 x 4E, 12 x 00, 3 x C2, FC, GAP1 50 x 4E; на сектор
// 12 x 00, 3 x A1, FE, C H R N, CRC, GAP2 22 x 4E, 12 x 00, 3 x A1, FB/F8,
// данные, CRC, GAP3 gap3 x 4E; хвост 4E. A1 и C2 - с разрядом метки. И
// таблица заголовков.
void track_build(Track& t, const SectorSpec* s, int count, int gap3);
// Таблица заголовков по байтам и меткам: три A1-метки и FC..FF, поле
// данных - три A1-метки и F8..FB через 25..48 байтов после CRC заголовка.
void track_index(Track& t);
// Длина данных сектора по байту N.
inline uint32_t sector_len(uint8_t n) {
    return 128u << (n & 3u);
}

// --- Диск в дисководе ---

// Диск в дисководе: дорожки цилиндра лежат у него, контроллер читает и
// пишет их на месте. На плате - образ на носителе (MediaDisk), на ПК в
// тестах - образы ПК (tests/vg93_ref).
class Disk {
public:
    // Две дорожки цилиндра (стороны 0 и 1) или nullptr - ещё не готовы:
    // источник сам заказал чтение, о готовности скажет source_ready.
    virtual Track* cylinder(int cyl) = 0;
    // Контроллер записал сторону цилиндра.
    virtual void written(int cyl, int side) = 0;
    virtual bool write_protect() const      = 0;
    // Сектор r стороны side цилиндра cyl есть где хранить: у образа на
    // носителе за концом короткого файла - нет.
    virtual bool stores(int /*cyl*/, int /*side*/, uint8_t /*r*/) const { return true; }
    // Цилиндров у образа: головка идёт до его последнего, если он дальше
    // упора дисковода.
    virtual int cylinders() const { return 0; }

protected:
    ~Disk() = default;
};

// --- Образ на носителе ---

// Форматы образов. На плате открываются TRD и SCL; остальные разбирает
// модель на ПК (tests/vg93_ref/disk_image).
enum class ImageFormat : uint8_t { Unknown, Trd, Scl, Fdi, Udi, Td0, Hfe };

// Порядок секторов TR-DOS на дорожке: 1, 9, 2, 10, ...
inline constexpr uint8_t kTrdosOrder[16] = {1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15, 8, 16};

// Выделение подряд из области владельца; освобождения нет, область
// сбрасывается вместе с образом.
struct Arena {
    uint8_t* base = nullptr;
    size_t size   = 0;
    size_t used   = 0;
    void* take(size_t n) {
        const size_t at = (used + 3) & ~static_cast<size_t>(3);
        if (base == nullptr || at + n > size) return nullptr;
        used = at + n;
        return base + at;
    }
};

// Участок файла - подряд идущие сектора носителя.
struct MediaExtent {
    uint32_t lba, sectors;
};

class MediaDisk final : public Disk {
public:
    static constexpr int kMaxExtents = 32; // участков носителя у одного файла

    // Память буферов - из arena (на плате PSRAM), kArenaBytes, 80 КБ. false - не
    // TRD/SCL, файл испорчен или памяти нет.
    bool open(ImageFormat fmt, const hal::SectorIo& io, const MediaExtent* ext, int extents, uint32_t size, bool read_only, Arena& arena);

    // Набор файлов: open_set, add на каждый файл в порядке каталога, seal.
    bool open_set(const hal::SectorIo& io, Arena& arena);
    // name - имя на носителе: 8 знаков до точки и первый знак расширения -
    // имя и тип TR-DOS. У Hobeta запись каталога - из head, первых
    // kHobetaHead байт файла. false - не помещается: каталог, диск, участки.
    bool add(const char* name, uint32_t size, const uint8_t* head, const MediaExtent* ext, int extents);
    // Каталог SCL и набора собран: служебный сектор и число цилиндров.
    void seal();

    // Цикл Core1: разбор записанных сторон, записи в файл, затем заказанный
    // цилиндр и его дорожки. true - цилиндр готов, дисководу пора сказать
    // source_ready.
    bool service();

    Track* cylinder(int cyl) override;
    void written(int cyl, int side) override;
    bool write_protect() const override { return ro_; }
    bool stores(int cyl, int side, uint8_t r) const override;
    int cylinders() const override { return cyls_; }

    // Сбоев записи в файл с открытия.
    uint32_t write_errors() const { return write_errors_; }
    uint32_t read_errors() const { return read_errors_; }
    // Записанное разобрано и дописано в файл: образ можно закрыть.
    bool idle() const {
        return writes_[0].load(std::memory_order_acquire) == settled_[0] && writes_[1].load(std::memory_order_acquire) == settled_[1] &&
               q_head_.load(std::memory_order_acquire) == q_tail_.load(std::memory_order_acquire);
    }

private:
    // Разметка TR-DOS.
    static constexpr uint32_t kSecBytes    = 256;
    static constexpr uint32_t kSecPerTrack = 16;
    static constexpr uint32_t kCylSecs     = 2 * kSecPerTrack;
    static constexpr int kCyls             = 80;
    static constexpr int kMaxCyls          = 128;
    static constexpr uint32_t kMaxSecs     = kMaxCyls * kCylSecs;
    static_assert(kSetSectors == kMaxSecs - kSecPerTrack, "the set fills the largest disk");

    static constexpr int kQueue       = 64;  // секторов в очереди записи
    static constexpr int kSlots       = 64;  // секторов без места в файле
    static constexpr int kCellTracks  = 2;   // дорожек не по разметке
    static constexpr uint32_t kDirSec = 9;   // каталог в памяти: дорожка 0, сектора 1..9
    static constexpr int kPool        = 512; // участков носителя на все файлы набора

    // Сектора диска подряд из одного файла: с байта skip, bytes байт; за
    // концом файла - нули.
    struct Run {
        uint32_t first, secs, skip, bytes;
        uint16_t ext, extents; // участки носителя в pool
    };

    struct Pending {
        uint16_t v;
        uint8_t data[256];
    };

    // Буферы образа - один кусок арены.
    struct Bufs {
        uint8_t stage[kCylSecs * kSecBytes]; // сектора цилиндра cyl_
        Track tracks[2];                     // его дорожки (стороны 0 и 1), автомат пишет в них
        uint8_t sector[hal::kSectorBytes];   // сектор носителя под чтение-запись
        uint8_t slot_of[kMaxSecs];           // номер ячейки + 1, 0 - нет
        uint8_t slots[kSlots * kSecBytes];   // сектора без места в файле
        Track cells[kCellTracks];            // дорожки не по разметке
        Pending queue[kQueue];               // очередь записи
        Run runs[kSetFiles];
        MediaExtent pool[kPool];
    };

public:
    static constexpr size_t kArenaBytes = sizeof(Bufs);

private:
    bool setup(const hal::SectorIo& io, bool read_only, Arena& arena, bool dir);
    // Участок с сектором v или nullptr - в файлах его нет.
    const Run* run_of(uint32_t v) const {
        for (int i = 0; i < nruns_; i++) {
            if (v - b_->runs[i].first < b_->runs[i].secs) return &b_->runs[i];
        }
        return nullptr;
    }
    bool run(uint32_t first, uint32_t secs, uint32_t skip, uint32_t bytes, const MediaExtent* ext, int extents);
    void entry(const uint8_t* e);
    bool name_taken(const uint8_t* e);
    bool read_sector(uint32_t v, uint8_t* dst);
    // Сектор носителя: r-й (1..16) на дорожке cyl * 2 + side.
    static uint32_t disk_sector(int cyl, int side, unsigned r) {
        return (static_cast<uint32_t>(cyl) * 2u + static_cast<uint32_t>(side)) * kSecPerTrack + r - 1u;
    }
    // Его байты в stage (32 сектора заказанного цилиндра).
    uint8_t* stage_sector(int side, unsigned r) const { return b_->stage + (static_cast<uint32_t>(side) * kSecPerTrack + r - 1u) * kSecBytes; }
    bool file_bytes(const Run& r, uint32_t off, uint8_t* buf, uint32_t len, bool write);
    void settle(int cyl);
    void apply(int cyl, int side);
    void build(int cyl, uint32_t bad = 0);
    // Ячейка сектора без места в файле; create - завести, если нет.
    uint8_t* slot(uint32_t v, bool create) {
        if (b_->slot_of[v] != 0) return b_->slots + (b_->slot_of[v] - 1u) * kSecBytes;
        if (!create || slots_used_ == kSlots) return nullptr;
        b_->slot_of[v]   = ++slots_used_;
        uint8_t* const s = b_->slots + (slots_used_ - 1u) * kSecBytes;
        std::memset(s, 0, kSecBytes);
        return s;
    }

    hal::SectorIo io_{};
    Bufs* b_        = nullptr;
    bool ro_        = true;
    int cyls_       = kCyls;
    int nruns_      = 0;
    int pooled_     = 0;
    uint32_t files_ = 0; // каталог в памяти: записей и секторов файлов
    uint32_t used_  = 0;

    uint8_t slots_used_ = 0;
    uint8_t cells_of_[kMaxCyls * 2]{}; // дорожка не по разметке: номер копии + 1
    uint8_t cells_used_ = 0;
    std::atomic<uint32_t> q_head_{0}, q_tail_{0};
    std::atomic<int> cyl_{-1};             // цилиндр дорожек, -1 - нет; пишет цикл
    std::atomic<uint32_t> writes_[2] = {}; // записи сторон; пишет поток
    uint32_t settled_[2]             = {}; // из них разобрано циклом
    std::atomic<int> want_{-1};            // заказ; пишет поток
    uint32_t write_errors_ = 0;
    uint32_t read_errors_  = 0; // секторы, не прочитанные с носителя
};
} // namespace devices::bdi
