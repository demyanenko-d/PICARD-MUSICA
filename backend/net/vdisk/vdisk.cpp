// SPDX-License-Identifier: MIT
// Виртуальный диск FAT32: сектора тома из листингов и файлов поставщика.

#include "net/vdisk/vdisk.h"

#include <algorithm>
#include <cstring>

#include "core/util/fnv1a.h"
#include "core/util/le_bytes.h"

#include "devices/storage/fat_layout.h"
#include "net/vdisk/fat_names.h"

namespace net::vdisk {
namespace {

// Ячейка в таблице. Флаги - в старших разрядах размера.
struct Slot {
    Key key;
    uint32_t size_flags;
    uint16_t parent;
    uint16_t issuer; // листинг, который выдал ячейку
};
static_assert(sizeof(Slot) == kSlotRecordBytes, "slot record size");

constexpr uint32_t kUsedBit   = 1u << 31;
constexpr uint32_t kDirBit    = 1u << 30;
constexpr uint32_t kSizeMask  = kDirBit - 1u;
constexpr uint16_t kPermanent = 0xffffu; // выдана навсегда: корень
constexpr uint16_t kNoSlot    = 0xffffu;
constexpr uint32_t kMaxDepth  = 32;
constexpr uint32_t kProbes    = 32; // ячеек-кандидатов от позиции хэша
// Отметка чтения переписывается, когда отстала на столько чтений: не
// писать в PSRAM на каждый сектор.
constexpr uint32_t kStampStep = 256;
constexpr uint32_t kCopyBytes = 64;
constexpr char kLabel[]       = "ZX-ART     "; // метка тома
static_assert(sizeof(kLabel) - 1u == 11u, "volume label is 11 bytes");

// Листинг в кэше каталогов: готовые байты записей. start - сплошная
// позиция в кольце, адрес - по маске.
struct DirRec {
    uint32_t start;
    uint32_t bytes;
    uint32_t seq; // порядок создания: меньше - старше
    uint16_t slot;
    uint16_t id;
    uint16_t entries;
    bool live;
    bool loading;
};
constexpr uint32_t kDirRecs = 32;

enum class FileState : uint8_t { Free, Requested, Loading, Done };

// Файл - потоком через окно: машина читает файл подряд, окно держит
// принятое от kStreamBackBytes позади читающего. Назад дальше окна - файл
// заново с начала: часть файла сайт не отдаёт.
struct Stream {
    uint32_t size;
    uint32_t received;
    uint32_t read_pos; // последнее, что машина просила
    uint16_t slot;
    FileState state;
};

uint8_t* s_mem              = nullptr;
platform::WriteAliasFn s_wr = nullptr;
uint8_t* s_slots_mem        = nullptr;
uint8_t* s_stamps_mem       = nullptr;
uint8_t* s_hashes_mem       = nullptr;
uint8_t* s_dir_mem          = nullptr;
uint8_t* s_stream_mem       = nullptr;
uint32_t s_ring_head        = 0; // сплошная позиция конца самого нового листинга
uint32_t s_dir_seq          = 0;
uint16_t s_dir_id           = 0;
uint32_t s_clock            = 0;
int32_t s_listing           = -1; // листинг, который сейчас принимается
uint16_t s_want_slot        = kNoSlot;
WantKind s_want_kind        = WantKind::None;
uint16_t s_failed_slot      = kNoSlot; // отказ поставщика: одно чтение вернёт Error
Stats s_stats{};

// Записи - в хвосте блока: читает и пишет только цикл Core1, через кэш.
// Строки кэша с участками под алиасом записи не делит: начало и размер
// кратны 8.
struct Tables {
    DirRec dirs[kDirRecs];
    Stream stream;
};
static_assert(sizeof(Tables) <= kTableBytes && kTableBytes - sizeof(Tables) < 8u && kTableBytes % 8u == 0u, "vdisk tables fill their tail");
Tables* s_t = nullptr;

// --- Память ---

void write_bytes(uint8_t* dst, const void* src, uint32_t n) {
    std::memcpy(s_wr(dst, n), src, n);
}

void fill_zero(uint8_t* dst, uint32_t n) {
    const uint8_t zero[kCopyBytes]{};
    for (uint32_t off = 0; off < n; off += kCopyBytes) {
        write_bytes(dst + off, zero, std::min(n - off, kCopyBytes));
    }
}

// Кольцо size байт (степень двойки), позиция сплошная: через край -
// кусками.
void wrap_read(const uint8_t* ring, uint32_t size, uint32_t pos, uint8_t* out, uint32_t n) {
    for (uint32_t done = 0; done < n;) {
        const uint32_t at   = (pos + done) & (size - 1u);
        const uint32_t part = std::min(n - done, size - at);
        std::memcpy(out + done, ring + at, part);
        done += part;
    }
}

void wrap_write(uint8_t* ring, uint32_t size, uint32_t pos, const uint8_t* p, uint32_t n) {
    for (uint32_t done = 0; done < n;) {
        const uint32_t at   = (pos + done) & (size - 1u);
        const uint32_t part = std::min(n - done, size - at);
        write_bytes(ring + at, p + done, part);
        done += part;
    }
}

Slot get_slot(uint32_t i) {
    Slot s;
    std::memcpy(&s, s_slots_mem + i * sizeof(Slot), sizeof(Slot));
    return s;
}

void put_slot(uint32_t i, const Slot& s) {
    write_bytes(s_slots_mem + i * sizeof(Slot), &s, sizeof(Slot));
}

uint32_t get_stamp(uint32_t i) {
    uint32_t v;
    std::memcpy(&v, s_stamps_mem + i * 4u, 4);
    return v;
}

void put_stamp(uint32_t i, uint32_t v) {
    write_bytes(s_stamps_mem + i * 4u, &v, 4);
}

void touch(uint32_t slot) {
    ++s_clock;
    if (s_clock - get_stamp(slot) >= kStampStep) put_stamp(slot, s_clock);
}

using soundsinth::util::put_le16;
using soundsinth::util::put_le32;

uint32_t slot_cluster(uint32_t slot) {
    return kRootCluster + slot * kSlotClusters;
}

// --- Служебные сектора ---

// Адрес CHS записи раздела по геометрии kHeads x kSectorsPerTrack; за
// пределом - 1023/15/63.
void put_chs(uint8_t* p, uint32_t lba) {
    uint32_t c = lba / (kHeads * kSectorsPerTrack);
    uint32_t h = lba / kSectorsPerTrack % kHeads;
    uint32_t s = lba % kSectorsPerTrack + 1u;
    if (c > 1023u) {
        c = 1023u;
        h = kHeads - 1u;
        s = kSectorsPerTrack;
    }
    p[0] = static_cast<uint8_t>(h);
    p[1] = static_cast<uint8_t>(s | ((c >> 8) << 6));
    p[2] = static_cast<uint8_t>(c);
}

namespace fat = devices::storage;

void build_mbr(uint8_t* out) {
    put_le32(out + fat::kMbrDiskId, 0x5a585452u);
    uint8_t* const p = out + fat::kMbrPartitionTable;
    put_chs(p + fat::kMbrEntryChsFirst, kPartStart);
    p[fat::kMbrEntryType] = 0x0c; // FAT32 LBA
    put_chs(p + fat::kMbrEntryChsLast, kTotalSectors - 1u);
    put_le32(p + fat::kMbrEntryFirstSector, kPartStart);
    put_le32(p + fat::kMbrEntrySectorCount, kPartSectors);
    out[fat::kMbrSignature]      = 0x55;
    out[fat::kMbrSignature + 1u] = 0xaa;
}

void build_boot(uint8_t* out) {
    out[0] = 0xeb;
    out[1] = 0x58;
    out[2] = 0x90;
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result): поле FAT без нуля в конце
    std::memcpy(out + 3, "PICARD  ", 8);
    put_le16(out + fat::kBpbBytesPerSec, kSectorBytes);
    out[fat::kBpbSecPerClus] = static_cast<uint8_t>(kClusterSectors);
    put_le16(out + fat::kBpbReserved, kReserved);
    out[fat::kBpbNumFats] = static_cast<uint8_t>(kFatCount);
    out[fat::kBpbMedia]   = 0xf8;
    put_le16(out + fat::kBpbSecPerTrack, kSectorsPerTrack);
    put_le16(out + fat::kBpbHeads, kHeads);
    put_le32(out + fat::kBpbHidden, kPartStart);
    put_le32(out + fat::kBpbTotal32, kPartSectors);
    put_le32(out + fat::kBpbFatSize32, kFatSectors);
    put_le32(out + fat::kBpbRootClus32, kRootCluster);
    put_le16(out + fat::kBpbFsInfo32, kFsInfoSector);
    put_le16(out + fat::kBpbBackup32, kBackupBoot);
    out[fat::kBpbDrive32]   = 0x80;
    out[fat::kBpbBootSig32] = 0x29;
    put_le32(out + fat::kBpbVolId32, 0x5a585452u);
    std::memcpy(out + fat::kBpbLabel32, kLabel, fat::kBpbLabelBytes);
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result): поле FAT без нуля в конце
    std::memcpy(out + fat::kBpbFsType32, "FAT32   ", 8);
    out[fat::kMbrSignature]      = 0x55;
    out[fat::kMbrSignature + 1u] = 0xaa;
}

