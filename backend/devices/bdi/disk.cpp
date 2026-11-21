// SPDX-License-Identifier: MIT
// Диск Beta Disk: дорожка MFM и образ на носителе. На плате выполняется из
// банков BDI. Дорожки строит и разбирает цикл Core1 (MediaDisk::service);
// автомат ВГ93 читает и пишет их на месте (cylinder, written).
#include "devices/bdi/disk.h"

#include <cstdio>
#include <cstring>

namespace devices::bdi {

// --- Дорожка MFM ---

namespace {

struct CrcTab {
    uint16_t v[256];
};
constexpr CrcTab make_crc_tab() {
    CrcTab t{};
    for (unsigned i = 0; i < 256; i++) {
        auto r = static_cast<uint16_t>(i << 8);
        for (int k = 0; k < 8; k++) {
            r = static_cast<uint16_t>((r << 1) ^ ((r & 0x8000) != 0 ? 0x1021u : 0));
        }
        t.v[i] = r;
    }
    return t;
}
constexpr CrcTab kCrc = make_crc_tab();

} // namespace

uint16_t crc16(uint16_t crc, const uint8_t* p, size_t n) {
    while (n-- > 0) {
        crc = static_cast<uint16_t>((crc << 8) ^ kCrc.v[(crc >> 8) ^ *p++]);
    }
    return crc;
}

namespace {

struct Writer {
    Track& t;
    uint32_t p;
    void put(uint8_t v, bool m = false) {
        if (p < kTrackLen) t.put(p++, v, m);
    }
    void run(uint8_t v, int n) {
        while (n-- > 0) {
            put(v);
        }
    }
    uint16_t field(uint16_t crc, const uint8_t* s, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) {
            put(s[i]);
        }
        return crc16(crc, s, n);
    }
    void crc(uint16_t v) {
        put(static_cast<uint8_t>(v >> 8));
        put(static_cast<uint8_t>(v));
    }
};

bool a1x3(const Track& t, uint32_t p) {
    for (uint32_t k = 0; k < 3; k++) {
        const uint32_t q = (p + k) % kTrackLen;
        if (!t.mark(q) || t.data[q] != 0xa1) return false;
    }
    return true;
}

uint16_t crc_at(const Track& t, uint32_t p, uint32_t n, uint16_t crc) {
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t v = t.at(p + k);
        crc             = crc16(crc, &v, 1);
    }
    return crc;
}

bool crc_ok(const Track& t, uint32_t mark, uint32_t n) {
    const uint16_t crc = crc_at(t, mark, n + 1, crc16(0xffff, kA1x3, 3));
    return crc == static_cast<uint16_t>((t.at(mark + n + 1) << 8) | t.at(mark + n + 2));
}

} // namespace

void track_build(Track& t, const SectorSpec* s, int count, int gap3) {
    std::memset(&t, 0, sizeof t);
    Writer w{t, 0};
    w.run(0x4e, 80);
    w.run(0x00, 12);
    for (int i = 0; i < 3; i++) {
        w.put(0xc2, true);
    }
    w.put(0xfc);
    w.run(0x4e, 50);
    for (int i = 0; i < count; i++) {
        const uint8_t id[4] = {s[i].c, s[i].h, s[i].r, s[i].n};
        w.run(0x00, 12);
        for (int k = 0; k < 3; k++) {
            w.put(0xa1, true);
        }
        uint8_t mark = 0xfe;
        uint16_t crc = w.field(crc16(0xffff, kA1x3, 3), &mark, 1);
        crc          = w.field(crc, id, 4);
        w.crc((s[i].bad_crc & kBadIdCrc) != 0 ? static_cast<uint16_t>(~crc) : crc);
        w.run(0x4e, 22);
        if (s[i].data != nullptr) {
            w.run(0x00, 12);
            for (int k = 0; k < 3; k++) {
                w.put(0xa1, true);
            }
            mark = s[i].deleted ? 0xf8 : 0xfb;
            crc  = w.field(crc16(0xffff, kA1x3, 3), &mark, 1);
            crc  = w.field(crc, s[i].data, sector_len(s[i].n));
            w.crc((s[i].bad_crc & kBadDataCrc) != 0 ? static_cast<uint16_t>(~crc) : crc);
        }
        w.run(0x4e, gap3);
    }
    w.run(0x4e, static_cast<int>(kTrackLen - w.p));
    track_index(t);
}

