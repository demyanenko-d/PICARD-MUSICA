// SPDX-License-Identifier: MIT
// Арбитр носителя. Сам носитель - за контрактом порта. SMUC - задел.

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
    uint32_t lba               = 0;
    hal::Medium medium         = hal::Medium::Card;
    bool valid                 = false;
    uint8_t data[kSectorBytes] = {};
};

CacheLine s_cache[kLines];

CacheLine& line_for(uint32_t lba) {
    return s_cache[lba % kLines];
}

// Попадания и промахи по сторонам: нулевая - плата, дальше хозяева
// эмуляторов. Кэш один на всех и всего восемь строк, а носители у
// сторон разные: без раздельного счёта не видно, толкают ли они друг
// друга и стоит ли заводить каждой свой.
inline constexpr uint8_t kStatSides = kEmulatorCount + 1u;
uint32_t s_hits[kStatSides]         = {};
uint32_t s_misses[kStatSides]       = {};

// Носитель клиента: у платы он всегда карта. Хост сюда не ходит:
// эмуляторы карты берут носитель по номеру хозяина.
// Носитель каждого эмулятора. Умолчание - карта: до разбора настроек
// флешки всё равно нет.
hal::Medium s_emulator_medium[kEmulatorCount] = {hal::Medium::Card, hal::Medium::Card};

hal::Medium medium_of(uint8_t owner) {
    return s_emulator_medium[owner < kEmulatorCount ? owner : 0u];
}

hal::Medium SOUNDSINTH_HOT_PATH(medium_for)(Client) {
    return hal::Medium::Card;
}

// Пропуск хоста вперёд (storage_set_host_yield). s_yielding - повторно не
// входить: хост читает обычным путём, но крюк мог бы позвать фоновое.
void (*s_host_yield)(void*) = nullptr;
void* s_host_yield_user     = nullptr;
bool s_yielding             = false;

void cache_drop_all() {
    for (uint32_t i = 0; i < kLines; ++i) {
        s_cache[i].valid = false;
    }
}

// Положить сектор в кэш. Строка одна на класс номеров, чужой сектор
// вытесняется.
void SOUNDSINTH_HOT_PATH(cache_put)(uint32_t lba, hal::Medium m, const uint8_t* data) {
    CacheLine& ln = line_for(lba);
    std::memcpy(ln.data, data, kSectorBytes);
    ln.lba    = lba;
    ln.medium = m;
    ln.valid  = true;
}

// --- Блочное упреждение ---
//
// Носитель, у которого пачка секторов дешевле одиночных, читается
// блоками: промах тянет весь блок одной командой, дальше остальные
// секторы достаются без обращения к носителю. Замер флешки: восемь
// секторов одной командой стоят 3877 мкс против 7160 у восьми одиночных.
//
// Вытеснение по давности, а не отображением по номеру: блоков мало и они
// большие, и при чтении подряд отображение выбрасывало бы тот, который
// вот-вот понадобится.
//
// Постройчного кэша это не отменяет: тот ловит повторы (таблица FAT,
// каталог), а этот - чтение подряд. Носитель, у которого пачка не
// дешевле, сюда не заходит вовсе - media_run_sectors отдаёт ему единицу.
struct Block {
    uint32_t base = 0; // первый сектор блока, кратен своему размеру
    bool valid    = false;
    uint32_t used = 0; // отметка давности
};

// По носителю свой набор: стороны карты и флешка друг друга не вытесняют.
Block s_blocks[kBlockMediumCount][kBlocksPerMedium];
uint32_t s_block_clock = 0;

// Буфер снаружи. Нет буфера - нет и блочного кэша.
uint8_t* s_block_buf             = nullptr;
BlockCacheWrite s_block_to_write = nullptr;

// Заказ предвыборки по носителю: блок, который стоит подтянуть, пока хост
// жуёт нынешний. База ноль значит "заказа нет" - блок с базой ноль
// заказывать незачем, его читают при подъёме.
constexpr uint32_t kNoPrefetch              = 0;
uint32_t s_prefetch_base[kBlockMediumCount] = {};

uint32_t s_block_hits     = 0;
uint32_t s_block_fills    = 0;
uint32_t s_prefetch_fills = 0;

inline uint32_t medium_index(hal::Medium m) {
    const uint32_t i = static_cast<uint32_t>(m);
    return i < kBlockMediumCount ? i : 0u;
}

inline uint8_t* block_data(uint32_t mi, uint32_t bi) {
    return s_block_buf + (mi * kBlocksPerMedium + bi) * kBlockBytes;
}