// Свободно 0: драйверу нечего искать в FAT.
void build_fsinfo(uint8_t* out) {
    put_le32(out + fat::kFsiLeadSig, fat::kFsiLead);
    put_le32(out + fat::kFsiStructSig, fat::kFsiStruct);
    put_le32(out + fat::kFsiFree, 0u);
    put_le32(out + fat::kFsiNext, 0xffffffffu);
    put_le32(out + fat::kFsiTrailSig, fat::kFsiTrail);
}

// За последним кластером тома - "плохой": и там свободного нет.
void build_fat(uint32_t index, uint8_t* out) {
    constexpr uint32_t kPerSector = kSectorBytes / 4u;
    for (uint32_t j = 0; j < kPerSector; ++j) {
        const uint32_t c = index * kPerSector + j;
        uint32_t v       = 0x0ffffff7u;
        if (c == 0u) {
            v = 0x0ffffff8u;
        } else if (c == 1u) {
            v = 0x0fffffffu;
        } else if (c < kClusters + 2u) {
            v = (c - 2u) % kSlotClusters == kSlotClusters - 1u ? 0x0fffffffu : c + 1u;
        }
        put_le32(out + j * 4u, v);
    }
}

// --- Листинги ---

int32_t find_dir(uint32_t slot) {
    for (uint32_t i = 0; i < kDirRecs; ++i) {
        if (s_t->dirs[i].live && s_t->dirs[i].slot == slot) return static_cast<int32_t>(i);
    }
    return -1;
}