void track_index(Track& t) {
    t.nid = 0;
    for (uint32_t p = 0; p < kTrackLen && t.nid < kTrackIds; p++) {
        if (!a1x3(t, p) || (t.at(p + 3) & 0xfc) != 0xfc) continue;
        TrackId& d = t.id[t.nid++];
        d          = TrackId{static_cast<uint16_t>(p), kNoDam, t.at(p + 4), t.at(p + 5), t.at(p + 6), t.at(p + 7), 0, crc_ok(t, p + 3, 4), false};
        for (uint32_t q = p + 10 + 25; q <= p + 10 + 48; q++) {
            if ((t.at(q) & 0xfc) == 0xf8 && a1x3(t, q - 3)) {
                d.dam     = static_cast<uint16_t>(q % kTrackLen);
                d.mark    = t.at(q);
                d.data_ok = crc_ok(t, q, sector_len(d.n));
                break;
            }
        }
        p += 3;
    }
}

// --- Образ на носителе ---

// Заказ пишется и при совпадении: шаг внутрь заказал другой цилиндр, шаг
// обратно до прохода цикла его отменяет - иначе цикл построит заказанный
// поверх дорожек, на которых стоит дисковод.
Track* MediaDisk::cylinder(int cyl) {
    want_.store(cyl, std::memory_order_release);
    return cyl_.load(std::memory_order_acquire) == cyl ? b_->tracks : nullptr;
}

// Записанная сторона - уже в дорожке цилиндра; в сектора её разбирает цикл.
void MediaDisk::written(int cyl, int side) {
    if (ro_ || cyl_.load(std::memory_order_acquire) != cyl) return;
    writes_[side].store(writes_[side].load(std::memory_order_relaxed) + 1u, std::memory_order_release);
}

namespace {

constexpr int kGap3Trd   = 54; // 16 секторов по 256 Б в оборот MFM
constexpr uint32_t kNone = 0xffffffffu;

// Каталог TR-DOS: запись о файле и служебный сектор 9 (дорожка 0).
constexpr uint32_t kDirEntryBytes  = 16;
constexpr uint32_t kFilesPerDirSec = 16;   // kSecBytes / kDirEntryBytes
constexpr uint32_t kEntryBytes     = 14;   // имя 8, тип, начало 2, длина 2, секторов; дальше - место файла
constexpr uint32_t kInfoFreeSec    = 0xe1; // первый свободный: сектор, дорожка
constexpr uint32_t kInfoFreeTrk    = 0xe2;
constexpr uint32_t kInfoDiskType   = 0xe3; // 0x16 - 80 цилиндров, две стороны
constexpr uint32_t kInfoFiles      = 0xe4;
constexpr uint32_t kInfoFreeSecs   = 0xe5; // два байта
constexpr uint32_t kInfoTrdosId    = 0xe7; // 0x10 - признак TR-DOS
constexpr uint32_t kInfoUnused     = 0xea; // 9 пробелов
constexpr uint32_t kInfoLabel      = 0xf5; // 8 знаков

// Знак имени TR-DOS: вне ASCII - '_'.
uint8_t name_char(char c) {
    return c >= 0x20 && c < 0x7f ? static_cast<uint8_t>(c) : '_';
}

} // namespace

