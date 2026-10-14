// SPDX-License-Identifier: MIT
// Арбитр носителя. Сам носитель - за контрактом порта. SMUC - задел.

#include "devices/storage/storage.h"

#include <cinttypes>
#include <cstring>

#include "platform/hot_path.h"
#include "platform/log.h"

#include "devices/hal/media.h"
#include "devices/sd/card_protocol.h" // devices::sd::sd_card_log_handshake
#include "devices/storage/storage_host.h"

namespace devices::storage {
namespace {

namespace hal = devices::hal;

// --- Кэш чтения ---
//
// Прямого отображения: номер сектора по модулю числа строк. Не LRU и не
// ассоциативный: при одном писателе и быстром носителе выигрыш меньше
// цены лишнего состояния, промах стоит чтения сектора с носителя.
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
inline constexpr uint8_t kStatBoard = 0;
inline constexpr uint8_t kStatSides = kEmulatorCount + 1u;

inline uint8_t stat_side_of(sd::SdOwner owner) {
    return static_cast<uint8_t>(static_cast<uint8_t>(owner) + 1u);
}
uint32_t s_hits[kStatSides]   = {};
uint32_t s_misses[kStatSides] = {};

// Носитель каждого эмулятора. Умолчание - карта: до разбора настроек
// флешки всё равно нет.
hal::Medium s_emulator_medium[kEmulatorCount] = {hal::Medium::Card, hal::Medium::Card};

// Чужой номер (None) получает носитель DivMMC, а не отказ. Здесь это
// безопасно: номер приходит от автомата эмулятора, а не с шины, и ответ -
// только носитель, не чужой обмен. У эмулятора карты подмена отдала бы
// обмен соседней стороне, и там чужой номер - отказ.
hal::Medium medium_of(sd::SdOwner owner) {
    const uint8_t i = static_cast<uint8_t>(owner);
    return s_emulator_medium[i < kEmulatorCount ? i : 0u];
}

// Носитель платы - всегда карта. Эмуляторы карты берут носитель по номеру
// хозяина.
constexpr hal::Medium kBoardMedium = hal::Medium::Card;

// Пропуск хоста вперёд (storage_set_host_yield). s_yielding - повторно не
// входить: хост читает обычным путём, но крюк мог бы позвать фоновое.
hal::WaitServiceFn s_host_yield = nullptr;
void* s_host_yield_user         = nullptr;
bool s_yielding                 = false;

// Единица арбитража - сектор: перед каждым обращением фонового клиента к
// носителю. Раза на страницу распаковщика мало: между ними f_lseek читает
// FAT по цепочке кластеров, и заказ esxDOS ждёт до 40 мс. Только перед
// обращением: попадание в кэш носитель не трогает, и пропускать некого.
void yield_to_host() {
    if (s_host_yield == nullptr || s_yielding) return;
    s_yielding = true;
    s_host_yield(s_host_yield_user);
    s_yielding = false;
}

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

// Выбросить строку, если она держит этот сектор этого носителя.
void cache_drop_line(uint32_t lba, hal::Medium m) {
    CacheLine& ln = line_for(lba);
    if (ln.lba == lba && ln.medium == m) ln.valid = false;
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

// Буфер снаружи. Нет буфера - нет и блочного кэша. Алиас записи у платы -
// psram_write_alias: адрес мимо кэша XIP, строки кэша при этом
// сбрасываются. Читать его только через writable.
uint8_t* s_block_buf                = nullptr;
BlockCacheWrite s_block_write_alias = nullptr;

// Заказ предвыборки по носителю: блок, который стоит подтянуть, пока хост
// жуёт нынешний. База ноль значит "заказа нет" - блок с базой ноль
// заказывать незачем, его читают при подъёме.
constexpr uint32_t kNoPrefetch              = 0;
uint32_t s_prefetch_base[kBlockMediumCount] = {};
// Заказ сделан фоновым клиентом: перед наполнением блока хост пропускается
// вперёд, как перед одиночным чтением.
bool s_prefetch_background[kBlockMediumCount] = {};

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

// Адрес для записи bytes байт буфера с p: через алиас записи, если он есть.
uint8_t* writable(uint8_t* p, uint32_t bytes) {
    return s_block_write_alias != nullptr ? s_block_write_alias(p, bytes) : p;
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
    uint8_t* dst      = writable(block_data(mi, bi), run * kSectorBytes);
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

// Смена носителя (флешку вынули и вставили другую, карта пропала и
// вернулась) двигает поколение снимка. Строки, блоки и заказ упреждения
// после неё относятся к прежнему носителю и сбрасываются целиком, у обоих
// носителей: событие редкое, разбираться, чей, дороже.
uint32_t s_media_gen_seen = 0;

SOUNDSINTH_NOINLINE void drop_for_media_change(uint32_t gen) {
    s_media_gen_seen = gen;
    cache_drop_all();
    blocks_drop_all();
}

SOUNDSINTH_ALWAYS_INLINE void follow_media_generation() {
    const uint32_t gen = hal::media_state_generation();
    if (gen != s_media_gen_seen) drop_for_media_change(gen);
}

// Годный блок набора mi, внутри которого лежит сектор lba (block_find
// ищет по базе блока). index - его номер в наборе.
Block* block_holding(uint32_t lba, uint32_t mi, uint32_t* index) {
    for (uint32_t i = 0; i < kBlocksPerMedium; ++i) {
        Block& b = s_blocks[mi][i];
        if (!b.valid) continue;
        if (lba < b.base || lba - b.base >= kBlockSectors) continue;
        *index = i;
        return &b;
    }
    return nullptr;
}

// Блок с этим сектором - негоден. Зовётся на неудачной записи: что
// осталось на носителе, неизвестно, и отдавать прежнее содержимое нельзя.
void block_drop(uint32_t lba, hal::Medium m) {
    if (s_block_buf == nullptr) return;
    uint32_t i = 0;
    if (Block* b = block_holding(lba, medium_index(m), &i)) b->valid = false;
}

// Записанный сектор - и в блок, если он там лежит. Без этого блок отдавал
// бы прежние данные: построчный кэш запись обновляет, а блок оставался бы
// со старым содержимым.
void block_patch(uint32_t lba, hal::Medium m, const uint8_t* data) {
    if (s_block_buf == nullptr) return;
    const uint32_t mi = medium_index(m);
    uint32_t i        = 0;
    const Block* b    = block_holding(lba, mi, &i);
    if (b == nullptr) return;
    std::memcpy(writable(block_data(mi, i) + (lba - b->base) * kSectorBytes, kSectorBytes), data, kSectorBytes);
}

} // namespace

// --- Публичное ---

void storage_attach_block_cache(uint8_t* buf, uint32_t bytes, BlockCacheWrite to_write) {
    if (buf != nullptr && bytes < kBlockCacheBytes) return;
    blocks_drop_all();
    s_block_buf         = buf;
    s_block_write_alias = to_write;
}

bool storage_init() {
    // Кэш сбрасывается до инициализации: при смене носителя старые
    // секторы относятся к другой карте.
    cache_drop_all();
    blocks_drop_all();
    const bool ok    = hal::media_init();
    s_media_gen_seen = hal::media_state_generation();
    return ok;
}

bool storage_present() {
    return hal::media_state(kBoardMedium).present;
}

uint32_t storage_sector_count() {
    return hal::media_state(kBoardMedium).sectors;
}

namespace {
// side - сторона счётчиков: kStatBoard или stat_side_of(хозяин).
bool SOUNDSINTH_HOT_PATH(read_from)(hal::Medium m, uint32_t lba, uint8_t* dst, bool background, uint8_t side) {
    // Признак - из снимка в SRAM, а не у драйвера: проверка стоит ДО
    // поиска в кэше, то есть даже попадание платило бы дорогой во флеш
    // через протокол карты или стек USB. За заказом хоста в это время
    // стоит Z80.
    follow_media_generation();
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
            ++s_misses[side < kStatSides ? side : 0u];
            // Спрошенный сектор отдаётся сразу одиночным чтением, а блок
            // заказывается фоном. Блок целиком стоит много дороже сектора
            // (замер флешки: 895 мкс за сектор против 455 на сектор в
            // пачке), и ждать его хосту незачем - остаток приедет, пока он
            // забирает этот.
            if (background) yield_to_host();
            if (!hal::media_read(m, lba, dst)) return false;
            s_prefetch_base[mi]       = base;
            s_prefetch_background[mi] = background;
            return true;
        }
        ++s_hits[side < kStatSides ? side : 0u];
        ++s_block_hits;
        b->used = ++s_block_clock;
        std::memcpy(dst, block_data(mi, bi) + (lba - base) * kSectorBytes, kSectorBytes);
        // Дошли до второй половины блока - заказать следующий. Читают
        // подряд, и пока машина жуёт остаток этого, следующий успеет
        // приехать: 8 КБ через IN это около двадцати пяти миллисекунд, а
        // блок с флешки - семь.
        if (lba - base >= size / 2u && block_find(base + size, mi, nullptr) == nullptr) {
            s_prefetch_base[mi]       = base + size;
            s_prefetch_background[mi] = background;
        }
        return true;
    }

    const CacheLine& ln = line_for(lba);
    if (ln.valid && ln.lba == lba && ln.medium == m) {
        std::memcpy(dst, ln.data, kSectorBytes);
        ++s_hits[side < kStatSides ? side : 0u];
        return true;
    }
    ++s_misses[side < kStatSides ? side : 0u];

    if (background) yield_to_host();
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
    return read_from(kBoardMedium, lba, dst, true, kStatBoard);
}

void storage_set_emulator_medium(sd::SdOwner owner, hal::Medium m) {
    const uint8_t i = static_cast<uint8_t>(owner);
    if (i < kEmulatorCount) s_emulator_medium[i] = m;
}

hal::Medium storage_emulator_medium(sd::SdOwner owner) {
    return medium_of(owner);
}

// Из снимка: сверку зовут с каждого витка цикла Core1, а числа меняются
// считанные разы за работу.
SOUNDSINTH_HOT_PATH_ATTR("storage_emulator_present")
bool storage_emulator_present(sd::SdOwner owner) {
    return hal::media_state(medium_of(owner)).present;
}

SOUNDSINTH_HOT_PATH_ATTR("storage_emulator_sectors")
uint32_t storage_emulator_sectors(sd::SdOwner owner) {
    return hal::media_state(medium_of(owner)).sectors;
}

SOUNDSINTH_HOT_PATH_ATTR("storage_media_generation")
uint32_t storage_media_generation() {
    return hal::media_state_generation();
}

namespace {
// Заказ одного носителя. true - блок читался (удачно или нет): за вызов
// одно обращение к носителю.
bool prefetch_medium(uint32_t mi) {
    const uint32_t base = s_prefetch_base[mi];
    if (base == kNoPrefetch) return false;
    s_prefetch_base[mi] = kNoPrefetch;
    const hal::Medium m = static_cast<hal::Medium>(mi);
    const uint32_t run  = hal::media_run_sectors(m);
    if (run <= 1u || !hal::media_state(m).present) return false;
    const uint32_t size = run < kBlockSectors ? run : kBlockSectors;
    if (block_find(base, mi, nullptr) != nullptr) return false; // успели прочитать обычным путём
    if (s_prefetch_background[mi]) yield_to_host();
    if (block_fill(base, m, size)) ++s_prefetch_fills;
    return true;
}
} // namespace

void storage_prefetch_step() {
    if (s_block_buf == nullptr) return;
    follow_media_generation();
    // По одному заказу за виток и по кругу носителей: оба читают подряд, и
    // отдавать предпочтение одному не за что.
    for (uint32_t mi = 0; mi < kBlockMediumCount; ++mi) {
        if (prefetch_medium(mi)) return;
    }
}

void storage_prefetch_step_board() {
    if (s_block_buf == nullptr) return;
    follow_media_generation();
    prefetch_medium(medium_index(kBoardMedium));
}

bool SOUNDSINTH_HOT_PATH(storage_emulator_read)(sd::SdOwner owner, uint32_t lba, uint8_t* dst) {
    return read_from(medium_of(owner), lba, dst, false, stat_side_of(owner));
}

void storage_set_host_yield(hal::WaitServiceFn fn, void* user) {
    s_host_yield_user = user;
    s_host_yield      = fn;
}

namespace {
// Запись насквозь, общая для платы и эмуляторов.
bool write_to(hal::Medium m, uint32_t lba, const uint8_t* src) {
    follow_media_generation();
    if (src == nullptr || !hal::media_state(m).present) return false;

    if (!hal::media_write(m, lba, src)) {
        // Что на носителе после неудачной записи - неизвестно, строка
        // выбрасывается: лучше перечитать.
        cache_drop_line(lba, m);
        block_drop(lba, m);
        return false;
    }

    // Строка обновляется, а не выбрасывается: записанное тут же
    // перечитывают - FatFs правит каталог и таблицу размещения по
    // прочитанному, хост перечитывает только что записанное.
    cache_put(lba, m, src);
    block_patch(lba, m, src);
    return true;
}
} // namespace

bool storage_write_board(uint32_t lba, const uint8_t* src) {
    return write_to(kBoardMedium, lba, src);
}

bool storage_write(sd::SdOwner owner, uint32_t lba, const uint8_t* src) {
    // Носитель тот же, что у чтения этого хозяина: иначе машина
    // пишет на одну сторону, а перечитывает с другой.
    return write_to(medium_of(owner), lba, src);
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
    const bool rd = sector != nullptr && read_from(kBoardMedium, 0, sector, false, kStatBoard);
    debug_logf(" (%" PRIu32 " MB, sector 0 %s)", storage_sector_count() / kSectorsPerMb,
               !rd ? "unreadable" : ((sector[kMbrSignature] == 0x55 && sector[kMbrSignature + 1] == 0xaa) ? "0x55AA" : "no signature"));
    if (rd) log_partitions(sector);
    return true;
}

} // namespace devices::storage