bool issuer_live(uint16_t id) {
    if (id == kPermanent) return true;
    for (const DirRec& r : s_t->dirs) {
        if (r.live && r.id == id) return true;
    }
    return false;
}

// Приём в окно открыт.
bool receiving() {
    return s_t->stream.state == FileState::Loading;
}

bool streamed(uint32_t slot) {
    return s_t->stream.state != FileState::Free && s_t->stream.slot == slot;
}

// Ячейка на пути к живому листингу: её каталог драйвер может перечитать
// через "..".
bool on_live_path(uint32_t slot) {
    for (const DirRec& r : s_t->dirs) {
        if (!r.live) continue;
        uint32_t s = r.slot;
        for (uint32_t depth = 0; depth < kMaxDepth; ++depth) {
            if (s == slot) return true;
            if (s == 0u) break;
            s = get_slot(s).parent;
        }
    }
    return false;
}

// Занятую ячейку можно отдать другому объекту.
bool slot_reusable(uint32_t i, const Slot& s) {
    if (i == 0u || i == s_want_slot) return false;
    if (issuer_live(s.issuer)) return false;
    if (streamed(i)) return false;
    return (s.size_flags & kDirBit) == 0u || !on_live_path(i);
}

void evict_dir(DirRec& r) {
    r.live = false;
    ++s_stats.dirs_evicted;
}

// Самый старый листинг, кроме принимаемого.
bool evict_oldest_dir() {
    int32_t oldest = -1;
    for (uint32_t i = 0; i < kDirRecs; ++i) {
        const DirRec& r = s_t->dirs[i];
        if (!r.live || static_cast<int32_t>(i) == s_listing) continue;
        if (oldest < 0 || r.seq < s_t->dirs[oldest].seq) oldest = static_cast<int32_t>(i);
    }
    if (oldest < 0) return false;
    evict_dir(s_t->dirs[oldest]);
    return true;
}

uint32_t slot_hash(Key key, uint32_t parent) {
    uint64_t h  = (key ^ (static_cast<uint64_t>(parent) << 48)) * 0x9e3779b97f4a7c15ull;
    h          ^= h >> 29;
    return static_cast<uint32_t>(h % (kSlots - 1u)) + 1u;
}

