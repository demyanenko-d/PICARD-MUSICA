// SPDX-License-Identifier: MIT
#include "player/nav/nav.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

#include "core/util/fnv1a.h"
#include "core/util/le_bytes.h"

#include "devices/bdi/vg93.h"
#include "devices/bdi/disk.h"

namespace player::nav {

using namespace devices;

using bdi::kDrives;
using bdi::kFileSectors;
using bdi::kSetFiles;
using bdi::kSetSectors;
using bdi::set_sectors;

namespace {

constexpr uint32_t kMagic        = 0x5641424e;  // "NBAV"
constexpr uint32_t kSumSeed      = 0x9e3779b9u; // не FNV: смена обнулит память в PSRAM
constexpr const char* kVolName[] = {"SD", "USB", "WIFI"};
static_assert(sizeof(kVolName) / sizeof(kVolName[0]) == kVolumes, "a name per volume");
static_assert(static_cast<uint32_t>(Kind::Image) == nav_page::kKindImage && static_cast<uint32_t>(Kind::RecentImage) > nav_page::kKindImage,
              "images sort last in Kind");

uint32_t sum_of(const Memory& m) {
    return soundsinth::util::fnv1a(kSumSeed, reinterpret_cast<const uint8_t*>(&m), offsetof(Memory, sum));
}

// Строчная CP866 - в прописную: латиница, а..п (A0..AF -> 80..8F), р..я
// (E0..EF -> 90..9F), ё (F1 -> F0).
uint8_t fold(uint8_t c) {
    if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 32);
    if (c >= 0xa0 && c <= 0xaf) return static_cast<uint8_t>(c - 0x20);
    if (c >= 0xe0 && c <= 0xef) return static_cast<uint8_t>(c - 0x50);
    if (c == 0xf1) return 0xf0;
    return c;
}

bool before(const Item& a, const Item& b) {
    if ((a.kind == Kind::Dir) != (b.kind == Kind::Dir)) return a.kind == Kind::Dir;
    for (int i = 0; i < kNameBytes; i++) {
        const uint8_t x = fold(static_cast<uint8_t>(a.name[i])), y = fold(static_cast<uint8_t>(b.name[i]));
        if (x != y) return x < y;
        if (x == 0) return false;
    }
    return false;
}

// Путь + "/" + имя; false - не помещается.
bool join(char* path, const char* name) {
    const size_t len = std::strlen(path), add = std::strlen(name);
    const size_t at = len > 1 ? len + 1 : len;
    if (at + add + 1 > static_cast<size_t>(kPathBytes)) return false;
    path[len] = '/';
    std::memcpy(path + at, name, add + 1);
    return true;
}

using soundsinth::util::put_le16;

// Страница: заголовок, вставленные образы, окно строк с first.
void render(const Navigator& nv, uint8_t* page, uint32_t first, uint8_t port_7ffd, const Board& board) {
    namespace pg = nav_page;
    std::memcpy(page + pg::kMagic, pg::kMagicBytes, sizeof(pg::kMagicBytes));
    const auto count    = static_cast<uint32_t>(nv.count());
    page[pg::kLevel]    = static_cast<uint8_t>(nv.level());
    page[pg::kPort7ffd] = port_7ffd;
    page[pg::kBank]     = 0;
    put_le16(page + pg::kCount, count);
    put_le16(page + pg::kFirst, first);
    put_le16(page + pg::kCursor, static_cast<uint32_t>(nv.cursor()));
    put_le16(page + pg::kMarked, static_cast<uint32_t>(nv.marked()));
    put_le16(page + pg::kMarkedKb, (nv.marked_sectors() + 3u) / 4u);
    char* const title = reinterpret_cast<char*>(page + pg::kTitle);
    switch (nv.level()) {
        case pg::Level::Top:
            cut(title, pg::kTitleBytes, {"BETA DISK"});
            break;
        case pg::Level::Recent:
            cut(title, pg::kTitleBytes, {"RECENT"});
            break;
        case pg::Level::Dir:
            cut(title, pg::kTitleBytes, {kVolName[nv.where().vol], ":", nv.where().path});
    }
    for (int d = 0; d < kDrives; d++) {
        cut(reinterpret_cast<char*>(page + pg::kDrives + d * pg::kDriveNameBytes), pg::kDriveNameBytes, {board.drive_name(d)});
    }
    for (uint32_t r = 0; r < pg::kRowCount; r++) {
        uint8_t* const row = page + pg::kRows + r * pg::kRowBytes;
        std::memset(row, 0, pg::kRowBytes);
        if (first + r >= count) continue;
        const Item& it = nv.item(static_cast<int>(first + r));
        row[0]         = static_cast<uint8_t>(static_cast<uint8_t>(it.kind) | (it.enabled ? 0 : pg::kRowLockedBit) | (it.marked ? pg::kRowMarkedBit : 0));
        cut(reinterpret_cast<char*>(row + 1), pg::kRowBytes - 1, {it.name});
    }
}

uint8_t command(Navigator& nv, Place& picked, const uint8_t* page, uint32_t& first, const Board& board) {
    namespace pg     = nav_page;
    const auto index = static_cast<uint32_t>(page[pg::kArgLo] | (page[pg::kArgHi] << 8));
    const int drive  = page[pg::kArgDrive] & pg::kArgDriveMask;
    switch (page[pg::kCmd]) {
        case pg::Open:
            nv.open();
            return pg::Moved;
        case pg::Window:
            first = index;
            return pg::Ok;
        case pg::Enter:
            switch (nv.enter(static_cast<int>(index), picked)) {
                case Pick::Moved:
                    return pg::Moved;
                case Pick::Image:
                    return pg::Image;
                default:
                    return pg::Fail;
            }
        case pg::Back:
            nv.back();
            return pg::Moved;
        case pg::Mount:
            if (!board.mount(drive, picked, (page[pg::kArgDrive] & pg::kArgReadOnlyBit) != 0)) return pg::Fail;
            nv.remember(picked);
            return pg::Ok;
        case pg::Eject:
            return board.eject(drive) ? pg::Ok : pg::Fail;
        case pg::Mark:
            return nv.mark(static_cast<int>(index));
        case pg::Unmark:
            nv.unmark();
            return pg::Ok;
        case pg::MountSet: {
            // Отметок нет - набор из файла под курсором.
            const bool one = nv.marked() == 0;
            if (one) {
                if (const uint8_t r = nv.mark(static_cast<int>(index)); r != pg::Ok) return r;
            }
            int bad         = static_cast<int>(index);
            const uint8_t r = board.mount_set(drive, nv, bad);
            if (r == pg::Ok || one) nv.unmark();
            nv.point(bad);
            return r;
        }
        default:
            return pg::Ok;
    }
}

} // namespace