// Буферы из арены и чистое состояние; dir - каталог в памяти (SCL, набор).
// false - памяти нет.
bool MediaDisk::setup(const hal::SectorIo& io, bool read_only, Arena& arena, bool dir) {
    io_ = io;
    ro_ = read_only;
    b_  = static_cast<Bufs*>(arena.take(sizeof(Bufs)));
    if (b_ == nullptr) return false;
    std::memset(b_->slot_of, 0, kMaxSecs);
    std::memset(cells_of_, 0, sizeof(cells_of_));
    slots_used_ = cells_used_ = 0;
    nruns_ = pooled_ = 0;
    files_ = used_ = 0;
    cyls_          = kCyls;
    cyl_.store(-1, std::memory_order_relaxed);
    want_.store(-1, std::memory_order_relaxed);
    for (int side = 0; side < 2; side++) {
        writes_[side].store(0, std::memory_order_relaxed);
        settled_[side] = 0;
    }
    static_assert(kDirSec * kSecBytes >= kSetFiles * kDirEntryBytes + kSecBytes, "the catalog fits the directory sectors");
    for (uint32_t v = 0; dir && v < kDirSec; v++) { // каталог: 8 секторов по 16 файлов и сектор 9
        slot(v, true);
    }
    return true;
}

bool MediaDisk::run(uint32_t first, uint32_t secs, uint32_t skip, uint32_t bytes, const MediaExtent* ext, int extents) {
    if (nruns_ == static_cast<int>(kSetFiles) || pooled_ + extents > kPool) return false;
    b_->runs[nruns_++] = Run{first, secs, skip, bytes, static_cast<uint16_t>(pooled_), static_cast<uint16_t>(extents)};
    std::memcpy(b_->pool + pooled_, ext, sizeof(MediaExtent) * static_cast<size_t>(extents));
    pooled_ += extents;
    return true;
}

// Запись каталога в памяти: 14 байт и место файла - сразу за предыдущим.
// Имя и тип (9 байт) уже есть в каталоге диска.
bool MediaDisk::name_taken(const uint8_t* e) {
    for (uint32_t i = 0; i < files_; i++) {
        if (std::memcmp(slot(i / kFilesPerDirSec, false) + (i % kFilesPerDirSec) * kDirEntryBytes, e, 9) == 0) return true;
    }
    return false;
}

// TR-DOS открывает файл по имени и типу, первый совпавший: длинные имена,
// обрезанные до 8 знаков, сливаются. Повтор получает счётчик в хвосте
// имени: Karbofos, Karbofo1, Karbofo2, ... Karbof10.
void MediaDisk::entry(const uint8_t* src) {
    uint8_t e[kEntryBytes];
    std::memcpy(e, src, kEntryBytes);
    size_t stem = 8;
    while (stem > 0 && e[stem - 1] == ' ') {
        stem--;
    }
    for (unsigned n = 1; n < 1000 && name_taken(e); n++) {
        char num[4];
        const size_t digits = static_cast<size_t>(std::snprintf(num, sizeof(num), "%u", n));
        const size_t at     = stem < 8 - digits ? stem : 8 - digits;
        std::memcpy(e, src, 8);
        std::memcpy(e + at, num, digits);
    }
    uint8_t* const d  = slot(files_ / kFilesPerDirSec, false) + (files_ % kFilesPerDirSec) * kDirEntryBytes;
    const uint32_t at = kSecPerTrack + used_;
    std::memcpy(d, e, kEntryBytes);
    d[14]  = static_cast<uint8_t>(at % kSecPerTrack);
    d[15]  = static_cast<uint8_t>(at / kSecPerTrack);
    used_ += e[13];
    files_++;
}

void MediaDisk::seal() {
    const uint32_t end = kSecPerTrack + used_; // первый свободный сектор диска
    if (end > static_cast<uint32_t>(kCyls) * kCylSecs) cyls_ = static_cast<int>((end + kCylSecs - 1) / kCylSecs);
    const uint32_t free_secs = static_cast<uint32_t>(cyls_) * kCylSecs - end;
    uint8_t* const info      = slot(kDirSec - 1, false); // дорожка 0, сектор 9
    info[kInfoFreeSec]       = static_cast<uint8_t>(end % kSecPerTrack);
    info[kInfoFreeTrk]       = static_cast<uint8_t>(end / kSecPerTrack);
    info[kInfoDiskType]      = 0x16;
    info[kInfoFiles]         = static_cast<uint8_t>(files_);
    info[kInfoFreeSecs]      = static_cast<uint8_t>(free_secs);
    info[kInfoFreeSecs + 1]  = static_cast<uint8_t>(free_secs >> 8);
    info[kInfoTrdosId]       = 0x10;
    std::memset(info + kInfoUnused, ' ', 9);
    std::memset(info + kInfoLabel, ' ', 8);
}