// Индекс блока внутри набора носителя: по нему считается адрес данных.
Block* SOUNDSINTH_HOT_PATH(block_find)(uint32_t base, uint32_t mi, uint32_t* out_index) {
    for (uint32_t i = 0; i < kBlocksPerMedium; ++i) {
        Block& b = s_blocks[mi][i];
        if (b.valid && b.base == base) {
            if (out_index != nullptr) *out_index = i;
            return &b;
        }
    }
    return nullptr;
}

// Жертва: свободный, иначе самый давний. Своя у каждого носителя.
uint32_t block_victim(uint32_t mi) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < kBlocksPerMedium; ++i) {
        if (!s_blocks[mi][i].valid) return i;
        if (s_blocks[mi][i].used < s_blocks[mi][v].used) v = i;
    }
    return v;
}

// Наполнить блок одной командой. Признак годности ставится после чтения:
// оборвалось - блок остаётся негодным, а не отдаёт полмусора.
bool block_fill(uint32_t base, hal::Medium m, uint32_t run) {
    const uint32_t mi = medium_index(m);
    const uint32_t bi = block_victim(mi);
    Block& b          = s_blocks[mi][bi];
    b.valid           = false;
    uint8_t* dst      = block_data(mi, bi);
    if (s_block_to_write != nullptr) dst = s_block_to_write(dst, run * kSectorBytes);
    if (!hal::media_read_run(m, base, run, dst)) return false;
    b.base  = base;
    b.used  = ++s_block_clock;
    b.valid = true;
    ++s_block_fills;
    return true;
}

void blocks_drop_all() {
    for (auto& per_medium : s_blocks) {
        for (Block& b : per_medium) {
            b.valid = false;
        }
    }
    for (uint32_t& p : s_prefetch_base) {
        p = kNoPrefetch;
    }
}

// Записанный сектор - и в блок, если он там лежит. Без этого блок отдавал
// бы прежние данные: построчный кэш запись обновляет, а блок оставался бы
// со старым содержимым.
// Блок с этим сектором - негоден. Зовётся на неудачной записи: что
// осталось на носителе, неизвестно, и отдавать прежнее содержимое нельзя.
void block_drop(uint32_t lba, hal::Medium m) {
    if (s_block_buf == nullptr) return;
    const uint32_t mi = medium_index(m);
    for (uint32_t i = 0; i < kBlocksPerMedium; ++i) {
        Block& b = s_blocks[mi][i];
        if (!b.valid) continue;
        if (lba < b.base || lba - b.base >= kBlockSectors) continue;
        b.valid = false;
        return;
    }
}

void block_patch(uint32_t lba, hal::Medium m, const uint8_t* data) {
    if (s_block_buf == nullptr) return;
    const uint32_t mi = medium_index(m);
    for (uint32_t i = 0; i < kBlocksPerMedium; ++i) {
        Block& b = s_blocks[mi][i];
        if (!b.valid) continue;
        if (lba < b.base || lba - b.base >= kBlockSectors) continue;
        uint8_t* dst = block_data(mi, i) + (lba - b.base) * kSectorBytes;
        if (s_block_to_write != nullptr) dst = s_block_to_write(dst, kSectorBytes);
        std::memcpy(dst, data, kSectorBytes);
        return;
    }
}

} // namespace

// --- Публичное ---

void storage_attach_block_cache(uint8_t* buf, uint32_t bytes, BlockCacheWrite to_write) {
    if (buf != nullptr && bytes < kBlockCacheBytes) return;
    blocks_drop_all();
    s_block_buf      = buf;
    s_block_to_write = to_write;
}

bool storage_init() {
    // Кэш сбрасывается до инициализации: при смене носителя старые
    // секторы относятся к другой карте.
    cache_drop_all();
    blocks_drop_all();
    return hal::media_init();
}

bool storage_present(Client who) {
    return hal::media_present(medium_for(who));
}

uint32_t storage_sector_count(Client who) {
    return hal::media_sector_count(medium_for(who));
}