// Из занятых, которые можно отдать, - давно не читанная.
struct Pick {
    uint16_t slot = kNoSlot;
    uint32_t age  = 0;
    bool empty    = false;

    void offer(uint32_t i, const Slot& s) {
        if (empty) return;
        if ((s.size_flags & kUsedBit) == 0u) {
            slot  = static_cast<uint16_t>(i);
            empty = true;
            return;
        }
        if (!slot_reusable(i, s)) return;
        const uint32_t a = s_clock - get_stamp(i);
        if (slot == kNoSlot || a > age) {
            slot = static_cast<uint16_t>(i);
            age  = a;
        }
    }
};

// Ячейка под объект. Тот же ключ в том же каталоге - прежняя ячейка.
uint16_t alloc_slot(Key key, uint32_t parent, bool dir, bool& same) {
    same             = false;
    const uint32_t h = slot_hash(key, parent);
    Pick pick;
    for (uint32_t p = 0; p < kProbes; ++p) {
        const uint32_t i = (h - 1u + p) % (kSlots - 1u) + 1u;
        const Slot s     = get_slot(i);
        if ((s.size_flags & kUsedBit) != 0u && s.key == key && s.parent == parent && ((s.size_flags & kDirBit) != 0u) == dir) {
            same = true;
            return static_cast<uint16_t>(i);
        }
        pick.offer(i, s);
    }
    // Окно занято: вся таблица, а не найдётся - вытеснять листинги.
    while (pick.slot == kNoSlot) {
        for (uint32_t i = 1; i < kSlots && !pick.empty; ++i) {
            pick.offer(i, get_slot(i));
        }
        if (pick.slot == kNoSlot && !evict_oldest_dir()) return kNoSlot;
    }
    if (!pick.empty) ++s_stats.slots_reused;
    return pick.slot;
}

// Байты листинга d от off, не больше сектора. Возвращает их число.
uint32_t dir_read(const DirRec& d, uint32_t off, uint8_t* out) {
    const uint32_t n = off < d.bytes ? std::min(d.bytes - off, kSectorBytes) : 0u;
    wrap_read(s_dir_mem, kDirCacheBytes, d.start + off, out, n);
    return n;
}

// Приписать байты к принимаемому листингу. Листинги, чьё начало кольцо
// сейчас перезапишет, вытесняются.
void dir_append(const uint8_t* p, uint32_t n) {
    DirRec& r            = s_t->dirs[s_listing];
    const uint32_t floor = s_ring_head + n - kDirCacheBytes;
    for (uint32_t i = 0; i < kDirRecs; ++i) {
        DirRec& o = s_t->dirs[i];
        if (o.live && static_cast<int32_t>(i) != s_listing && static_cast<int32_t>(o.start - floor) < 0) evict_dir(o);
    }
    wrap_write(s_dir_mem, kDirCacheBytes, s_ring_head, p, n);
    s_ring_head += n;
    r.bytes     += n;
}

// --- Имена ---

// " (n)" перед расширением; основа укорачивается, если не влезает.
uint32_t number_name(uint16_t* name, uint32_t len, uint32_t number) {
    uint16_t tail[8] = {' ', '('};
    uint32_t t       = 2;
    char d[4];
    uint32_t k = 0;
    do {
        d[k++]  = static_cast<char>('0' + number % 10u);
        number /= 10u;
    } while (number != 0u && k < 3u);
    while (k > 0u) {
        tail[t++] = static_cast<uint16_t>(d[--k]);
    }
    tail[t++]          = ')';
    const uint32_t ext = fat_ext_len(name, len);
    uint16_t saved[6];
    std::memcpy(saved, name + len - ext, ext * sizeof(uint16_t));
    uint32_t base = len - ext;
    if (base + t + ext > kNameMax) base = kNameMax - t - ext;
    std::memcpy(name + base, tail, t * sizeof(uint16_t));
    std::memcpy(name + base + t, saved, ext * sizeof(uint16_t));
    return base + t + ext;
}

uint8_t sfn_checksum(const uint8_t sfn[11]) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11u; ++i) {
        sum = static_cast<uint8_t>(((sum & 1u) << 7) + (sum >> 1) + sfn[i]);
    }
    return sum;
}

uint16_t fat_date(uint16_t year) {
    if (year < 1980u || year > 2107u) year = 1980u;
    return static_cast<uint16_t>(((year - 1980u) << 9) | (1u << 5) | 1u);
}