bool MediaDisk::open(ImageFormat fmt, const hal::SectorIo& io, const MediaExtent* ext, int extents, uint32_t size, bool read_only, Arena& arena) {
    if ((fmt != ImageFormat::Trd && fmt != ImageFormat::Scl) || extents <= 0 || extents > kMaxExtents) return false;
    if (!setup(io, read_only, arena, fmt == ImageFormat::Scl)) return false;
    if (fmt == ImageFormat::Trd) {
        const uint32_t secs = size / kSecBytes;
        if (size == 0 || size % kSecBytes != 0 || secs > kMaxSecs) return false;
        if (secs > static_cast<uint32_t>(kCyls) * kCylSecs) cyls_ = static_cast<int>((secs + kCylSecs - 1) / kCylSecs);
        return run(0, secs, 0, size, ext, extents);
    }
    // SCL: заголовок "SINCLAIR", число файлов, по 14 байт на файл (имя 8,
    // тип, параметры 4, длина в секторах), затем данные подряд.
    run(kSecPerTrack, 0, 0, size, ext, extents);
    Run& r = b_->runs[0];
    uint8_t head[9];
    if (size < 9 || !file_bytes(r, 0, head, 9, false) || std::memcmp(head, "SINCLAIR", 8) != 0 || head[8] > kSetFiles) return false;
    for (uint32_t i = 0; i < head[8]; i++) {
        uint8_t e[kEntryBytes];
        if (!file_bytes(r, 9 + kEntryBytes * i, e, kEntryBytes, false)) return false;
        entry(e);
    }
    r.skip  = 9 + kEntryBytes * head[8];
    r.secs  = used_;
    r.bytes = used_ * kSecBytes;
    if (r.skip + r.bytes > size || used_ > kSetSectors) return false;
    seal();
    return true;
}

bool MediaDisk::open_set(const hal::SectorIo& io, Arena& arena) {
    return setup(io, true, arena, true);
}

bool MediaDisk::add(const char* name, uint32_t size, const uint8_t* head, const MediaExtent* ext, int extents) {
    const bool hob      = hobeta(name, size);
    const uint32_t skip = hob ? kHobetaHead : 0;
    const uint32_t secs = set_sectors(name, size);
    if (secs == 0 || secs > kFileSectors || used_ + secs > kSetSectors || (hob && head == nullptr)) return false;
    if (!run(kSecPerTrack + used_, secs, skip, size - skip, ext, extents)) return false;
    uint8_t e[kEntryBytes] = {};
    if (hob) {
        std::memcpy(e, head, 13);
    } else {
        const char* const dot = std::strrchr(name, '.');
        const size_t stem     = dot != nullptr ? static_cast<size_t>(dot - name) : std::strlen(name);
        for (size_t i = 0; i < 8; i++) {
            e[i] = i < stem ? name_char(name[i]) : ' ';
        }
        e[8]  = dot != nullptr && dot[1] != 0 ? name_char(dot[1]) : 'C';
        e[11] = static_cast<uint8_t>(size);
        e[12] = static_cast<uint8_t>(size >> 8);
    }
    e[13] = static_cast<uint8_t>(secs);
    entry(e);
    return true;
}

bool MediaDisk::file_bytes(const Run& r, uint32_t off, uint8_t* buf, uint32_t len, bool write) {
    while (len != 0) {
        uint32_t fs = off / hal::kSectorBytes, lba = kNone;
        for (int i = 0; i < r.extents && lba == kNone; i++) {
            const MediaExtent& e = b_->pool[r.ext + i];
            if (fs < e.sectors) {
                lba = e.lba + fs;
            } else {
                fs -= e.sectors;
            }
        }
        const uint32_t in = off % hal::kSectorBytes;
        const uint32_t n  = len < hal::kSectorBytes - in ? len : hal::kSectorBytes - in;
        if (lba == kNone || ((!write || n < hal::kSectorBytes) && !io_.read(io_.user, lba, b_->sector))) return false;
        if (write) {
            std::memcpy(b_->sector + in, buf, n);
            if (!io_.write(io_.user, lba, b_->sector)) return false;
        } else {
            std::memcpy(buf, b_->sector + in, n);
        }
        off += n;
        buf += n;
        len -= n;
    }
    return true;
}

