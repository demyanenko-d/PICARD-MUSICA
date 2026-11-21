// SPDX-License-Identifier: MIT
#pragma once

// Виртуальный диск FAT32 только для чтения: сектора собираются на лету из
// листингов и файлов, которые отдаёт поставщик (каталог в сети).
//
// Каждый объект - каталог или файл - занимает ячейку в 1 МБ, 32 кластера
// по 32 КБ. FAT считается формулой: цепочка внутри ячейки, последний
// кластер ячейки - её конец. От содержимого она не зависит, и кэш FAT
// драйвера устареть не может.
//
// Ячейка выбирается по хэшу пары (ключ, родитель): тот же объект в том же
// каталоге получает те же кластеры и после повторного листинга. Занять
// чужую ячейку можно, когда выдавший её листинг вытеснен, она не на пути
// к живому листингу, не держит файл и не ждёт данных; из таких берётся
// та, которую дольше всех не читали.
//
// Все вызовы - из одного потока: и чтение секторов, и доставка данных.

#include <cstddef>
#include <cstdint>

#include "platform/memory.h"

#include "devices/hal/media.h"

namespace net::vdisk {

inline constexpr uint32_t kSectorBytes    = devices::hal::kSectorBytes;
inline constexpr uint32_t kClusterSectors = 64; // 32 КБ
inline constexpr uint32_t kClusterBytes   = kClusterSectors * kSectorBytes;
inline constexpr uint32_t kSlotClusters   = 32;
inline constexpr uint32_t kSlotSectors    = kSlotClusters * kClusterSectors;
inline constexpr uint32_t kSlotBytes      = kSlotSectors * kSectorBytes; // 1 МБ
// Том не длиннее геометрии CHS 16383 x 16 x 63 и 2^24 секторов: драйверы
// с CHS и с трёхбайтовым LBA видят его целиком.
inline constexpr uint32_t kSlots = 8060;

inline constexpr uint32_t kHeads           = 16;
inline constexpr uint32_t kSectorsPerTrack = 63;
inline constexpr uint32_t kChsSectors      = 16383u * kHeads * kSectorsPerTrack;

inline constexpr uint32_t kPartStart    = 2048;
inline constexpr uint32_t kReserved     = 32;
inline constexpr uint32_t kFatCount     = 2;
inline constexpr uint32_t kFsInfoSector = 1; // от начала раздела
inline constexpr uint32_t kBackupBoot   = 6;
inline constexpr uint32_t kClusters     = kSlots * kSlotClusters;
inline constexpr uint32_t kFatSectors   = ((kClusters + 2u) * 4u + kSectorBytes - 1u) / kSectorBytes;
inline constexpr uint32_t kFatStart     = kPartStart + kReserved;
inline constexpr uint32_t kDataStart    = kFatStart + kFatCount * kFatSectors;
inline constexpr uint32_t kPartSectors  = kReserved + kFatCount * kFatSectors + kClusters * kClusterSectors;
inline constexpr uint32_t kTotalSectors = kPartStart + kPartSectors;
inline constexpr uint32_t kRootCluster  = 2;
static_assert(kTotalSectors <= kChsSectors, "volume must fit the CHS geometry");
static_assert(kTotalSectors <= (1u << 24), "volume must fit 24-bit LBA");
static_assert(kClusters >= 65525u, "cluster count must make the volume FAT32");
static_assert(kSlots < 0xffffu, "slot numbers must fit 16 bits");

// Длинное имя - до 64 знаков (UTF-16); в записи листинга - UTF-8 до
// kNameBytes байт. Файл - до одной ячейки.
inline constexpr uint32_t kNameMax       = 64;
inline constexpr uint32_t kNameBytes     = 2u * kNameMax;
inline constexpr uint32_t kFileMax       = kSlotBytes;
inline constexpr uint32_t kDirEntriesMax = 2000;
// Запись каталога с самым длинным именем: части длинного имени и короткое.
inline constexpr uint32_t kEntryBytesMax = 32u * ((kNameMax + 12u) / 13u + 1u);

// Память, которую отдаёт порт: таблица ячеек, отметки чтения, хэши имён
// принимаемого листинга, кэш каталогов кольцом, кэш файлов кусками.
inline constexpr uint32_t kSlotRecordBytes = 16;
inline constexpr uint32_t kSlotTableBytes  = kSlots * kSlotRecordBytes;
inline constexpr uint32_t kStampBytes      = kSlots * 4u;
inline constexpr uint32_t kNameHashes      = 8192;
inline constexpr uint32_t kNameHashBytes   = kNameHashes * 4u;
inline constexpr uint32_t kDirCacheBytes   = 512u * 1024u;
// Окно файла: машина читает подряд, сеть быстрее машины в среднем, а
// окно сглаживает скачки обеих. Позади читающего держится kStreamBackBytes.
inline constexpr uint32_t kStreamBytes     = 64u * 1024u;
inline constexpr uint32_t kStreamBackBytes = 16u * 1024u;
static_assert((kStreamBytes & (kStreamBytes - 1u)) == 0u, "stream window is addressed by mask");
// Записи кэшей каталогов и файлов - в хвосте того же блока.
inline constexpr uint32_t kTableBytes  = 656;
inline constexpr uint32_t kMemoryBytes = kSlotTableBytes + kStampBytes + kNameHashBytes + kDirCacheBytes + kStreamBytes + kTableBytes;
static_assert((kDirCacheBytes & (kDirCacheBytes - 1u)) == 0u, "directory ring is addressed by mask");
static_assert(64u + kDirEntriesMax * kEntryBytesMax <= kDirCacheBytes, "largest listing must fit the directory cache");
static_assert(64u + kDirEntriesMax * kEntryBytesMax <= kSlotBytes, "largest listing must fit its slot");
static_assert(kNameHashes >= 4u * kDirEntriesMax, "name hash table (long and short names) must stay half empty");

// Ключ объекта у поставщика. 0 - корень.
using Key                  = uint64_t;
inline constexpr Key kRoot = 0;

enum class ReadResult : uint8_t { Ready, Pending, Error };

// Запись листинга. Имя - UTF-8; год 1980..2107 становится датой записи.
struct Entry {
    Key key       = 0;
    uint32_t size = 0;
    uint16_t year = 0;
    bool dir      = false;
    char name[kNameBytes + 1]{};
};

enum class WantKind : uint8_t { None, List, File };

// Что нужно машине сейчас: листинг каталога или файл.
struct Want {
    WantKind kind = WantKind::None;
    Key key       = 0;
};

struct Stats {
    uint32_t reads;
    uint32_t pending;
    uint32_t errors;
    uint32_t listings;
    uint32_t listings_failed;
    uint32_t dirs_evicted;
    uint32_t slots_reused;    // ячейка отдана другому объекту
    uint32_t slots_kept;      // повторный листинг вернул прежнюю ячейку
    uint32_t entries_dropped; // каталог полон, ячеек нет или файл больше ячейки
    uint32_t names_renamed;   // повтор имени в каталоге получил номер
    uint32_t files_loaded;
    uint32_t files_failed;
    uint32_t files_short;     // приём кончился раньше размера из листинга
    uint32_t files_dropped;   // приём брошен: машина перешла к другому файлу
    uint32_t stream_restarts; // машина читала позади окна - файл заново
    uint32_t placeholders;    // отдано заглушек каталога
};

// mem - kMemoryBytes, to_write - адрес записи в неё от порта. false - памяти
// мало, диск не поднят.
bool vdisk_init(uint8_t* mem, uint32_t bytes, platform::WriteAliasFn to_write);

// Сектор тома. Pending - данных ещё нет, повторить позже.
//
// placeholder: сектор каталога, который ещё не пришёл, отдаётся сразу:
// пришедшие записи (в начале - "." и "..") и запись LOADING. Без этого
// машина, которая не ждёт долго, не входит в каталог. Отказ листинга -
// каталог с записью ERROR.
ReadResult vdisk_read(uint32_t lba, uint8_t* out, bool placeholder = false);

// Чего ждёт последнее отложенное чтение.
Want vdisk_want();

// Листинг: begin, записи по одной, end. Ключ - из vdisk_want. add
// возвращает false, если запись не принята.
bool vdisk_list_begin(Key dir);
bool vdisk_list_add(const Entry& e);
void vdisk_list_end(bool ok);

// Файл: begin, данные по порядку, end. Данных больше размера из листинга
// - хвост отбрасывается; меньше - приём считается неудачным. data не
// больше room; false - приём брошен (машина перешла к другому файлу),
// соединение закрывается без end.
bool vdisk_file_begin(Key file);
uint32_t vdisk_file_room();
bool vdisk_file_data(const uint8_t* p, uint32_t n);
void vdisk_file_end(bool ok);
// Приём заново с начала: соединение оборвалось, сайт отдаёт файл только
// целиком. Сектора, которые уже были, снова ждут повтора.
void vdisk_file_restart();

Stats vdisk_stats();

// Память отдана, диск работает.
bool vdisk_ready();

} // namespace net::vdisk