void short_entry(uint8_t* e, const uint8_t sfn[11], uint8_t attr, uint32_t cluster, uint32_t size, uint16_t year) {
    std::memset(e, 0, 32);
    std::memcpy(e, sfn, 11);
    e[11]             = attr;
    const uint16_t dt = fat_date(year);
    put_le16(e + 16, dt);
    put_le16(e + 18, dt);
    put_le16(e + 20, cluster >> 16);
    put_le16(e + 24, dt);
    put_le16(e + 26, cluster);
    put_le32(e + 28, size);
}

// Короткая запись с именем name, дополненным пробелами до 11 знаков.
void named_entry(uint8_t* e, const char* name, uint8_t attr, uint32_t cluster) {
    uint8_t sfn[11];
    std::memset(sfn, ' ', 11);
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result): поле FAT без нуля в конце
    std::memcpy(sfn, name, std::strlen(name));
    short_entry(e, sfn, attr, cluster, 0, 0);
}

// Начало каталога: у корня метка тома, у прочих "." и "..". Возвращает
// число байтов.
uint32_t dir_head(uint32_t slot, uint8_t* out) {
    if (slot == 0u) {
        named_entry(out, kLabel, 0x08, 0);
        return 32;
    }
    const uint32_t parent = get_slot(slot).parent;
    named_entry(out, ".", 0x10, slot_cluster(slot));
    named_entry(out + 32, "..", 0x10, parent == 0u ? 0u : slot_cluster(parent));
    return 64;
}

// Записи длинного имени, последняя часть первой.
uint32_t long_entries(uint8_t* out, const uint16_t* name, uint32_t len, uint8_t sum) {
    static constexpr uint8_t kPos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    const uint32_t parts              = (len + 12u) / 13u;
    for (uint32_t k = 0; k < parts; ++k) {
        const uint32_t part = parts - 1u - k;
        uint8_t* e          = out + k * 32u;
        std::memset(e, 0, 32);
        e[0]  = static_cast<uint8_t>((part + 1u) | (k == 0u ? 0x40u : 0u));
        e[11] = 0x0f;
        e[13] = sum;
        for (uint32_t j = 0; j < 13u; ++j) {
            const uint32_t i = part * 13u + j;
            uint16_t ch      = 0xffffu;
            if (i < len) {
                ch = name[i];
            } else if (i == len) {
                ch = 0u;
            }
            put_le16(e + kPos[j], ch);
        }
    }
    return parts * 32u;
}

// Хэш длинного имени без учёта регистра, как сравнивает FAT.
uint32_t long_hash(const uint16_t* name, uint32_t len) {
    uint32_t h = soundsinth::util::kFnvSeed;
    for (uint32_t i = 0; i < len; ++i) {
        h = (h ^ fat_fold(name[i])) * soundsinth::util::kFnvPrime;
    }
    return h;
}

// Ключ уже есть в принимаемом листинге; нет - запоминается. Хэш-таблица в
// памяти диска, ноль - пусто. Длинные и короткие имена - в одной таблице,
// их ключи разведены младшим разрядом.
bool hash_taken(uint32_t h) {
    for (uint32_t i = h % kNameHashes;; i = (i + 1u) % kNameHashes) {
        uint32_t v;
        std::memcpy(&v, s_hashes_mem + i * 4u, 4);
        if (v == h) return true;
        if (v == 0u) {
            write_bytes(s_hashes_mem + i * 4u, &h, 4);
            return false;
        }
    }
}

bool long_taken(const uint16_t* name, uint32_t len) {
    return hash_taken(long_hash(name, len) | 1u);
}

bool sfn_taken(const uint8_t sfn[11]) {
    const uint32_t h = soundsinth::util::fnv1a(soundsinth::util::kFnvSeed, sfn, 11) & ~1u;
    return hash_taken(h == 0u ? 2u : h);
}

// Длинное имя записи; повтор в листинге получает номер " (n)".
uint32_t unique_long_name(const char* utf8, uint16_t* name) {
    uint32_t len = fat_long_name(utf8, name);
    if (!long_taken(name, len)) return len;
    const uint32_t base_len = len;
    uint16_t base[kNameMax];
    std::memcpy(base, name, base_len * sizeof(uint16_t));
    for (uint32_t n = 2; long_taken(name, len) && n < 1000u; ++n) {
        std::memcpy(name, base, base_len * sizeof(uint16_t));
        len = number_name(name, base_len, n);
    }
    ++s_stats.names_renamed;
    return len;
}