// Сектор v из файла; чего в файлах нет и что не прочиталось - нули.
// Сектор целиком внутри файла образа: запись за конец короткого TRD файл
// не дописывает: машина получает ошибку записи.
bool MediaDisk::stores(int cyl, int side, uint8_t r) const {
    if (r == 0 || r > kSecPerTrack) return true;
    const uint32_t v   = disk_sector(cyl, side, r);
    const Run* const u = run_of(v);
    return u != nullptr && (v - u->first + 1u) * kSecBytes <= u->bytes;
}

// false - носитель не отдал сектор: машина получит его с ошибкой CRC, а не
// нули под видом данных.
bool MediaDisk::read_sector(uint32_t v, uint8_t* dst) {
    std::memset(dst, 0, kSecBytes);
    const Run* const r = run_of(v);
    const uint32_t at  = r != nullptr ? (v - r->first) * kSecBytes : 0;
    if (r == nullptr || at >= r->bytes) return true;
    const uint32_t n = r->bytes - at < kSecBytes ? r->bytes - at : kSecBytes;
    if (file_bytes(*r, r->skip + at, dst, n, false)) return true;
    std::memset(dst, 0, kSecBytes);
    read_errors_++;
    return false;
}

bool MediaDisk::service() {
    const int cyl  = cyl_.load(std::memory_order_relaxed);
    const int want = want_.load(std::memory_order_acquire);
    bool next      = want >= 0 && want != cyl;
    // Дорожки уходят под новый цилиндр: без цилиндра поток в них не пишет,
    // а записанное до этого разбирается ниже. Поток успел вернуться на
    // прежний цилиндр между чтением заказа и этой записью - дорожки
    // остаются ему; true - разбудить, если он уже ждёт.
    bool back = false;
    if (next) {
        cyl_.store(-1, std::memory_order_release);
        if (want_.load(std::memory_order_acquire) == cyl) {
            cyl_.store(cyl, std::memory_order_release);
            next = false;
            back = true;
        }
    }
    settle(cyl);
    uint32_t t = q_tail_.load(std::memory_order_relaxed);
    for (; t != q_head_.load(std::memory_order_acquire); q_tail_.store(++t, std::memory_order_release)) {
        const Pending& p   = b_->queue[t % kQueue];
        const Run* const r = run_of(p.v);
        if (r == nullptr || !file_bytes(*r, r->skip + (p.v - r->first) * kSecBytes, const_cast<uint8_t*>(p.data), kSecBytes, true)) write_errors_++;
    }
    if (!next) return back;
    // Очередь пуста: всё записанное уже в файле. За краем образа -
    // неразмеченные дорожки.
    if (want >= cyls_) {
        for (int side = 0; side < 2; side++) {
            track_build(b_->tracks[side], nullptr, 0, 0);
        }
        cyl_.store(want, std::memory_order_release);
        return true;
    }
    uint32_t bad = 0;
    for (int side = 0; side < 2; side++) {
        for (unsigned r = 1; r <= kSecPerTrack; r++) {
            if (!read_sector(disk_sector(want, side, r), stage_sector(side, r))) bad |= 1u << (side * kSecPerTrack + r - 1u);
        }
    }
    build(want, bad);
    cyl_.store(want, std::memory_order_release);
    return true;
}

// Записанные автоматом стороны цилиндра cyl - в сектора. Запись посреди
// разбора меняет счётчик, и сторона разбирается заново.
void MediaDisk::settle(int cyl) {
    for (int side = 0; side < 2 && cyl >= 0; side++) {
        for (uint32_t w; (w = writes_[side].load(std::memory_order_acquire)) != settled_[side];) {
            apply(cyl, side);
            if (writes_[side].load(std::memory_order_acquire) == w) settled_[side] = w;
        }
    }
}