void serve(Navigator& nv, Place& picked, uint8_t* page, uint8_t port_7ffd, const Board& board) {
    namespace pg = nav_page;
    if (page[pg::kReq] == page[pg::kAck]) return;
    uint32_t first       = static_cast<uint32_t>(page[pg::kFirst] | (page[pg::kFirst + 1] << 8));
    const uint8_t result = command(nv, picked, page, first, board);
    render(nv, page, result == pg::Moved ? 0 : first, port_7ffd, board);
    page[pg::kResult] = result;
    std::atomic_signal_fence(std::memory_order_release);
    page[pg::kAck] = page[pg::kReq];
}

void Navigator::init(Item* items, int cap, Memory* mem, const Media& media) {
    items_ = items;
    cap_   = cap;
    mem_   = mem;
    media_ = media;
    if (mem_->magic != kMagic || mem_->sum != sum_of(*mem_) || mem_->recent_count > kRecent) {
        std::memset(mem_, 0, sizeof(Memory));
        mem_->magic = kMagic;
        save();
    }
}

void Navigator::save() {
    mem_->sum = sum_of(*mem_);
}

// Единственный пункт верхнего уровня, в который можно войти.
bool Navigator::single_top(int& idx) const {
    int n = 0;
    for (int i = 0; i < count_; i++) {
        if (items_[i].enabled) {
            idx = i;
            n++;
        }
    }
    return n == 1;
}

// Список пуст, курсор в начале, отметок нет.
void Navigator::clear() {
    count_       = 0;
    cursor_      = 0;
    marked_      = 0;
    marked_secs_ = 0;
}

// Первый пункт списка - выход на уровень выше.
void Navigator::up_item() {
    clear();
    items_[count_++] = Item{Kind::Dir, 0, true, {}, false, 0};
    cut(items_[0].name, kNameBytes, {kUp});
}

void Navigator::cursor_to(Kind kind, const char* name) {
    for (int i = 0; i < count_; i++) {
        if (items_[i].kind == kind && std::strcmp(items_[i].name, name) == 0) cursor_ = i;
    }
}

void Navigator::show_top() {
    level_ = nav_page::Level::Top;
    clear();
    for (int v = 0; v < kVolumes && count_ < cap_; v++) {
        Item& it = items_[count_++];
        it       = Item{Kind::Volume, static_cast<uint8_t>(v), v != kVolWifi && media_.present(v), {}, false, 0};
        cut(it.name, kNameBytes, {kVolName[v]});
    }
    if (count_ < cap_) {
        Item& it = items_[count_++];
        it       = Item{Kind::Recent, 0, mem_->recent_count != 0, {}, false, 0};
        cut(it.name, kNameBytes, {"RECENT"});
    }
}