// Короткое имя: точное 8.3 без длинного или как у Windows - основа без
// хвоста, если она точна, иначе ~1..~4, дальше с хэшем длинного имени; тогда
// в lfn - записи длинного имени. Возвращает их байты.
uint32_t short_name(const uint16_t* name, uint32_t len, uint8_t sfn[11], uint8_t& case_bits, uint8_t* lfn) {
    if (fat_exact_sfn(name, len, sfn, case_bits) && !sfn_taken(sfn)) return 0;
    const SfnBasis basis = fat_sfn_basis(name, len);
    const auto hash      = static_cast<uint16_t>(long_hash(name, len) >> 8);
    uint32_t k           = basis.lossy ? 1u : 0u;
    fat_sfn_candidate(basis, k, hash, sfn);
    while (sfn_taken(sfn) && k < 1000u) {
        fat_sfn_candidate(basis, ++k, hash, sfn);
    }
    case_bits = 0;
    return long_entries(lfn, name, len, sfn_checksum(sfn));
}

// Ячейка под запись листинга r: тот же объект - прежняя. kNoSlot - ячеек нет.
uint16_t place(const Entry& in, const DirRec& r, uint32_t size) {
    bool same           = false;
    const uint16_t slot = alloc_slot(in.key, r.slot, in.dir, same);
    if (slot == kNoSlot) return kNoSlot;
    if (same) {
        ++s_stats.slots_kept;
        if (streamed(slot) && s_t->stream.size != size && !receiving()) s_t->stream.state = FileState::Free;
    } else {
        put_stamp(slot, s_clock);
        if (s_failed_slot == slot) s_failed_slot = kNoSlot;
    }
    put_slot(slot, Slot{in.key, kUsedBit | (in.dir ? kDirBit : 0u) | size, static_cast<uint16_t>(r.slot), r.id});
    return slot;
}

// --- Файлы ---

// Окно под файл ячейки; принимаемый в него прежний файл бросается.
void stream_open(uint32_t slot, uint32_t size) {
    if (receiving()) ++s_stats.files_dropped;
    s_t->stream = Stream{size, 0, 0, static_cast<uint16_t>(slot), FileState::Requested};
}

// --- Чтение ---

ReadResult pending(WantKind kind, uint32_t slot) {
    s_want_kind = kind;
    s_want_slot = static_cast<uint16_t>(slot);
    ++s_stats.pending;
    return ReadResult::Pending;
}

ReadResult failed() {
    ++s_stats.errors;
    return ReadResult::Error;
}

bool take_failure(uint32_t slot) {
    if (s_failed_slot != slot) return false;
    s_failed_slot = kNoSlot;
    return true;
}

// Сектор-заглушка каталога: что уже пришло, затем запись name. Без
// листинга первый сектор начинается с "." и ".." (у корня - метки тома).
ReadResult dir_stub(uint32_t slot, uint32_t off, uint8_t* out, const DirRec* d, const char* name) {
    uint32_t at = 0;
    if (d != nullptr) {
        at = dir_read(*d, off, out);
    } else if (off == 0u) {
        at = dir_head(slot, out);
    }
    if (at + 32u <= kSectorBytes) named_entry(out + at, name, 0x21, 0);
    ++s_stats.placeholders;
    return ReadResult::Ready;
}

// Листинг ещё в пути: заглушка LOADING, ожидание остаётся.
ReadResult loading_stub(uint32_t slot, uint32_t off, uint8_t* out, const DirRec* d) {
    (void)pending(WantKind::List, slot);
    return dir_stub(slot, off, out, d, "LOADING");
}

ReadResult read_dir(uint32_t slot, uint32_t off, uint8_t* out, bool placeholder) {
    const int32_t r = find_dir(slot);
    if (r >= 0) {
        const DirRec& d = s_t->dirs[r];
        if (d.loading && off + kSectorBytes > d.bytes) return placeholder ? loading_stub(slot, off, out, &d) : pending(WantKind::List, slot);
        dir_read(d, off, out);
        return ReadResult::Ready;
    }
    // Листинг не получен: каталог с записью ERROR, а не ошибка диска -
    // машина не бросает работу, повторный вход попробует снова.
    if (take_failure(slot)) return dir_stub(slot, off, out, nullptr, "ERROR");
    if (placeholder) return loading_stub(slot, off, out, nullptr);
    return pending(WantKind::List, slot);
}