namespace {
bool SOUNDSINTH_HOT_PATH(read_from)(hal::Medium m, uint32_t lba, uint8_t* dst, bool background, uint8_t who) {
    // Признак - из снимка в SRAM, а не у драйвера: проверка стоит ДО
    // поиска в кэше, то есть даже попадание платило бы дорогой во флеш
    // через протокол карты или стек USB. За заказом хоста в это время
    // стоит Z80.
    if (dst == nullptr || !hal::media_state(m).present) return false;

    // Носитель, у которого пачка дешевле одиночных, читается блоками.
    const uint32_t run = hal::media_run_sectors(m);
    if (run > 1u && s_block_buf != nullptr) {
        const uint32_t size = run < kBlockSectors ? run : kBlockSectors;
        const uint32_t base = lba - (lba % size);
        const uint32_t mi   = medium_index(m);
        uint32_t bi         = 0;
        Block* b            = block_find(base, mi, &bi);
        if (b == nullptr) {
            ++s_misses[who < kStatSides ? who : 0u];
            // Спрошенный сектор отдаётся сразу одиночным чтением, а блок
            // заказывается фоном. Блок целиком стоит много дороже сектора
            // (замер флешки: 895 мкс за сектор против 455 на сектор в
            // пачке), и ждать его хосту незачем - остаток приедет, пока он
            // забирает этот.
            if (!hal::media_read(m, lba, dst)) return false;
            s_prefetch_base[mi] = base;
            return true;
        }
        ++s_hits[who < kStatSides ? who : 0u];
        ++s_block_hits;
        b->used = ++s_block_clock;
        std::memcpy(dst, block_data(mi, bi) + (lba - base) * kSectorBytes, kSectorBytes);
        // Дошли до второй половины блока - заказать следующий. Читают
        // подряд, и пока машина жуёт остаток этого, следующий успеет
        // приехать: 8 КБ через IN это около двадцати пяти миллисекунд, а
        // блок с флешки - семь.
        if (lba - base >= size / 2u && block_find(base + size, mi, nullptr) == nullptr) {
            s_prefetch_base[mi] = base + size;
        }
        return true;
    }

    const CacheLine& ln = line_for(lba);
    if (ln.valid && ln.lba == lba && ln.medium == m) {
        std::memcpy(dst, ln.data, kSectorBytes);
        ++s_hits[who < kStatSides ? who : 0u];
        return true;
    }
    ++s_misses[who < kStatSides ? who : 0u];

    // Единица арбитража - сектор: перед каждым обращением фонового клиента
    // к носителю. Раз на страницу распаковщика было мало: между ними
    // f_lseek по цепочке кластеров читал FAT, и заказ esxDOS ждал до 40 мс.
    if (background && s_host_yield != nullptr && !s_yielding) {
        s_yielding = true;
        s_host_yield(s_host_yield_user);
        s_yielding = false;
    }
    if (!hal::media_read(m, lba, dst)) return false;
    // Фоновое чтение в кэш не кладётся. Банк читается подряд длинными
    // кусками: каждые kLines секторов вымывали бы весь кэш, и таблица FAT
    // с каталогом, ради которых он и заведён, во время игры .mid с банком
    // с карты лежали бы вымытыми. Попаданий сам фоновый клиент не теряет -
    // при чтении подряд их у него нет по построению (замер платы: 5
    // попаданий на 880 промахов).
    if (!background) cache_put(lba, m, dst);
    return true;
}
} // namespace

bool SOUNDSINTH_HOT_PATH(storage_read_background)(uint32_t lba, uint8_t* dst) {
    return read_from(medium_for(Client::Board), lba, dst, true, 0u);
}

void storage_set_emulator_medium(uint8_t owner, hal::Medium m) {
    if (owner < kEmulatorCount) s_emulator_medium[owner] = m;
}

hal::Medium storage_emulator_medium(uint8_t owner) {
    return medium_of(owner);
}

// Из снимка: сверку зовут с каждого витка цикла Core1, а числа меняются
// считанные разы за работу.
SOUNDSINTH_HOT_PATH_ATTR("storage_emulator_present")
bool storage_emulator_present(uint8_t owner) {
    return hal::media_state(medium_of(owner)).present;
}

SOUNDSINTH_HOT_PATH_ATTR("storage_emulator_sectors")
uint32_t storage_emulator_sectors(uint8_t owner) {
    return hal::media_state(medium_of(owner)).sectors;
}

SOUNDSINTH_HOT_PATH_ATTR("storage_media_generation")
uint32_t storage_media_generation() {
    return hal::media_state_generation();
}

void storage_prefetch_step() {
    if (s_block_buf == nullptr) return;
    // По одному заказу за виток и по кругу носителей: оба читают подряд, и
    // отдавать предпочтение одному не за что.
    for (uint32_t mi = 0; mi < kBlockMediumCount; ++mi) {
        const uint32_t base = s_prefetch_base[mi];
        if (base == kNoPrefetch) continue;
        s_prefetch_base[mi] = kNoPrefetch;
        const hal::Medium m = static_cast<hal::Medium>(mi);
        const uint32_t run  = hal::media_run_sectors(m);
        if (run <= 1u || !hal::media_state(m).present) continue;
        const uint32_t size = run < kBlockSectors ? run : kBlockSectors;
        if (block_find(base, mi, nullptr) != nullptr) continue; // успели прочитать обычным путём
        if (block_fill(base, m, size)) ++s_prefetch_fills;
        return;
    }
}

bool SOUNDSINTH_HOT_PATH(storage_emulator_read)(uint8_t owner, uint32_t lba, uint8_t* dst) {
    return read_from(medium_of(owner), lba, dst, false, static_cast<uint8_t>(owner + 1u));
}