bool Navigator::show(const Place& p) {
    if (p.vol >= kVolumes || p.vol == kVolWifi || !media_.present(p.vol)) return false;
    const int n = media_.list(p.vol, p.path, items_ + 1, cap_ - 1);
    if (n < 0) return false;
    std::sort(items_ + 1, items_ + 1 + n, before);
    up_item();
    count_ += n;
    level_  = nav_page::Level::Dir;
    at_     = p;
    return true;
}

void Navigator::open() {
    if (mem_->last.path[0] != 0 && show(mem_->last)) return;
    show_top();
    int only = 0;
    if (single_top(only)) {
        Place picked;
        enter(only, picked);
    }
}

Pick Navigator::enter(int i, Place& picked) {
    if (i < 0 || i >= count_) return Pick::None;
    const Item it = items_[i];
    switch (it.kind) {
        case Kind::Volume: {
            if (!it.enabled) return Pick::None;
            Place p{it.vol, "/"};
            if (!show(p)) return Pick::None;
            break;
        }
        case Kind::Recent:
            if (!it.enabled) return Pick::None;
            level_ = nav_page::Level::Recent;
            up_item();
            for (int k = 0; k < mem_->recent_count && count_ < cap_; k++) {
                Item& r = items_[count_++];
                r       = Item{Kind::RecentImage, mem_->recent[k].vol, true, {}, false, 0};
                cut(r.name, kNameBytes, {base_name(mem_->recent[k].path)});
            }
            break;
        case Kind::Dir: {
            if (std::strcmp(it.name, kUp) == 0) {
                back();
                return Pick::Moved;
            }
            Place p = at_;
            if (!join(p.path, it.name) || !show(p)) return Pick::None;
            break;
        }
        case Kind::Image:
            picked = at_;
            return join(picked.path, it.name) ? Pick::Image : Pick::None;
        case Kind::RecentImage:
            picked = mem_->recent[i - 1];
            return Pick::Image;
        case Kind::File:
            return Pick::None;
    }
    if (level_ == nav_page::Level::Dir) {
        mem_->last = at_;
        save();
    }
    return Pick::Moved;
}

void Navigator::back() {
    if (level_ == nav_page::Level::Top) return;
    const Place from  = at_;
    char* const slash = std::strrchr(at_.path, '/');
    if (level_ == nav_page::Level::Recent || slash == nullptr || std::strcmp(at_.path, "/") == 0) {
        const bool recent = level_ == nav_page::Level::Recent;
        show_top();
        cursor_to(recent ? Kind::Recent : Kind::Volume, recent ? "RECENT" : kVolName[from.vol]);
        return;
    }
    Place up = at_;
    if (slash == at_.path) {
        up.path[1] = 0;
    } else {
        up.path[slash - at_.path] = 0;
    }
    if (!show(up)) {
        show_top();
        return;
    }
    cursor_to(Kind::Dir, base_name(from.path));
    mem_->last = at_;
    save();
}

uint8_t Navigator::mark(int i) {
    namespace pg = nav_page;
    if (i < 0 || i >= count_ || items_[i].kind != Kind::File) return pg::Fail;
    Item& it            = items_[i];
    const uint32_t secs = set_sectors(it.name, it.size);
    if (it.marked) {
        marked_--;
        marked_secs_ -= secs;
    } else {
        if (secs == 0 || secs > kFileSectors) return pg::TooLong;
        if (marked_ == static_cast<int>(kSetFiles)) return pg::TooMany;
        if (marked_secs_ + secs > kSetSectors) return pg::NoRoom;
        marked_++;
        marked_secs_ += secs;
    }
    it.marked = !it.marked;
    return pg::Ok;
}

void Navigator::unmark() {
    for (int i = 0; i < count_; i++) {
        items_[i].marked = false;
    }
    marked_      = 0;
    marked_secs_ = 0;
}

void Navigator::remember(const Place& image) {
    int n = mem_->recent_count;
    for (int k = 0; k < n; k++) {
        if (mem_->recent[k].vol == image.vol && std::strcmp(mem_->recent[k].path, image.path) == 0) {
            std::memmove(&mem_->recent[k], &mem_->recent[k + 1], sizeof(Place) * static_cast<size_t>(n - 1 - k));
            n--;
            break;
        }
    }
    if (n == kRecent) n--;
    std::memmove(&mem_->recent[1], &mem_->recent[0], sizeof(Place) * static_cast<size_t>(n));
    mem_->recent[0]    = image;
    mem_->recent_count = static_cast<uint8_t>(n + 1);
    save();
}

} // namespace player::nav
