// Арбитр носителя (storage.h). Сам носитель - за devices/hal/media.h: по
// планам выбор между SD и USB flash для DivMMC, Z-Controller и SMUC.

#include "devices/storage/storage.h"

#include <cinttypes>
#include <cstring>

#include "devices/hal/media.h"
#include "devices/sd/card_protocol.h" // devices::sd::sd_card_log_handshake
#include "devices/storage/storage_host.h"
#include "platform/hot_path.h"
#include "platform/log.h"

namespace devices::storage {
namespace {

namespace hal = devices::hal;

// --- Кэш чтения ---
//
// Прямого отображения: номер сектора по модулю числа строк. Не LRU и не
// ассоциативный: при одном писателе и быстром носителе выигрыш меньше
// цены лишнего состояния, промах стоит около 200 мкс.
//
// Кэш общий для обоих клиентов, поэтому он здесь, а не в эмуляторе. Но
// носитель у строки свой: когда хосту досталась флешка, а плате карта,
// один и тот же номер сектора значит на них разное.

constexpr uint32_t kLines = kCacheSectors;

struct CacheLine {
    uint32_t    lba = 0;
    hal::Medium medium = hal::Medium::Card;
    bool        valid = false;
    uint8_t     data[kSectorBytes] = {};
};

CacheLine s_cache[kLines];

CacheLine& line_for(uint32_t lba) { return s_cache[lba % kLines]; }

// Носитель клиента. У платы он один и тот же всегда; хосту флешка
// достаётся, как только её воткнули, и отбирается, когда вынули.
//
// Смена на ходу - дело пользователя, а не наше: машина в этот момент
// думает, что карта у неё та же. Поэтому переход пишется в журнал -
// иначе развалившаяся файловая система выглядела бы беспричинной.
hal::Medium s_host_medium = hal::Medium::Card;

hal::Medium SOUNDSINTH_HOT_PATH(medium_for)(Client who) {
    if (who == Client::Board) return hal::Medium::Card;
    const hal::Medium now = hal::media_present(hal::Medium::Usb) ? hal::Medium::Usb : hal::Medium::Card;
    if (now != s_host_medium) {
        s_host_medium = now;
        debug_logf("storage: машине отдана %s\n", now == hal::Medium::Usb ? "флешка" : "карта");
    }
    return now;
}

// Пропуск хоста вперёд (storage_set_host_yield). s_yielding - повторно не
// входить: хост читает обычным путём, но крюк мог бы позвать фоновое.
void (*s_host_yield)(void*) = nullptr;
void* s_host_yield_user = nullptr;
bool s_yielding = false;

void cache_drop_all() {
    for (uint32_t i = 0; i < kLines; ++i) s_cache[i].valid = false;
}

// Положить сектор в кэш. Строка одна на класс номеров, чужой сектор
// вытесняется.
void SOUNDSINTH_HOT_PATH(cache_put)(uint32_t lba, hal::Medium m, const uint8_t* data) {
    CacheLine& ln = line_for(lba);
    std::memcpy(ln.data, data, kSectorBytes);
    ln.lba = lba;
    ln.medium = m;
    ln.valid = true;
}

} // namespace

// --- Публичное ---

bool storage_init() {
    // Кэш сбрасывается до инициализации: при смене носителя старые
    // секторы относятся к другой карте.
    cache_drop_all();
    return hal::media_init();
}

bool storage_present(Client who) { return hal::media_present(medium_for(who)); }

uint32_t storage_sector_count(Client who) { return hal::media_sector_count(medium_for(who)); }

namespace {
bool SOUNDSINTH_HOT_PATH(read_sector)(uint32_t lba, uint8_t* dst, bool background) {
    const hal::Medium m = medium_for(background ? Client::Board : Client::Host);
    if (dst == nullptr || !hal::media_present(m)) return false;

    const CacheLine& ln = line_for(lba);
    if (ln.valid && ln.lba == lba && ln.medium == m) {
        std::memcpy(dst, ln.data, kSectorBytes);
        return true;
    }

    // Единица арбитража - сектор: перед каждым обращением фонового клиента
    // к носителю. Раз на страницу распаковщика было мало: между ними
    // f_lseek по цепочке кластеров читал FAT, и заказ esxDOS ждал до 40 мс.
    if (background && s_host_yield != nullptr && !s_yielding) {
        s_yielding = true;
        s_host_yield(s_host_yield_user);
        s_yielding = false;
    }
    if (!hal::media_read(m, lba, dst)) return false;
    cache_put(lba, m, dst);
    return true;
}
} // namespace

bool SOUNDSINTH_HOT_PATH(storage_read)(uint32_t lba, uint8_t* dst) {
    return read_sector(lba, dst, false);
}

bool SOUNDSINTH_HOT_PATH(storage_read_background)(uint32_t lba, uint8_t* dst) {
    return read_sector(lba, dst, true);
}

void storage_set_host_yield(void (*fn)(void*), void* user) {
    s_host_yield_user = user;
    s_host_yield = fn;
}

bool storage_write(uint32_t lba, const uint8_t* src) {
    // Пишет только хост: код платы носитель не меняет.
    const hal::Medium m = medium_for(Client::Host);
    if (src == nullptr || !hal::media_present(m)) return false;

    if (!hal::media_write(m, lba, src)) {
        // Что на носителе после неудачной записи - неизвестно, строка
        // выбрасывается: лучше перечитать.
        CacheLine& ln = line_for(lba);
        if (ln.lba == lba && ln.medium == m) ln.valid = false;
        return false;
    }

    // Строка обновляется, а не выбрасывается: хост почти всегда
    // перечитывает только что записанное.
    cache_put(lba, m, src);
    return true;
}

void storage_log_health() { hal::media_log_health(); }

// --- Самопроверка при старте ---

namespace {

// Разметка MBR в секторе 0.
constexpr uint32_t kMbrPartitionTable = 446;
constexpr uint32_t kMbrPartitionCount = 4;
constexpr uint32_t kMbrEntryBytes = 16;
constexpr uint32_t kMbrEntryType = 4;
constexpr uint32_t kMbrEntryFirstSector = 8;
constexpr uint32_t kMbrEntrySectorCount = 12;
constexpr uint32_t kMbrSignature = 510; // 0x55, 0xAA
constexpr uint32_t kSectorsPerMb = 1024u * 1024u / kSectorBytes;

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void log_partitions(const uint8_t* sector) {
    debug_log("\nboot: partitions:");
    bool any = false;
    for (uint8_t i = 0; i < kMbrPartitionCount; ++i) {
        const uint8_t* e = &sector[kMbrPartitionTable + i * kMbrEntryBytes];
        const uint8_t type = e[kMbrEntryType];
        if (type == 0) continue;
        any = true;
        debug_logf(" [%u] type 0x%02X, from sector %" PRIu32 ", %" PRIu32 " MB;", static_cast<unsigned>(i),
                   static_cast<unsigned>(type), read_le32(e + kMbrEntryFirstSector),
                   read_le32(e + kMbrEntrySectorCount) / kSectorsPerMb);
    }
    if (any) return;
    debug_log(" TABLE EMPTY -- card is not partitioned");
    // Суперфлоппи: файловая система с нулевого сектора, без таблицы
    // разделов. В начале команда перехода.
    if (sector[0] == 0xeb || sector[0] == 0xe9) debug_log(" (but looks like a FAT boot sector)");
}

} // namespace

bool storage_boot_check(uint8_t* sector) {
    const bool ok = storage_init();
    // Рукопожатие само не печатает: его зовёт и переинициализация.
    devices::sd::sd_card_log_handshake();
    debug_logf("boot: storage_init %s", ok ? "PASS" : "FAIL");
    if (!ok) return false;
    const bool rd = sector != nullptr && storage_read(0, sector);
    debug_logf(" (%" PRIu32 " MB, sector 0 %s)", storage_sector_count(Client::Board) / kSectorsPerMb,
               !rd ? "unreadable"
                   : ((sector[kMbrSignature] == 0x55 && sector[kMbrSignature + 1] == 0xaa) ? "0x55AA"
                                                                                           : "no signature"));
    if (rd) log_partitions(sector);
    return true;
}

} // namespace devices::storage