ReadResult read_file(uint32_t slot, uint32_t size, uint32_t off, uint8_t* out) {
    if (off >= size) return ReadResult::Ready;
    if (take_failure(slot)) return failed();
    const uint32_t end = std::min(off + kSectorBytes, size);
    Stream& f          = s_t->stream;
    if (f.state == FileState::Free || f.slot != slot || f.size != size) {
        stream_open(slot, size);
    } else if (off + kStreamBytes < f.received) {
        stream_open(slot, size);
        ++s_stats.stream_restarts;
    }
    f.read_pos = off;
    if (f.received < end) return pending(WantKind::File, slot);
    wrap_read(s_stream_mem, kStreamBytes, off, out, end - off);
    return ReadResult::Ready;
}

} // namespace

bool vdisk_init(uint8_t* mem, uint32_t bytes, platform::WriteAliasFn to_write) {
    if (mem == nullptr || to_write == nullptr || bytes < kMemoryBytes) return false;
    s_mem        = mem;
    s_wr         = to_write;
    s_slots_mem  = mem;
    s_stamps_mem = s_slots_mem + kSlotTableBytes;
    s_hashes_mem = s_stamps_mem + kStampBytes;
    s_dir_mem    = s_hashes_mem + kNameHashBytes;
    s_stream_mem = s_dir_mem + kDirCacheBytes;
    s_t          = reinterpret_cast<Tables*>(s_stream_mem + kStreamBytes);
    for (DirRec& r : s_t->dirs) {
        r = DirRec{};
    }
    s_t->stream   = Stream{};
    s_ring_head   = 0;
    s_dir_seq     = 0;
    s_dir_id      = 0;
    s_clock       = 0;
    s_listing     = -1;
    s_want_slot   = kNoSlot;
    s_want_kind   = WantKind::None;
    s_failed_slot = kNoSlot;
    s_stats       = Stats{};
    // Таблица пустая целиком: ячейка без флага занятости свободна.
    fill_zero(s_slots_mem, kSlotTableBytes + kStampBytes);
    put_slot(0, Slot{kRoot, kUsedBit | kDirBit, 0, kPermanent});
    return true;
}

ReadResult vdisk_read(uint32_t lba, uint8_t* out, bool placeholder) {
    ++s_stats.reads;
    if (s_mem == nullptr || lba >= kTotalSectors) return failed();
    std::memset(out, 0, kSectorBytes);
    if (lba < kPartStart) {
        if (lba == 0u) build_mbr(out);
        return ReadResult::Ready;
    }
    if (lba < kFatStart) {
        const uint32_t rel = lba - kPartStart;
        if (rel == 0u || rel == kBackupBoot) {
            build_boot(out);
        } else if (rel == kFsInfoSector || rel == kBackupBoot + kFsInfoSector) {
            build_fsinfo(out);
        }
        return ReadResult::Ready;
    }
    if (lba < kDataStart) {
        build_fat((lba - kFatStart) % kFatSectors, out);
        return ReadResult::Ready;
    }
    const uint32_t d    = lba - kDataStart;
    const uint32_t slot = d / kSlotSectors;
    const uint32_t off  = (d % kSlotSectors) * kSectorBytes;
    const Slot s        = get_slot(slot);
    if ((s.size_flags & kUsedBit) == 0u) return ReadResult::Ready;
    touch(slot);
    if ((s.size_flags & kDirBit) != 0u) return read_dir(slot, off, out, placeholder);
    return read_file(slot, s.size_flags & kSizeMask, off, out);
}

Want vdisk_want() {
    if (s_want_kind == WantKind::None) return Want{};
    return Want{s_want_kind, get_slot(s_want_slot).key};
}

