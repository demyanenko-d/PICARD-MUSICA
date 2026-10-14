// SPDX-License-Identifier: MIT
#include "core/config/config_store.h"

#include <cstddef>
#include <cstring>

#include "core/util/crc32.h"

namespace soundsinth::config {
namespace {

// Сумма считается по заголовку без подписи и без самой суммы, а следом по
// настройкам. Подпись в неё не входит: она и так сверяется отдельно, а
// формат, длина и счётчик - входят, иначе порча заголовка прошла бы мимо.
uint32_t page_crc(const PageHeader& h, const uint8_t* payload) {
    uint32_t crc = util::kCrc32Init;
    crc          = util::crc32_update(crc, reinterpret_cast<const uint8_t*>(&h.format), sizeof(h.format));
    crc          = util::crc32_update(crc, reinterpret_cast<const uint8_t*>(&h.bytes), sizeof(h.bytes));
    crc          = util::crc32_update(crc, reinterpret_cast<const uint8_t*>(&h.counter), sizeof(h.counter));
    return util::crc32_update(crc, payload, h.bytes);
}

bool header_of(const uint8_t* page, PageHeader& out) {
    if (page == nullptr) return false;
    std::memcpy(&out, page, sizeof(out));
    if (out.magic != kMagic) return false;
    if (out.format != kFormatVersion) return false;
    // Короче нынешнего блок писала прошивка, у которой полей было меньше:
    // читается он целиком, недостающие остаются умолчаниями. Длиннее -
    // писала прошивка новее нас, и что в лишних байтах, мы не знаем.
    if (out.bytes > kLegacyBytes) return false;
    if (out.bytes == 0) return false;
    return out.crc == page_crc(out, page + kPayloadOffset);
}

// Поля за неподвижной частью: номер записи и место в Settings. Номера не
// переиспользуются: запись, которую прошивка не знает, пропускается.
struct Record {
    uint16_t id;
    uint16_t offset;
    uint16_t size;
};
constexpr Record kRecords[] = {
    {1, offsetof(Settings, joy_8bit), sizeof(Settings::joy_8bit)},
    {2, offsetof(Settings, joy_keys), sizeof(Settings::joy_keys)},
};

struct RecordHeader {
    uint16_t id;
    uint16_t bytes;
};
static_assert(sizeof(RecordHeader) == 4, "the record header is frozen");

constexpr uint32_t kExtOffset = kPayloadOffset + kLegacyBytes;
constexpr uint32_t kExtData   = kExtOffset + sizeof(ExtHeader);

// Записи читаются, только когда сошлась сумма расширения. Неподвижная
// часть от него не зависит: блок старой прошивки расширения не имеет, а
// настройки в нём годные.
void ext_read(const uint8_t* page, Settings& out) {
    ExtHeader e{};
    std::memcpy(&e, page + kExtOffset, sizeof(e));
    if (e.magic != kExtMagic || e.bytes > kPageBytes - kExtData) return;
    if (e.crc != util::crc32_update(util::kCrc32Init, page + kExtData, e.bytes)) return;
    auto* const dst = reinterpret_cast<uint8_t*>(&out);
    uint32_t at     = 0;
    while (at + sizeof(RecordHeader) <= e.bytes) {
        RecordHeader r{};
        std::memcpy(&r, page + kExtData + at, sizeof(r));
        at += sizeof(r);
        if (r.bytes > e.bytes - at) return;
        for (const Record& k : kRecords) {
            // Короче нынешней запись писала прошивка с меньшей таблицей:
            // хвост остаётся умолчанием. Длиннее - лишнее отбрасывается.
            if (k.id == r.id) std::memcpy(dst + k.offset, page + kExtData + at, r.bytes < k.size ? r.bytes : k.size);
        }
        at += r.bytes;
    }
}

void ext_build(uint8_t* page, const Settings& s) {
    const auto* const src = reinterpret_cast<const uint8_t*>(&s);
    uint32_t at           = 0;
    for (const Record& k : kRecords) {
        const RecordHeader r{k.id, k.size};
        std::memcpy(page + kExtData + at, &r, sizeof(r));
        std::memcpy(page + kExtData + at + sizeof(r), src + k.offset, k.size);
        at += sizeof(r) + k.size;
    }
    ExtHeader e{};
    e.magic = kExtMagic;
    e.bytes = static_cast<uint16_t>(at);
    e.crc   = util::crc32_update(util::kCrc32Init, page + kExtData, at);
    std::memcpy(page + kExtOffset, &e, sizeof(e));
}

constexpr uint32_t records_bytes() {
    uint32_t n = 0;
    for (const Record& k : kRecords) {
        n += sizeof(RecordHeader) + k.size;
    }
    return n;
}
static_assert(kExtData + records_bytes() <= kPageBytes, "the records fit the block");

// Счётчик восьмизначный и когда-нибудь перевернётся. Сравнение по разности
// со знаком переживает это: важно не какой больше, а какой следующий.
bool newer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

} // namespace

PageInfo page_inspect(const uint8_t* page) {
    PageInfo info;
    PageHeader h{};
    if (!header_of(page, h)) return info;
    info.valid   = true;
    info.counter = h.counter;
    return info;
}

bool page_read(const uint8_t* page, Settings& out) {
    PageHeader h{};
    if (!header_of(page, h)) return false;
    // Поверх умолчаний, а не на пустое место: у короткого блока хвост полей
    // остаётся тем, чем пришёл.
    out = Settings{};
    std::memcpy(&out, page + kPayloadOffset, h.bytes);
    if (h.bytes == kLegacyBytes) ext_read(page, out);
    settings_clamp(out);
    return true;
}

void page_build(uint8_t* page, const Settings& s, uint32_t counter) {
    // Единицы, а не нули: стёртая флеш-память состоит из единиц, и запас
    // блока тогда пишется "как есть", не меняя ни одной ячейки.
    std::memset(page, 0xFF, kPageBytes);
    PageHeader h{};
    h.magic   = kMagic;
    h.format  = kFormatVersion;
    h.bytes   = kLegacyBytes;
    h.counter = counter;
    std::memcpy(page + kPayloadOffset, &s, kLegacyBytes);
    ext_build(page, s);
    h.crc = page_crc(h, page + kPayloadOffset);
    std::memcpy(page, &h, sizeof(h));
}

int store_pick(const uint8_t* page0, const uint8_t* page1) {
    const PageInfo a = page_inspect(page0);
    const PageInfo b = page_inspect(page1);
    if (a.valid && b.valid) return newer(b.counter, a.counter) ? 1 : 0;
    if (a.valid) return 0;
    if (b.valid) return 1;
    return -1;
}

NextWrite store_next(const uint8_t* page0, const uint8_t* page1) {
    NextWrite next;
    const int cur = store_pick(page0, page1);
    if (cur < 0) return next; // годных нет: нулевой блок, счётчик с единицы
    const PageInfo info = page_inspect(cur == 0 ? page0 : page1);
    next.slot           = cur == 0 ? 1 : 0;
    next.counter        = info.counter + 1u;
    return next;
}

} // namespace soundsinth::config
