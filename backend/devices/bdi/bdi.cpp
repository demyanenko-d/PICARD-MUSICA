// SPDX-License-Identifier: MIT
// Beta Disk платы. Поток и обработчики шины выполняются из банков ОЗУ
// DivMMC 8-15 (SOUNDSINTH_BDI_CODE), как ВГ93, дисковод и диск (библиотека
// soundsinth_bdi): на пути шины флеша быть не должно. Остальное - во флеше:
// сюда приходят и до заполнения банков, и без поднятого Beta Disk.
//
// Образы - на Core1 в основном витке (bdi_poll, вызовы навигатора) и при
// загрузке на Core0 (bdi_reserve); носитель отсюда читается без рекурсии в
// его драйвер. Тома FatFs подключает навигатор.
#include "devices/bdi/bdi.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "ff.h"

#include "platform/compiler.h"
#include "platform/log.h"
#include "platform/mono_time.h"

#include "devices/hal/bus_worker.h"
#include "devices/hal/media.h"
#include "devices/hal/z80_ports.h"
#include "devices/storage/partition.h"
#include "devices/storage/storage.h"
#include "devices/storage/volumes.h"

namespace devices::bdi {

BdiFile g_bdi_files[kDrives];
BdiHost* g_bdi_host = nullptr;

// --- Поток ---

namespace {

// DRQ снят: разряд в #FF и в статусе (разряд 1) - у команд II/III DRQ и
// есть этот разряд.
inline void drop_drq(uint8_t* cells) {
    cells[4] = static_cast<uint8_t>(cells[4] & ~kSysDrqBit);
    cells[0] = static_cast<uint8_t>(cells[0] & ~kStDrqBit);
}

} // namespace

SOUNDSINTH_BDI_CODE("bdi_init") void BdiHost::init(uint64_t now, hal::ClockFn clock) {
    clock_ = clock;
    off_   = now;
    last_  = 0;
    head_.store(0, std::memory_order_relaxed);
    tail_ = 0;
    for (Ev& e : ring_) {
        e.ready = 0;
    }
    st_      = {};
    trace_n_ = 0;
    frozen_  = false;
    vg93_    = Vg93{};
    vg93_.reset(0);
    publish(0);
}

// Чтения - только с последствием, и им не отдаются последние kReserve мест:
// поток, занятый разбором, иначе терял бы запись команды. Принятое событие
// будит поток.
SOUNDSINTH_BDI_CODE("bdi_push") void BdiHost::push(uint8_t port, uint8_t v, uint32_t stamp, bool write) {
    const uint32_t room = write ? kRing : kRing - kReserve;
    uint32_t h          = head_.load(std::memory_order_relaxed);
    do {
        if (h - tail_ >= room) {
            st_.lost++;
            return;
        }
    } while (!head_.compare_exchange_weak(h, h + 1, std::memory_order_relaxed));
    Ev& e   = ring_[h % kRing];
    e.stamp = stamp;
    e.port  = port;
    e.v     = v;
    std::atomic_signal_fence(std::memory_order_release);
    e.ready = 1;
    hal::bus_worker_kick();
}

SOUNDSINTH_BDI_CODE("bdi_bus_write") void BdiHost::bus_write(uint8_t port, uint8_t v, uint32_t stamp) {
    if (port == kPortCmd) cells_[4] = static_cast<uint8_t>(cells_[4] & ~kSysIntrqBit);
    if (port == kPortData && (cells_[4] & kSysDrqBit) != 0) drop_drq(cells_);
    push(port, v, stamp);
}

SOUNDSINTH_BDI_CODE("bdi_bus_read_done") void BdiHost::bus_read_done(uint8_t port, uint32_t stamp) {
    if (port == kPortCmd && (cells_[4] & kSysIntrqBit) != 0) {
        cells_[4] = static_cast<uint8_t>(cells_[4] & ~kSysIntrqBit);
        push(kReadStatus, 0, stamp, false);
    } else if (port == kPortData && (cells_[4] & kSysDrqBit) != 0) {
        drop_drq(cells_);
        push(kReadData, 0, stamp, false);
    }
}

SOUNDSINTH_BDI_CODE("bdi_publish") void BdiHost::publish(uint64_t t) {
    cells_[0] = vg93_.status(t);
    cells_[1] = vg93_.reg(1);
    cells_[2] = vg93_.reg(2);
    cells_[3] = vg93_.reg(3);
    cells_[4] = vg93_.sys();
}

// Обращение - в момент диска по метке, но не раньше прошлого и не позже
// неопубликованной смены автомата.
SOUNDSINTH_BDI_CODE("bdi_apply") void BdiHost::apply(const Ev& e, uint64_t now) {
    const uint64_t at = now - static_cast<uint32_t>(static_cast<uint32_t>(now) - e.stamp);
    uint64_t t        = at - off_;
    const uint64_t d  = vg93_.due();
    if (t >= d) t = d - 1;
    if (t < last_) t = last_;
    last_ = t;
    st_.events++;
    switch (e.port) {
        case kReset:
            vg93_.reset(t);
            break;
        case kReady:
            vg93_.source_ready(e.v, t);
            break;
        case kSwap:
            vg93_.swap(t);
            break;
        case kInsert:
            vg93_.insert(e.v, pending_[e.v], t);
            inserted_.fetch_add(1, std::memory_order_release);
            break;
        case kReadStatus:
            vg93_.read(0, t);
            break;
        case kReadData:
            vg93_.read(3, t);
            reads_++;
            break;
        default:
            if (e.port == kPortCmd) reads_ = 0;
            vg93_.write(slot(e.port), e.v, t);
    }
}

SOUNDSINTH_BDI_CODE("bdi_end_trace") void BdiHost::end_trace() {
    const bool busy = vg93_.command() >= 0x80 && (vg93_.status(last_) & kStBusyBit) != 0;
    if (busy_ && !busy && (vg93_.command() & 0xf0) != 0xd0) {
        trace_[trace_n_++ % kTrace] = Trace{vg93_.command(), vg93_.status(last_), vg93_.reg(1), vg93_.reg(2), reads_};
    }
    busy_ = busy;
}

SOUNDSINTH_BDI_CODE("bdi_service") uint64_t BdiHost::service(uint64_t now) {
    st_.runs++;
    for (;;) {
        if (clock_ != nullptr) now = clock_();
        // Обращения до публикации - раньше смены.
        for (Ev* e; (e = &ring_[tail_ % kRing])->ready != 0;) {
            std::atomic_signal_fence(std::memory_order_acquire);
            if (static_cast<int32_t>(static_cast<uint32_t>(now) - e->stamp) < 0) break; // положено после взятия now
            apply(*e, now);
            e->ready = 0;
            tail_    = tail_ + 1;
        }
        // Под секцией - не пришло ли обращение до now: оно раньше смены. Метка
        // позже now - после неё, его разбор - следующим заходом (толчок уже дан).
        const uint32_t lock = hal::bus_worker_lock();
        const Ev& e         = ring_[tail_ % kRing];
        if (e.ready != 0 && static_cast<int32_t>(static_cast<uint32_t>(now) - e.stamp) >= 0) {
            hal::bus_worker_unlock(lock);
            continue;
        }
        const uint64_t d = vg93_.due();
        if (d == kNever || d + off_ > now) {
            publish(now - off_);
            hal::bus_worker_unlock(lock);
            end_trace();
            return d == kNever ? kNever : d + off_;
        }
        // Навигатор: смена ждёт выхода. Вышли - она через байт: Z80 успеет
        // дочитать байт, на котором его остановил NMI.
        if (paused_ != nullptr && paused_()) {
            if (!frozen_) st_.pauses++;
            frozen_ = true;
            hal::bus_worker_unlock(lock);
            return now + kPausePoll;
        }
        if (frozen_) {
            frozen_            = false;
            const uint64_t to  = now + kByte - d;
            st_.paused        += to - off_;
            off_               = to;
            hal::bus_worker_unlock(lock);
            continue;
        }
        const uint64_t late = now - (d + off_);
        if (late > st_.late_max) st_.late_max = static_cast<uint32_t>(late);
        if (late > 16 * kHalfPerUs) st_.late16++;
        if (late > kLateSlack) {
            off_        += late;
            st_.stretch += late;
        }
        vg93_.step();
        last_ = d;
        publish(d);
        hal::bus_worker_unlock(lock);
        end_trace();
    }
}

// --- Обработчики шины и подъём ---

namespace {

SOUNDSINTH_BDI_CODE("bdi_on_write") void on_write(uint8_t port, uint8_t v) {
    if (!hal::z80_dos_active()) {
        g_bdi_host->note_outside();
        return;
    }
    g_bdi_host->bus_write(port, v, hal::bus_clock_stamp());
}

SOUNDSINTH_BDI_CODE("bdi_on_read_done") void on_read_done(uint8_t port) {
    g_bdi_host->bus_read_done(port, hal::bus_clock_stamp());
}

SOUNDSINTH_BDI_CODE("bdi_worker") void worker() {
    const uint64_t now  = hal::bus_clock_now();
    const uint64_t next = g_bdi_host->service(now);
    g_bdi_host->note_busy(hal::bus_clock_now() - now);
    hal::bus_worker_alarm(next);
}

} // namespace

SOUNDSINTH_INIT_CODE bool bdi_start(uint8_t* mem, size_t bytes) {
    if (bytes < sizeof(BdiHost)) {
        platform::debug_logf("bdi: banks %u B, need %u\n", static_cast<unsigned>(bytes), static_cast<unsigned>(sizeof(BdiHost)));
        return false;
    }
    g_bdi_host = new (mem) BdiHost;
    g_bdi_host->init(hal::bus_clock_now(), &hal::bus_clock_now);
    g_bdi_host->use_pause(&hal::z80_nav_open);
    // Чтения портов - без /WAIT: байт из ячейки отдаёт DMA, Z80 не ждёт
    // никогда; ячейки ведёт поток. Всё или ничего: без
    // ячейки на каждом порту ядра нет - иначе на #1F при DOS отвечала бы
    // мёртвая ячейка.
    for (uint32_t i = 0; i < std::size(kBdiPorts); i++) {
        const uint8_t p = kBdiPorts[i];
        if (!hal::z80_port_dos_cell(p, g_bdi_host->cell(p))) {
            for (uint32_t k = 0; k < i; k++) {
                hal::z80_port_dos_cell(kBdiPorts[k], nullptr);
            }
            g_bdi_host = nullptr;
            platform::debug_logf("bdi: port 0x%02X has no cell, Beta Disk off\n", p);
            return false;
        }
    }
    // Событие кольца будит поток: поток - раньше первого события, образы -
    // раньше обращений Z80.
    hal::bus_worker_init(worker);
    for (int d = 0; d < kDrives; d++) {
        const BdiFile& f = g_bdi_files[d];
        if (f.image == nullptr) continue;
        g_bdi_host->insert(d, f.image, hal::bus_clock_stamp());
        platform::debug_logf("bdi: %c: %s\n", 'A' + d, f.name);
    }
    for (const uint8_t p : kBdiPorts) {
        hal::z80_port_on_write(p, on_write);
    }
    hal::z80_port_dos_on_read_done(on_read_done);
    hal::bus_worker_kick();
    return true;
}

// --- Образы в дисководах ---

namespace {

// Буферы образа на носителе (цилиндр и его дорожки, очередь записи, сектора
// и дорожки без места в файле); блок на дисковод берётся при загрузке и
// переиспользуется при каждой вставке.
constexpr uint32_t kImageArena = MediaDisk::kArenaBytes;
constexpr uint32_t kImageBytes = (sizeof(MediaDisk) + 7u) & ~size_t{7};

// Карта кластеров и участки файла - в PSRAM, не на стеке.
struct Work {
    static constexpr DWORD kMapWords = 2 + 2 * MediaDisk::kMaxExtents;
    DWORD map[kMapWords];
    MediaExtent ext[MediaDisk::kMaxExtents];
};

uint8_t* s_block[kDrives] = {};
Work* s_work              = nullptr;

// Диск из файлов в сборке (bdi_set_*).
struct SetBuild {
    MediaDisk* img;
    int drive;
    uint8_t vol;
    unsigned files;
};
SetBuild s_set = {};

bool media_rd(void* user, uint32_t lba, uint8_t* dst) {
    return storage::storage_tool_read(*static_cast<const hal::Medium*>(user), lba, dst);
}
bool media_wr(void* user, uint32_t lba, const uint8_t* src) {
    return storage::storage_tool_write(*static_cast<const hal::Medium*>(user), lba, src);
}

ImageFormat format_of(const char* name) {
    const char* const dot = std::strrchr(name, '.');
    if (dot == nullptr) return ImageFormat::Unknown;
    const char e0 = static_cast<char>(dot[1] | 0x20), e1 = static_cast<char>(dot[2] | 0x20);
    return e0 == 't' && e1 == 'r' ? ImageFormat::Trd : e0 == 's' && e1 == 'c' ? ImageFormat::Scl : ImageFormat::Unknown;
}

// Последний элемент пути: "SD:/A/B.TRD" -> "B.TRD".
const char* base_name(const char* path) {
    const char* const s = std::strrchr(path, '/');
    return s != nullptr ? s + 1 : path;
}

// Секторы тома vol для образа.
hal::SectorIo io_of(uint8_t vol) {
    return hal::SectorIo{&media_rd, &media_wr, const_cast<hal::Medium*>(storage::volume_medium_ptr(vol))};
}

// Чистый образ в блоке дисковода drive, arena - под его буферы. Новый образ
// ложится на место прежнего без деструктора.
static_assert(std::is_trivially_destructible_v<MediaDisk>, "the drive block is reused without destroying the old image");
MediaDisk* fresh(int drive, Arena& arena) {
    arena = Arena{s_block[drive] + kImageBytes, kImageArena, 0};
    return new (s_block[drive]) MediaDisk;
}

// Образ img (nullptr - пусто) - ядру ВГ93 в дисковод drive; ставит его
// поток, inserted() растёт.
void plug(int drive, MediaDisk* img) {
    if (g_bdi_host == nullptr) return;
    g_bdi_host->insert(drive, img, hal::bus_clock_stamp());
}

// Образ - в таблицу дисководов и ядру ВГ93.
void put(int drive, MediaDisk* img, const char* name) {
    BdiFile& f = g_bdi_files[drive];
    f.image    = img;
    std::snprintf(f.name, sizeof(f.name), "%s", name);
    plug(drive, img);
}

// Участки файла full тома vol в s_work->ext: их число; 0 - не открылся, -1 -
// участков больше kMaxExtents. size - длина, head - первые kHobetaHead байт
// (nullptr - не нужны). Том уже подключён. Отдельно от обхода каталога: FIL
// и FILINFO в одном кадре стека не помещаются в предел.
SOUNDSINTH_NOINLINE int file_extents(uint8_t vol, const char* full, uint32_t& size, uint8_t* head) {
    Work& w = *s_work;
    FIL f;
    if (f_open(&f, full, FA_READ) != FR_OK) return 0;
    UINT got = 0;
    if (head != nullptr) f_read(&f, head, kHobetaHead, &got);
    w.map[0]            = Work::kMapWords;
    f.cltbl             = w.map;
    const bool chain    = f_lseek(&f, CREATE_LINKMAP) == FR_OK;
    size                = static_cast<uint32_t>(f_size(&f));
    const FATFS& fs     = *f.obj.fs;
    const uint32_t base = storage::fat_volume_base(vol) + static_cast<uint32_t>(fs.database);
    int n               = 0;
    for (const DWORD* p = w.map + 1; chain && *p != 0 && n < MediaDisk::kMaxExtents; p += 2, n++) {
        w.ext[n] = MediaExtent{base + (p[1] - 2u) * fs.csize, p[0] * fs.csize};
    }
    f_close(&f);
    return chain ? n : -1;
}

} // namespace

bool bdi_image_name(const char* name) {
    return format_of(name) != ImageFormat::Unknown;
}

// Дисковод пуст: поток снимает источник, очередь записи дописывается в файл.
// Поток не снял источник за 50 мс (событие потеряно на полном кольце) -
// образ остаётся вставленным: поток держит указатель на блок, и новый образ
// поверх него переписал бы объект под работающим ядром ВГ93.
bool bdi_eject(int drive) {
    MediaDisk* const old = g_bdi_files[drive].image;
    if (old == nullptr) return true;
    bool taken = true;
    if (g_bdi_host != nullptr) {
        const uint32_t was = g_bdi_host->inserted();
        plug(drive, nullptr);
        const uint32_t t0 = platform::mono_us();
        while (g_bdi_host->inserted() == was && platform::mono_us() - t0 < 50000u) {
        }
        taken = g_bdi_host->inserted() != was;
    }
    while (!old->idle()) {
        old->service();
    }
    if (!taken) {
        platform::debug_logf("bdi: %c: eject not taken in 50 ms, events lost %" PRIu32 ", the image stays\n", 'A' + drive, g_bdi_host->stats().lost);
        return false;
    }
    g_bdi_files[drive] = BdiFile{};
    return true;
}

bool bdi_mount(int drive, uint8_t vol, const char* full, bool read_only) {
    if (!bdi_eject(drive)) return false;
    const ImageFormat fmt = format_of(full);
    uint32_t size         = 0;
    const int n           = fmt != ImageFormat::Unknown ? file_extents(vol, full, size, nullptr) : 0;
    if (n <= 0) {
        platform::debug_logf("bdi: %s - not a TRD/SCL, not opened or more than %d fragments\n", full, MediaDisk::kMaxExtents);
        return false;
    }
    Arena arena;
    MediaDisk* const img = fresh(drive, arena);
    if (!img->open(fmt, io_of(vol), s_work->ext, n, size, read_only, arena)) {
        platform::debug_logf("bdi: %s - not an image\n", full);
        return false;
    }
    put(drive, img, base_name(full));
    platform::debug_logf("bdi: %c: %s%s\n", 'A' + drive, full, read_only ? " (read only)" : "");
    return true;
}

const char* bdi_drive_name(int drive) {
    return g_bdi_files[drive].name;
}

bool bdi_set_begin(int drive, uint8_t vol) {
    s_set = {};
    if (!bdi_eject(drive)) return false;
    Arena arena;
    MediaDisk* const img = fresh(drive, arena);
    if (!img->open_set(io_of(vol), arena)) return false;
    s_set = SetBuild{img, drive, vol, 0};
    return true;
}

// Файл не открылся; иначе кусков больше, чем держит карта файла или пул
// набора (пределы диска проверены при отметке).
SetAdd bdi_set_add(const char* full, const char* name, uint32_t size) {
    uint8_t head[kHobetaHead];
    const bool hob = hobeta(name, size);
    const int n    = file_extents(s_set.vol, full, size, hob ? head : nullptr);
    if (n <= 0 || !s_set.img->add(name, size, hob ? head : nullptr, s_work->ext, n)) {
        platform::debug_logf("bdi: set: %s - %s\n", name, n == 0 ? "not opened" : "too fragmented");
        return n == 0 ? SetAdd::NoFile : SetAdd::Fragments;
    }
    s_set.files++;
    return SetAdd::Ok;
}

void bdi_set_end(const char* where) {
    s_set.img->seal();
    char name[sizeof(BdiFile::name)];
    std::snprintf(name, sizeof(name), "FILES: %u", s_set.files);
    put(s_set.drive, s_set.img, name);
    platform::debug_logf("bdi: %c: %u files of %s as a TR-DOS disk, %d cylinders\n", 'A' + s_set.drive, s_set.files, where, s_set.img->cylinders());
}

// Блоки PSRAM дисководов - навсегда, при загрузке. Образы вставляет
// навигатор.
bool bdi_reserve(PsramTakeFn take) {
    for (uint8_t*& b : s_block) {
        b = take(kImageBytes + kImageArena);
    }
    s_work = reinterpret_cast<Work*>(take((sizeof(Work) + 7u) & ~size_t{7}));
    return s_block[kDrives - 1] != nullptr && s_work != nullptr;
}

void bdi_poll() {
    if (g_bdi_host == nullptr) return;
    for (int d = 0; d < kDrives; d++) {
        MediaDisk* const img = g_bdi_files[d].image;
        if (img != nullptr && img->service()) {
            g_bdi_host->source_ready(d, hal::bus_clock_stamp());
        }
    }
}

void bdi_swap_ab() {
    if (g_bdi_host == nullptr || g_bdi_files[0].image == nullptr || g_bdi_files[1].image == nullptr) return;
    const BdiFile t = g_bdi_files[0];
    g_bdi_files[0]  = g_bdi_files[1];
    g_bdi_files[1]  = t;
    // Образ живёт в блоке своего дисковода: блоки меняются вместе с образами,
    // иначе fresh() следующего Mount ляжет поверх образа другого дисковода.
    std::swap(s_block[0], s_block[1]);
    g_bdi_host->swap_ab(hal::bus_clock_stamp());
    platform::debug_logf("bdi: A <-> B: A %s, B %s\n", g_bdi_files[0].name, g_bdi_files[1].name);
}

void bdi_machine_reset() {
    if (g_bdi_host == nullptr) return;
    g_bdi_host->machine_reset(hal::bus_clock_stamp());
}

// Обращения, опоздания потока, концы команд с ошибками, ожидания цилиндра,
// дальше след концов команд с прошлого вывода. Без них "TR-DOS не видит
// диск" не отличить от "к портам не обращались".
bool bdi_log_line(int line, char* out, size_t cap) {
    if (g_bdi_host == nullptr) return false;
    const BdiHost& h        = *g_bdi_host;
    const BdiHost::Stats& s = h.stats();
    const Vg93::Stats& f    = h.vg93().stats();
    if (line == 0) {
        std::snprintf(out, cap,
                      "bdi: events %" PRIu32 " runs %" PRIu32 " lost %" PRIu32 " outside %" PRIu32 " | late max %" PRIu32 " us, over 16 us %" PRIu32
                      ", disk stopped %" PRIu32 " ms | busy max %" PRIu32 " us\n",
                      s.events, s.runs, s.lost, s.outside, s.late_max / kHalfPerUs, s.late16, static_cast<uint32_t>(s.stretch / (1000u * kHalfPerUs)),
                      s.busy_max / kHalfPerUs);
        return true;
    }
    if (line == 1) {
        std::snprintf(out, cap,
                      "bdi: ends %" PRIu32 " lost %" PRIu32 " crc %" PRIu32 " rnf %" PRIu32 " | source waits %" PRIu32 " max %" PRIu32
                      " ms | cmd %02X cyl %d head %d | nav pauses %" PRIu32 ", %" PRIu32 " ms\n",
                      f.ends, f.lost, f.crc, f.rnf, f.waits, f.wait_max / (1000u * kHalfPerUs), h.vg93().command(), h.vg93().cylinder(),
                      static_cast<int>(h.vg93().hld()), s.pauses, static_cast<uint32_t>(s.paused / (1000u * kHalfPerUs)));
        return true;
    }
    if (line == 2) {
        // Сбои носителя: сектор, не прочитанный с карты, машина получает с
        // ошибкой CRC; не записанный - теряется.
        uint32_t rd[kDrives] = {}, wr[kDrives] = {};
        for (int d = 0; d < kDrives; d++) {
            if (const MediaDisk* const m = g_bdi_files[d].image) {
                rd[d] = m->read_errors();
                wr[d] = m->write_errors();
            }
        }
        std::snprintf(out, cap, "bdi: media A read errors %" PRIu32 " write errors %" PRIu32 " | B read errors %" PRIu32 " write errors %" PRIu32 "\n", rd[0],
                      wr[0], rd[1], wr[1]);
        return true;
    }
    // cmd trk:sec st reads
    static uint32_t s_trace_at = 0;
    const uint32_t m           = h.trace_count();
    if (m - s_trace_at > BdiHost::kTrace) s_trace_at = m - BdiHost::kTrace;
    if (s_trace_at == m) return false;
    int k = std::snprintf(out, cap, "bdi: trace %" PRIu32 ":", s_trace_at);
    for (uint32_t i = 0; i < 9 && s_trace_at != m && k >= 0 && static_cast<size_t>(k) < cap; i++) {
        const BdiHost::Trace& t  = h.trace(s_trace_at++);
        k                       += std::snprintf(out + k, cap - static_cast<size_t>(k), " %02X %u:%u %02X %u", t.cmd, t.trk, t.sec, t.st, t.reads);
    }
    if (k >= 0 && static_cast<size_t>(k) + 1u < cap) std::snprintf(out + k, cap - static_cast<size_t>(k), "\n");
    return true;
}
} // namespace devices::bdi