bool vdisk_list_begin(Key dir) {
    if (s_want_kind != WantKind::List || s_listing >= 0) return false;
    const Slot ws = get_slot(s_want_slot);
    if (ws.key != dir || (ws.size_flags & kDirBit) == 0u) return false;
    int32_t free_rec = -1;
    while (free_rec < 0) {
        for (uint32_t i = 0; i < kDirRecs; ++i) {
            if (!s_t->dirs[i].live) free_rec = static_cast<int32_t>(i);
        }
        if (free_rec < 0 && !evict_oldest_dir()) return false;
    }
    if (++s_dir_id == kPermanent) s_dir_id = 1;
    DirRec& r = s_t->dirs[free_rec];
    r         = DirRec{s_ring_head, 0, ++s_dir_seq, s_want_slot, s_dir_id, 0, true, true};
    s_listing = free_rec;
    fill_zero(s_hashes_mem, kNameHashBytes);
    uint8_t e[64];
    dir_append(e, dir_head(r.slot, e));
    return true;
}

bool vdisk_list_add(const Entry& in) {
    if (s_listing < 0) return false;
    DirRec& r = s_t->dirs[s_listing];
    if (r.entries >= kDirEntriesMax || (!in.dir && in.size > kFileMax)) {
        ++s_stats.entries_dropped;
        return false;
    }
    uint16_t name[kNameMax];
    const uint32_t len = unique_long_name(in.name, name);
    uint8_t sfn[11];
    uint8_t buf[kEntryBytesMax];
    uint8_t case_bits   = 0;
    const uint32_t n    = short_name(name, len, sfn, case_bits, buf);
    uint32_t cluster    = 0;
    const uint32_t size = in.dir ? 0u : in.size;
    if (in.dir || size != 0u) {
        const uint16_t slot = place(in, r, size);
        if (slot == kNoSlot) {
            ++s_stats.entries_dropped;
            return false;
        }
        cluster = slot_cluster(slot);
    }
    short_entry(buf + n, sfn, in.dir ? 0x10 : 0x21, cluster, size, in.year);
    buf[n + 12u] = case_bits;
    dir_append(buf, n + 32u);
    ++r.entries;
    return true;
}

void vdisk_list_end(bool ok) {
    if (s_listing < 0) return;
    DirRec& r         = s_t->dirs[s_listing];
    s_listing         = -1;
    const bool waited = s_want_kind == WantKind::List && s_want_slot == r.slot;
    if (waited) s_want_kind = WantKind::None;
    if (ok) {
        r.loading = false;
        ++s_stats.listings;
        return;
    }
    r.live = false;
    // Отказ получает только то чтение, что его ждёт.
    if (waited) s_failed_slot = r.slot;
    ++s_stats.listings_failed;
}

bool vdisk_file_begin(Key file) {
    if (s_want_kind != WantKind::File) return false;
    const Slot ws = get_slot(s_want_slot);
    if (ws.key != file || (ws.size_flags & kDirBit) != 0u) return false;
    Stream& f = s_t->stream;
    if (f.slot != s_want_slot || f.state != FileState::Requested) return false;
    f.state    = FileState::Loading;
    f.received = 0;
    return true;
}

uint32_t vdisk_file_room() {
    if (!receiving()) return 0xffffffffu;
    const Stream& f      = s_t->stream;
    const uint32_t floor = f.read_pos > kStreamBackBytes ? f.read_pos - kStreamBackBytes : 0u;
    const uint32_t kept  = f.received > floor ? f.received - floor : 0u;
    return kept >= kStreamBytes ? 0u : kStreamBytes - kept;
}

bool vdisk_file_data(const uint8_t* p, uint32_t n) {
    if (!receiving()) return false;
    Stream& f = s_t->stream;
    n         = std::min(n, f.size - f.received);
    wrap_write(s_stream_mem, kStreamBytes, f.received, p, n);
    f.received += n;
    return true;
}

void vdisk_file_restart() {
    if (receiving()) s_t->stream.received = 0;
}

void vdisk_file_end(bool ok) {
    if (!receiving()) return;
    Stream& f         = s_t->stream;
    const bool waited = s_want_kind == WantKind::File && s_want_slot == f.slot;
    if (waited) s_want_kind = WantKind::None;
    if (ok && f.received == f.size) {
        f.state = FileState::Done;
        ++s_stats.files_loaded;
        return;
    }
    f.state = FileState::Free;
    if (!waited) { // ждут уже другой объект: отказ отдавать некому
        ++s_stats.files_dropped;
        return;
    }
    if (ok) ++s_stats.files_short;
    s_failed_slot = f.slot;
    ++s_stats.files_failed;
}

Stats vdisk_stats() {
    return s_stats;
}

bool vdisk_ready() {
    return s_mem != nullptr;
}

} // namespace net::vdisk