// Дорожка по разметке TR-DOS - 16 разных секторов 1..16 по 256 Б этого
// цилиндра и стороны, все CRC верны, метки данных обычные: сектора - в файл
// через очередь, без места в файле - в ячейки, и в stage. Иначе - копия
// дорожки в cells.
void MediaDisk::apply(int cyl, int side) {
    const int t     = cyl * 2 + side;
    const Track& tr = b_->tracks[side];
    bool layout     = tr.nid == kSecPerTrack;
    uint32_t seen   = 0;
    for (int k = 0; k < tr.nid && layout; k++) {
        const TrackId& f = tr.id[k];
        layout = f.c == cyl && f.h == side && f.n == 1 && f.r >= 1 && f.r <= kSecPerTrack && f.id_ok && f.dam != kNoDam && f.data_ok && f.mark >= 0xfa &&
                 (seen & (1u << f.r)) == 0;
        seen |= 1u << f.r;
    }
    if (!layout) {
        if (cells_of_[t] == 0) {
            if (cells_used_ == kCellTracks) {
                write_errors_++;
                return;
            }
            cells_of_[t] = ++cells_used_;
        }
        std::memcpy(&b_->cells[cells_of_[t] - 1u], &tr, sizeof(Track));
        return;
    }
    cells_of_[t] = 0; // копия дорожки больше не нужна; место в запасе не возвращается
    for (unsigned k = 0; k < kSecPerTrack; k++) {
        const TrackId& f   = tr.id[k];
        const uint32_t v   = disk_sector(cyl, side, f.r);
        uint8_t* const src = stage_sector(side, f.r);
        for (uint32_t i = 0; i < kSecBytes; i++) {
            src[i] = tr.at(f.dam + 1u + i);
        }
        // Ячейка есть - в ячейку; места нет ни в файле, ни в ячейках -
        // ошибка; иначе - в очередь записи.
        const bool in_file = run_of(v) != nullptr;
        uint8_t* const s   = slot(v, !in_file);
        if (s != nullptr) {
            std::memcpy(s, src, kSecBytes);
            continue;
        }
        if (!in_file) {
            write_errors_++;
            continue;
        }
        const uint32_t h = q_head_.load(std::memory_order_relaxed);
        if (h - q_tail_.load(std::memory_order_acquire) == kQueue) {
            write_errors_++;
            continue;
        }
        Pending& p = b_->queue[h % kQueue];
        p.v        = static_cast<uint16_t>(v);
        std::memcpy(p.data, src, kSecBytes);
        q_head_.store(h + 1, std::memory_order_release);
    }
}

// Дорожки цилиндра из stage и ячеек; сторона, записанная не по разметке, -
// своей копией.
// bad - сектора, не прочитанные с носителя: бит side * 16 + r - 1.
void MediaDisk::build(int cyl, uint32_t bad) {
    for (int side = 0; side < 2; side++) {
        const int t = cyl * 2 + side;
        if (cells_of_[t] != 0) {
            std::memcpy(&b_->tracks[side], &b_->cells[cells_of_[t] - 1u], sizeof(Track));
            continue;
        }
        SectorSpec spec[kSecPerTrack];
        // Без развёртки: смещения в Bufs постоянны, и GCC разворачивал цикл в 1 КБ.
#pragma GCC unroll 1
        for (unsigned i = 0; i < kSecPerTrack; i++) {
            const uint8_t r   = kTrdosOrder[i];
            const uint32_t v  = disk_sector(cyl, side, r);
            const uint8_t* s  = slot(v, false);
            const uint8_t crc = (bad >> (side * kSecPerTrack + r - 1u)) & 1u ? kBadDataCrc : uint8_t{0};
            spec[i]           = SectorSpec{static_cast<uint8_t>(cyl), static_cast<uint8_t>(side), r, 1, false, s != nullptr ? s : stage_sector(side, r), crc};
        }
        track_build(b_->tracks[side], spec, kSecPerTrack, kGap3Trd);
    }
}
} // namespace devices::bdi