void storage_set_host_yield(void (*fn)(void*), void* user) {
    s_host_yield_user = user;
    s_host_yield      = fn;
}

bool storage_write_board(uint32_t lba, const uint8_t* src) {
    const hal::Medium m = medium_for(Client::Board);
    if (src == nullptr || !hal::media_present(m)) return false;

    if (!hal::media_write(m, lba, src)) {
        // Что на носителе после неудачной записи - неизвестно, строка
        // выбрасывается: лучше перечитать.
        CacheLine& ln = line_for(lba);
        if (ln.lba == lba && ln.medium == m) ln.valid = false;
        block_drop(lba, m);
        return false;
    }

    // Строка обновляется, а не выбрасывается: записанное тут же
    // перечитывают - FatFs правит каталог и таблицу размещения по
    // прочитанному.
    cache_put(lba, m, src);
    block_patch(lba, m, src);
    return true;
}

bool storage_write(uint8_t owner, uint32_t lba, const uint8_t* src) {
    // Носитель тот же, что у чтения этого хозяина: иначе машина
    // пишет на одну сторону, а перечитывает с другой.
    const hal::Medium m = medium_of(owner);
    if (src == nullptr || !hal::media_present(m)) return false;

    if (!hal::media_write(m, lba, src)) {
        // Что на носителе после неудачной записи - неизвестно, строка
        // выбрасывается: лучше перечитать.
        CacheLine& ln = line_for(lba);
        if (ln.lba == lba && ln.medium == m) ln.valid = false;
        block_drop(lba, m);
        return false;
    }

    // Строка обновляется, а не выбрасывается: хост почти всегда
    // перечитывает только что записанное.
    cache_put(lba, m, src);
    block_patch(lba, m, src);
    return true;
}

void storage_log_health() {
    hal::media_log_health();
    // Строка печатается, только когда к носителю вообще ходили.
    uint32_t total = 0;
    for (uint8_t i = 0; i < kStatSides; ++i) {
        total += s_hits[i] + s_misses[i];
    }
    if (total == 0) return;
    debug_logf("storage: sector cache (hit/miss): board %" PRIu32 "/%" PRIu32 ", divmmc %" PRIu32 "/%" PRIu32 ", zctrl %" PRIu32 "/%" PRIu32 "\n", s_hits[0],
               s_misses[0], s_hits[1], s_misses[1], s_hits[2], s_misses[2]);
    // Блочное упреждение: сколько секторов отдано из блока, сколько блоков
    // наполнено и сколько из них - заранее, а не по промаху. Наполнено
    // заранее много - значит хост за сектором не ждал.
    if (s_block_fills != 0) {
        debug_logf("storage: blocks: served from block %" PRIu32 ", filled %" PRIu32 " (of them ahead %" PRIu32 ")\n", s_block_hits, s_block_fills,
                   s_prefetch_fills);
    }
}

// --- Самопроверка при старте ---

namespace {

// Разметка MBR в секторе 0.
constexpr uint32_t kMbrPartitionTable   = 446;
constexpr uint32_t kMbrPartitionCount   = 4;
constexpr uint32_t kMbrEntryBytes       = 16;
constexpr uint32_t kMbrEntryType        = 4;
constexpr uint32_t kMbrEntryFirstSector = 8;
constexpr uint32_t kMbrEntrySectorCount = 12;
constexpr uint32_t kMbrSignature        = 510; // 0x55, 0xaa
constexpr uint32_t kSectorsPerMb        = 1024u * 1024u / kSectorBytes;

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void log_partitions(const uint8_t* sector) {
    debug_log("\nboot: partitions:");
    bool any = false;
    for (uint8_t i = 0; i < kMbrPartitionCount; ++i) {
        const uint8_t* e   = &sector[kMbrPartitionTable + i * kMbrEntryBytes];
        const uint8_t type = e[kMbrEntryType];
        if (type == 0) continue;
        any = true;
        debug_logf(" [%u] type 0x%02X, from sector %" PRIu32 ", %" PRIu32 " MB;", static_cast<unsigned>(i), static_cast<unsigned>(type),
                   read_le32(e + kMbrEntryFirstSector), read_le32(e + kMbrEntrySectorCount) / kSectorsPerMb);
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
    const bool rd = sector != nullptr && read_from(medium_for(Client::Board), 0, sector, false, 0u);
    debug_logf(" (%" PRIu32 " MB, sector 0 %s)", storage_sector_count(Client::Board) / kSectorsPerMb,
               !rd ? "unreadable" : ((sector[kMbrSignature] == 0x55 && sector[kMbrSignature + 1] == 0xaa) ? "0x55AA" : "no signature"));
    if (rd) log_partitions(sector);
    return true;
}

} // namespace devices::storage
