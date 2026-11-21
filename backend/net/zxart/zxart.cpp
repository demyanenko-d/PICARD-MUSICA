// SPDX-License-Identifier: MIT
// Поставщик виртуального диска: архив zxart.ee каталогами.

#include "net/zxart/zxart.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "platform/mono_time.h"

#include "net/vdisk/vdisk.h"
#include "net/zxart/json_stream.h"

namespace net::zxart {
namespace {

using vdisk::Entry;
using vdisk::kDirEntriesMax;
using vdisk::Key;
using vdisk::kNameBytes;

// Словарь каталогов по выгрузке сайта. Ключ списка: разряд 31 - авторы,
// раздел 4, год 7, жанр 20.
struct DictList {
    uint32_t key;
    uint16_t first; // первый префикс в kDictPrefixes
    uint16_t count;
};
struct Genre {
    uint8_t section;
    uint32_t id;
    const char* name;
};
struct CategorySection {
    uint32_t id;
    uint8_t section;
};
struct YearMask {
    uint32_t key;  // список названий раздела или жанра
    uint64_t mask; // разряд - поле года (1 - не указан)
};
#include "net/zxart/zxart_dict.inc"

// Ключ каталога: вид 6 разрядов, раздел 4, год 7, жанр 20, младшие 27 -
// номер префикса словаря или id автора и программы. Файл: вид, релиз 29,
// узел дерева файлов 29.
enum Kind : uint32_t {
    kRootKind      = 0,
    kSection       = 1, // names/ authors/ years/ genres/ best/
    kNames         = 2, // названия: буквы словаря или сразу программы
    kNamesLetter   = 3, // префиксы одной буквы
    kNamesPart     = 4, // программы префикса
    kAuthors       = 5, // авторы: буквы словаря или сразу авторы
    kAuthorsLetter = 6,
    kAuthorsPart   = 7,  // авторы префикса
    kAuthorProds   = 8,  // программы автора в разделе, году, жанре
    kYears         = 9,  // годы раздела или жанра
    kYearMenu      = 10, // names/ authors/ года
    kGenreList     = 11,
    kGenreMenu     = 12, // names/ authors/ years/ жанра
    kBest          = 13, // лучшие по оценке
    kAuthor        = 14, // разделы автора
    kAuthorSection = 15, // программы автора в разделе
    kProd          = 16, // файлы-образы программы
    kFile          = 17,
};

constexpr uint32_t kRootSection = 15; // авторы всего сайта
constexpr uint32_t kLowMax      = (1u << 27) - 1u;
constexpr uint32_t kGenreMax    = (1u << 20) - 1u;
constexpr uint32_t kFileIdMax   = (1u << 29) - 1u;
constexpr unsigned kKindLsb     = 58;
constexpr unsigned kSecLsb      = 54;
constexpr unsigned kYearLsb     = 47;
constexpr unsigned kGenreLsb    = 27;
constexpr unsigned kReleaseLsb  = 29;
constexpr uint32_t kSecMask     = 15u;
constexpr uint32_t kYearMask    = 127u;

constexpr Key make_key(uint32_t kind, uint32_t sec = 0, uint32_t year = 0, uint32_t genre = 0, uint32_t low = 0) {
    return (static_cast<Key>(kind) << kKindLsb) | (static_cast<Key>(sec & kSecMask) << kSecLsb) | (static_cast<Key>(year & kYearMask) << kYearLsb) |
           (static_cast<Key>(genre & kGenreMax) << kGenreLsb) | static_cast<Key>(low & kLowMax);
}
constexpr Key file_key(uint32_t release, uint32_t node) {
    return (static_cast<Key>(kFile) << kKindLsb) | (static_cast<Key>(release & kFileIdMax) << kReleaseLsb) | static_cast<Key>(node & kFileIdMax);
}
constexpr uint32_t kind_of(Key k) {
    return static_cast<uint32_t>(k >> kKindLsb);
}
constexpr uint32_t sec_of(Key k) {
    return static_cast<uint32_t>(k >> kSecLsb) & kSecMask;
}
constexpr uint32_t year_of(Key k) {
    return static_cast<uint32_t>(k >> kYearLsb) & kYearMask;
}
constexpr uint32_t genre_of(Key k) {
    return static_cast<uint32_t>(k >> kGenreLsb) & kGenreMax;
}
constexpr uint32_t low_of(Key k) {
    return static_cast<uint32_t>(k) & kLowMax;
}
constexpr uint32_t release_of(Key k) {
    return static_cast<uint32_t>(k >> kReleaseLsb) & kFileIdMax;
}
constexpr uint32_t node_of(Key k) {
    return static_cast<uint32_t>(k) & kFileIdMax;
}
static_assert(make_key(kRootKind) == vdisk::kRoot, "root key");
static_assert(sizeof(kDictPrefixes) / sizeof(kDictPrefixes[0]) <= kLowMax, "prefix index must fit the key");

// Поле года: 0 - нет, 1 - не указан (zxProdYear=0), иначе год - 1978.
constexpr uint32_t kYearUnknown = 1;
constexpr uint32_t year_field(uint32_t year) {
    return year == 0u ? kYearUnknown : year - 1978u;
}
constexpr uint32_t year_value(uint32_t field) {
    return field == kYearUnknown ? 0u : field + 1978u;
}

struct Section {
    const char* name; // 8.3 без хвоста ~
    uint32_t category;
};
constexpr Section kSections[] = {
    {"games", 92177}, {"demos", 204819}, {"system", 92183}, {"applied", 244880}, {"educat", 92534}, {"press", 244858}, {"compils", 202588}, {"misc", 92188},
};
constexpr uint32_t kSectionCount = sizeof(kSections) / sizeof(kSections[0]);
static_assert(kSectionCount < kRootSection, "section index must fit the key");

constexpr uint32_t kFirstYear   = 1982;
constexpr uint32_t kDefaultYear = 2026;
constexpr uint32_t kPageItems   = 200; // авторы, релизы программы: записи короткие
// Программы: около 1 КБ полей на запись, страница по 200 - больше 120 КБ, и
// сайт рвёт такой ответ на середине.
constexpr uint32_t kProdPage    = 100;
constexpr uint32_t kAuthorItems = 1000; // все программы автора одним ответом: разделы
constexpr uint32_t kPathMax     = 512;

// Образы, которые программа Спектрума монтирует или грузит.
constexpr const char* kImageTypes[] = {"trd", "scl", "tap", "tzx", "z80", "sna", "dsk", "mgt", "img", "mdr", "rom", "nex", "szx"};

enum class Job : uint8_t { None, List, File, Prefetch };

struct Work {
    Job job;
    Key key;
    uint32_t start;
    uint32_t total;
    uint32_t records;
    uint32_t skip; // записей страницы, принятых до обрыва: при повторе не добавлять
    uint32_t begun_ms;
    uint32_t activity_ms;
    int status;
    bool need_request;
    bool status_ok;
    uint32_t sections; // разделы автора, по разряду
    uint32_t retries;  // повторов файла
};

// Поля записи и узлов дерева файлов, пока объект не закрыт.
struct Node {
    uint32_t id;
    uint32_t element;
    uint32_t size;
    char type[8];
    char name[JsonStream::kTextMax + 1];
};
constexpr int kNodeLevels = 5; // глубина 6, 8, 10, 12, 14

struct Record {
    uint32_t id;
    uint32_t year;
    uint32_t prod; // программа релиза
    char title[JsonStream::kTextMax + 1];
    Node nodes[kNodeLevels];
};

struct Path {
    char text[kPathMax];
    uint32_t len;
    bool overflow;

    void clear() {
        len      = 0;
        overflow = false;
        text[0]  = '\0';
    }
    Path& add(const char* s) {
        for (; *s != '\0'; ++s) {
            if (len + 1u >= kPathMax) {
                overflow = true;
                break;
            }
            text[len++] = *s;
        }
        text[len] = '\0';
        return *this;
    }
    Path& add(uint32_t v) {
        char d[11];
        char* p = d + sizeof(d) - 1u;
        *p      = '\0';
        do {
            *--p  = static_cast<char>('0' + v % 10u);
            v    /= 10u;
        } while (v != 0u);
        return add(p);
    }
    // Префикс в адресе: буквы и цифры ASCII как есть, прочее - %XX.
    Path& add_escaped(const char* s) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (; *s != '\0'; ++s) {
            const auto c = static_cast<uint8_t>(*s);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                const char one[2] = {static_cast<char>(c), '\0'};
                add(one);
            } else {
                const char three[4] = {'%', kHex[c >> 4], kHex[c & 15u], '\0'};
                add(three);
            }
        }
        return *this;
    }
};

HttpGet s_get       = nullptr;
HttpCancel s_cancel = nullptr;
// Путь живёт от build_path до передачи транспорту (s_get копирует его к
// себе), запись - только в разборе ответа на этот запрос: одна память на
// двоих. Путь начинается с clear(), запись - с начала объекта.
union RecordOrPath {
    Record rec;
    Path path;
};
// Программа пачки заранее: место в очереди, пришли ли её релизы.
struct BatchProd {
    uint32_t id;
    uint32_t slot;
    bool seen;
};

// Работа, запись и пачка - в памяти от порта (PSRAM): читает и пишет только
// цикл Core1.
struct State {
    Work work;
    RecordOrPath rp;
    BatchProd batch[kPrefetchBatch];
};
static_assert(sizeof(State) <= kStateBytes, "zxart state fits its memory");
State* s_z = nullptr;
JsonStream s_json;
Stats s_stats{};
uint32_t s_year = kDefaultYear;

// Срок ms от since; нулевой не действует.
struct Hold {
    uint32_t since;
    uint32_t ms;

    bool active(uint32_t now) const { return now - since < ms; }
};

// Темп запросов: промежуток после запроса, бюджет минуты, паузы после отказов.
struct Throttle {
    Hold gap;   // kGapMs после запроса
    Hold bad;   // kRetryMs после отказа на ключе bad_key
    Hold block; // kBlockMs после 403 или 429
    Key bad_key;
    uint32_t window_ms;
    uint32_t window_count;
};
Throttle s_th{};

// --- Текст ---

uint32_t parse_uint(const char* t) {
    uint32_t v = 0;
    for (; *t >= '0' && *t <= '9'; ++t) {
        v = v * 10u + static_cast<uint32_t>(*t - '0');
    }
    return v;
}

// Строка с обрезкой по размеру приёмника.
void copy_text(char* dst, uint32_t size, const char* src) {
    uint32_t n = 0;
    for (; src[n] != '\0' && n + 1u < size; ++n) {
        dst[n] = src[n];
    }
    dst[n] = '\0';
}

bool same_text(const char* a, const char* b) {
    return std::strcmp(a, b) == 0;
}

// Знак UTF-8 в dst, если влезает целиком.
void put_utf8(char* dst, uint32_t size, uint32_t& n, uint32_t c) {
    char b[3];
    const uint32_t k = utf8_encode(c, b);
    if (n + k >= size) return;
    std::memcpy(dst + n, b, k);
    n += k;
}

// Название с раскрытыми HTML-сущностями: сайт отдаёт "Heaven&#039;s".
void copy_title(char* dst, uint32_t size, const char* src) {
    static constexpr struct {
        const char* name;
        char c;
    } kNamed[] = {{"amp;", '&'}, {"quot;", '"'}, {"apos;", '\''}, {"lt;", '<'}, {"gt;", '>'}, {"nbsp;", ' '}};
    uint32_t n = 0;
    while (*src != '\0' && n + 1u < size) {
        if (*src == '&') {
            const char* p = src + 1;
            uint32_t code = 0;
            bool ok       = false;
            if (*p == '#') {
                const bool hex  = p[1] == 'x' || p[1] == 'X';
                p              += hex ? 2 : 1;
                const char* d   = p;
                for (; (*p >= '0' && *p <= '9') || (hex && ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))); ++p) {
                    const uint32_t v = *p <= '9' ? static_cast<uint32_t>(*p - '0') : static_cast<uint32_t>((*p | 0x20) - 'a' + 10);
                    code             = code * (hex ? 16u : 10u) + v;
                    if (code > 0xffffu) break;
                }
                ok = p != d && *p == ';' && code != 0u && code <= 0xffffu;
                if (ok) ++p;
            } else {
                for (const auto& e : kNamed) {
                    const uint32_t len = static_cast<uint32_t>(std::strlen(e.name));
                    if (std::strncmp(p, e.name, len) == 0) {
                        code  = static_cast<uint8_t>(e.c);
                        p    += len;
                        ok    = true;
                        break;
                    }
                }
            }
            if (ok) {
                put_utf8(dst, size, n, code);
                src = p;
                continue;
            }
        }
        dst[n++] = *src++;
    }
    dst[n] = '\0';
}

bool has_image_ext(const char* name) {
    const char* dot = std::strrchr(name, '.');
    if (dot == nullptr) return false;
    char ext[5];
    uint32_t n = 0;
    for (const char* p = dot + 1; *p != '\0' && n < 4u; ++p) {
        ext[n++] = static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
    }
    ext[n] = '\0';
    for (const char* t : kImageTypes) {
        if (same_text(t, ext)) return true;
    }
    return false;
}

bool is_image(const char* type, const char* name) {
    for (const char* t : kImageTypes) {
        if (same_text(t, type)) return true;
    }
    return same_text(type, "file") && has_image_ext(name);
}

// --- Записи ---

// Имена приходят из JSON и кэша релизов: запись листинга вмещает их целиком.
static_assert(JsonStream::kTextMax <= kNameBytes, "names must fit a listing entry");

void add_entry(Key key, const char* name, bool dir, uint32_t size = 0, uint16_t year = 0) {
    Entry e;
    e.key  = key;
    e.dir  = dir;
    e.size = size;
    e.year = year;
    copy_text(e.name, sizeof(e.name), name);
    vdisk::vdisk_list_add(e);
}

// --- Словарь ---

constexpr uint32_t kDictListCount   = sizeof(kDictLists) / sizeof(kDictLists[0]);
constexpr uint32_t kDictPrefixCount = sizeof(kDictPrefixes) / sizeof(kDictPrefixes[0]);

constexpr uint32_t list_key(bool authors, uint32_t sec, uint32_t year, uint32_t genre) {
    return (authors ? 1u << 31 : 0u) | (sec << 27) | (year << 20) | genre;
}

const DictList* find_list(uint32_t key) {
    const DictList* end = kDictLists + kDictListCount;
    const DictList* l   = std::lower_bound(kDictLists, end, key, [](const DictList& d, uint32_t k) { return d.key < k; });
    return l != end && l->key == key ? l : nullptr;
}

// Список, которому принадлежит префикс.
const DictList* list_of_prefix(uint32_t idx) {
    for (const DictList& l : kDictLists) {
        if (idx >= l.first && idx < static_cast<uint32_t>(l.first) + l.count) return &l;
    }
    return nullptr;
}

uint32_t char_len(const char* s) {
    const auto c = static_cast<uint8_t>(*s);
    if (c < 0x80u) return 1;
    if (c < 0xe0u) return 2;
    return c < 0xf0u ? 3u : 4u;
}

bool same_first(const char* a, const char* b) {
    const uint32_t n = char_len(a);
    return n == char_len(b) && std::memcmp(a, b, n) == 0;
}

// Имя каталога префикса: знак ASCII не буква и не цифра - '_'.
void prefix_name(const char* prefix, uint32_t bytes, char* out) {
    uint32_t i = 0;
    for (; i < bytes && prefix[i] != '\0'; ++i) {
        const auto c   = static_cast<uint8_t>(prefix[i]);
        const bool asc = c < 0x80u;
        const bool ok  = !asc || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        out[i]         = ok ? static_cast<char>(c) : '_';
    }
    out[i] = '\0';
}

// Буквы делимого списка: буква с единственным префиксом из одного знака -
// сразу каталог префикса.
void add_letters(const DictList& l, Key ctx, uint32_t letter_kind, uint32_t part_kind) {
    const uint32_t end = static_cast<uint32_t>(l.first) + l.count;
    for (uint32_t i = l.first; i < end;) {
        uint32_t j = i + 1u;
        while (j < end && same_first(kDictPrefixes[i], kDictPrefixes[j])) {
            ++j;
        }
        const char* p     = kDictPrefixes[i];
        const bool single = j == i + 1u && char_len(p) == std::strlen(p);
        char name[8];
        prefix_name(p, char_len(p), name);
        add_entry(make_key(single ? part_kind : letter_kind, sec_of(ctx), year_of(ctx), genre_of(ctx), i), name, true);
        i = j;
    }
}

// Префиксы одной буквы.
void add_letter_parts(Key k, uint32_t part_kind) {
    const uint32_t idx = low_of(k);
    const DictList* l  = list_of_prefix(idx);
    if (l == nullptr) return;
    const uint32_t end = static_cast<uint32_t>(l->first) + l->count;
    for (uint32_t i = idx; i < end && same_first(kDictPrefixes[i], kDictPrefixes[idx]); ++i) {
        char name[8];
        prefix_name(kDictPrefixes[i], 8, name);
        add_entry(make_key(part_kind, sec_of(k), year_of(k), genre_of(k), i), name, true);
    }
}

uint8_t section_of_category(uint32_t id) {
    const CategorySection* end = kCategorySections + sizeof(kCategorySections) / sizeof(kCategorySections[0]);
    const CategorySection* c   = std::lower_bound(kCategorySections, end, id, [](const CategorySection& s, uint32_t k) { return s.id < k; });
    return c != end && c->id == id ? c->section : 0xffu;
}

uint64_t year_mask(uint32_t sec, uint32_t genre) {
    const uint32_t key = list_key(false, sec, 0, genre);
    for (const YearMask& m : kYearMasks) {
        if (m.key == key) return m.mask;
    }
    return 0;
}

bool prods_kind(uint32_t kind) {
    return kind == kNames || kind == kNamesPart || kind == kAuthorProds || kind == kBest || kind == kAuthorSection;
}

uint32_t page_items(uint32_t kind) {
    return prods_kind(kind) ? kProdPage : kPageItems;
}

bool paged(uint32_t kind) {
    return kind == kNames || kind == kNamesPart || kind == kAuthors || kind == kAuthorsPart || kind == kAuthorProds || kind == kAuthorSection;
}

// --- Кэш релизов ---

// Файлы-образы программ последнего списка, взятые заранее. Память порта:
// очередь id программ списка, кольцо пачек (программа, её файлы) и кольцо
// файлов. Пачка вытесняется целиком: программа или вся в кэше, или нет.
constexpr uint32_t kQueueMax     = 2u * kDirEntriesMax; // последние списки подряд
constexpr uint32_t kCacheChunks  = 1024;
constexpr uint32_t kCacheNameMax = 51;

struct CachedFile {
    uint32_t release;
    uint32_t node;
    uint32_t size;
    char name[kCacheNameMax + 1];
};
static_assert(sizeof(CachedFile) == 64, "cached file record size");

struct Chunk {
    uint32_t prod;
    uint32_t first; // сплошной номер первого файла
    uint16_t count;
    uint16_t batch;
};
static_assert(sizeof(Chunk) == 12, "cache chunk size");

constexpr uint32_t kQueueOff   = 0;
constexpr uint32_t kStateOff   = kQueueOff + kQueueMax * 4u;
constexpr uint32_t kChunkOff   = (kStateOff + kQueueMax + 3u) & ~3u;
constexpr uint32_t kFileOff    = kChunkOff + kCacheChunks * sizeof(Chunk);
constexpr uint32_t kCacheFiles = (kCacheBytes - kFileOff) / sizeof(CachedFile);
static_assert(kCacheFiles >= 1024u, "release cache must hold at least 1024 files");

enum : uint8_t { kQueued = 0, kDone = 1 };

// Кэш в памяти порта, очередь и принимаемая пачка.
struct Cache {
    uint8_t* mem;
    platform::WriteAliasFn alias;
    Key queue_list; // список, из которого очередь
    uint32_t queue_n;
    uint32_t queued;       // записей kQueued: ноль - next_batch не обходит очередь
    uint32_t queue_cursor; // с этого места - следующая пачка
    uint32_t chunk_head;   // сплошные номера
    uint32_t chunk_tail;
    uint32_t file_head;
    uint32_t rec_file0; // первый файл принимаемого релиза
    uint32_t batch_n;
    uint32_t batch_chunk_head0;
    uint32_t batch_file_head0;
    uint16_t batch_serial;
    Hold pause; // kRetryMs после неудачной пачки
};
Cache s_c{};

void mem_write(uint32_t off, const void* src, uint32_t n) {
    std::memcpy(s_c.alias(s_c.mem + off, n), src, n);
}

uint32_t queue_id(uint32_t i) {
    uint32_t v;
    std::memcpy(&v, s_c.mem + kQueueOff + i * 4u, 4);
    return v;
}

uint8_t queue_state(uint32_t i) {
    return s_c.mem[kStateOff + i];
}

void set_queue_state(uint32_t i, uint8_t v) {
    mem_write(kStateOff + i, &v, 1);
}

Chunk get_chunk(uint32_t abs) {
    Chunk c;
    std::memcpy(&c, s_c.mem + kChunkOff + (abs % kCacheChunks) * sizeof(Chunk), sizeof(Chunk));
    return c;
}

// Вытеснить самую старую пачку целиком.
void evict_batch() {
    if (s_c.chunk_tail == s_c.chunk_head) return;
    const uint16_t b = get_chunk(s_c.chunk_tail).batch;
    while (s_c.chunk_tail != s_c.chunk_head && get_chunk(s_c.chunk_tail).batch == b) {
        ++s_c.chunk_tail;
    }
}

void put_chunk(uint32_t prod, uint32_t first, uint32_t count) {
    while (s_c.chunk_head - s_c.chunk_tail >= kCacheChunks) {
        evict_batch();
    }
    const Chunk c = {prod, first, static_cast<uint16_t>(count), s_c.batch_serial};
    mem_write(kChunkOff + (s_c.chunk_head % kCacheChunks) * static_cast<uint32_t>(sizeof(Chunk)), &c, sizeof(Chunk));
    ++s_c.chunk_head;
}

void put_file(const Node& n) {
    // Файлы, которые перезапишет новый, уходят вместе со своими пачками.
    while (s_c.chunk_tail != s_c.chunk_head && get_chunk(s_c.chunk_tail).first + kCacheFiles <= s_c.file_head) {
        evict_batch();
    }
    CachedFile f{};
    f.release = n.element;
    f.node    = n.id;
    f.size    = n.size;
    // Имя - целыми знаками UTF-8, сколько влезает.
    uint32_t len = 0;
    while (n.name[len] != '\0' && len + char_len(n.name + len) <= kCacheNameMax) {
        len += char_len(n.name + len);
    }
    std::memcpy(f.name, n.name, len);
    f.name[len] = '\0';
    mem_write(kFileOff + (s_c.file_head % kCacheFiles) * static_cast<uint32_t>(sizeof(CachedFile)), &f, sizeof(CachedFile));
    ++s_c.file_head;
}

bool cached(uint32_t prod) {
    for (uint32_t i = s_c.chunk_head; i != s_c.chunk_tail; --i) {
        if (get_chunk(i - 1u).prod == prod) return true;
    }
    return false;
}

// Каталог программы из кэша. false - программы в кэше нет.
bool cache_listing(uint32_t prod) {
    if (s_c.mem == nullptr || !cached(prod)) return false;
    for (uint32_t i = s_c.chunk_tail; i != s_c.chunk_head; ++i) {
        const Chunk c = get_chunk(i);
        if (c.prod != prod) continue;
        for (uint32_t k = 0; k < c.count; ++k) {
            CachedFile f;
            std::memcpy(&f, s_c.mem + kFileOff + ((c.first + k) % kCacheFiles) * sizeof(CachedFile), sizeof(CachedFile));
            add_entry(file_key(f.release, f.node), f.name, false, f.size);
        }
    }
    ++s_stats.cache_hits;
    return true;
}

// Новый список программ - в конец очереди, пачки - с него. Прежние
// списки остаются: вернуться в них можно из кэша каталогов vdisk, и вход
// в программу найдёт её в очереди. Не влезает - очередь заново.
void queue_reset(Key list) {
    s_c.queue_list = list;
    if (s_c.queue_n + kDirEntriesMax > kQueueMax) s_c.queue_n = s_c.queued = 0;
    s_c.queue_cursor = s_c.queue_n;
}

void queue_push(uint32_t prod) {
    if (s_c.mem == nullptr || s_c.queue_n >= kQueueMax) return;
    mem_write(kQueueOff + s_c.queue_n * 4u, &prod, 4);
    set_queue_state(s_c.queue_n, kQueued);
    ++s_c.queue_n;
    ++s_c.queued;
}

// Запрошен листинг программы: следующие пачки - от неё.
void queue_focus(uint32_t prod) {
    for (uint32_t i = 0; i < s_c.queue_n; ++i) {
        if (queue_id(i) == prod) {
            s_c.queue_cursor = i;
            return;
        }
    }
}

// Запись взята: счёт - только на настоящем переходе, программа бывает в
// очереди дважды, а после переполнения на месте пачки - чужая запись.
void queue_done(uint32_t i) {
    if (queue_state(i) != kQueued) return;
    set_queue_state(i, kDone);
    --s_c.queued;
}

// Следующая пачка: из очереди от курсора по кругу, без тех, что уже в
// кэше. false - брать нечего.
bool next_batch() {
    s_c.batch_n = 0;
    if (s_c.queued == 0u) return false;
    for (uint32_t n = 0; n < s_c.queue_n && s_c.batch_n < kPrefetchBatch; ++n) {
        const uint32_t i = (s_c.queue_cursor + n) % s_c.queue_n;
        if (queue_state(i) != kQueued) continue;
        const uint32_t id = queue_id(i);
        if (cached(id)) {
            queue_done(i);
            continue;
        }
        s_z->batch[s_c.batch_n++] = BatchProd{id, i, false};
    }
    return s_c.batch_n != 0u;
}

void batch_begin() {
    ++s_c.batch_serial;
    s_c.batch_chunk_head0 = s_c.chunk_head;
    s_c.batch_file_head0  = s_c.file_head;
}

// Пачка не пришла целиком: принятое из неё - вон.
void batch_rollback() {
    s_c.chunk_head = s_c.batch_chunk_head0;
    s_c.file_head  = s_c.batch_file_head0;
    if (s_c.chunk_tail > s_c.chunk_head) s_c.chunk_tail = s_c.chunk_head;
}

// Пачка пришла: программы без образов - пустыми записями, очередь - готово.
void batch_commit() {
    for (uint32_t i = 0; i < s_c.batch_n; ++i) {
        const BatchProd& b = s_z->batch[i];
        if (!b.seen) put_chunk(b.id, s_c.file_head, 0);
        if (b.slot < s_c.queue_n && queue_id(b.slot) == b.id) queue_done(b.slot);
    }
}

// Релиз пачки закрыт: его файлы - программе, если она из пачки.
void batch_release_done(uint32_t prod) {
    for (uint32_t i = 0; i < s_c.batch_n; ++i) {
        if (s_z->batch[i].id != prod) continue;
        s_z->batch[i].seen = true;
        put_chunk(prod, s_c.rec_file0, s_c.file_head - s_c.rec_file0);
        return;
    }
    s_c.file_head = s_c.rec_file0;
}

// --- Ответ ---

// Узлы дерева файлов релиза - на глубине 6, 8, 10 ...
int node_level(uint32_t depth) {
    if (depth < 6u || (depth & 1u) != 0u) return -1;
    const int level = static_cast<int>((depth - 6u) / 2u);
    return level < kNodeLevels ? level : -1;
}

void record_done() {
    if (s_z->work.job == Job::Prefetch) {
        batch_release_done(s_z->rp.rec.prod);
        return;
    }
    ++s_z->work.records;
    if (s_z->work.records <= s_z->work.skip) return;
    const Key k         = s_z->work.key;
    const uint32_t kind = kind_of(k);
    const uint32_t id   = s_z->rp.rec.id;
    if (id == 0u || kind == kAuthor) return;
    if (id > kLowMax) {
        ++s_stats.ids_dropped;
        return;
    }
    if (prods_kind(kind)) {
        add_entry(make_key(kProd, 0, 0, 0, id), s_z->rp.rec.title, true, 0, static_cast<uint16_t>(s_z->rp.rec.year));
        if (k == s_c.queue_list) queue_push(id);
    } else if (kind == kAuthors || kind == kAuthorsPart) {
        const Key to = sec_of(k) == kRootSection ? make_key(kAuthor, 0, 0, 0, id) : make_key(kAuthorProds, sec_of(k), year_of(k), genre_of(k), id);
        add_entry(to, s_z->rp.rec.title, true);
    }
}

void node_done(const Node& n) {
    const bool prefetch = s_z->work.job == Job::Prefetch;
    if ((!prefetch && kind_of(s_z->work.key) != kProd) || !is_image(n.type, n.name) || s_z->work.records < s_z->work.skip) return;
    if (n.element == 0u || n.id == 0u) return;
    if (n.element > kFileIdMax || n.id > kFileIdMax) {
        ++s_stats.ids_dropped;
        return;
    }
    if (prefetch) {
        put_file(n);
    } else {
        add_entry(file_key(n.element, n.id), n.name, false, n.size);
    }
}

void on_open(void*, uint32_t depth, bool array) {
    if (array) return;
    if (depth == 4u) {
        s_z->rp.rec.id       = 0;
        s_z->rp.rec.year     = 0;
        s_z->rp.rec.prod     = 0;
        s_z->rp.rec.title[0] = '\0';
        s_c.rec_file0        = s_c.file_head;
    } else if (node_level(depth) >= 0) {
        s_z->rp.rec.nodes[node_level(depth)] = Node{};
    }
}

void on_close(void*, uint32_t depth, bool array) {
    if (array) return;
    if (depth == 4u) {
        record_done();
    } else if (node_level(depth) >= 0) {
        node_done(s_z->rp.rec.nodes[node_level(depth)]);
    }
}

void top_value(const char* key, const char* text) {
    if (same_text(key, "responseStatus")) {
        s_z->work.status_ok = same_text(text, "success");
    } else if (same_text(key, "totalAmount")) {
        s_z->work.total = parse_uint(text);
    }
}

void on_value(void*, uint32_t depth, const char* key, const char* text, bool) {
    if (depth == 1u) {
        top_value(key, text);
    } else if (depth == 4u) {
        if (same_text(key, "id")) {
            s_z->rp.rec.id = parse_uint(text);
        } else if (same_text(key, "year")) {
            s_z->rp.rec.year = parse_uint(text);
        } else if (same_text(key, "prodId")) {
            s_z->rp.rec.prod = parse_uint(text);
        } else if (same_text(key, "title")) {
            copy_title(s_z->rp.rec.title, sizeof(s_z->rp.rec.title), text);
        }
    } else if (depth == 5u && kind_of(s_z->work.key) == kAuthor && same_text(key, "connectedCategoriesIds")) {
        const uint8_t sec = section_of_category(parse_uint(text));
        if (sec < kSectionCount) s_z->work.sections |= 1u << sec;
    } else if (node_level(depth) >= 0) {
        Node& n = s_z->rp.rec.nodes[node_level(depth)];
        if (same_text(key, "id")) {
            n.id = parse_uint(text);
        } else if (same_text(key, "elementId")) {
            n.element = parse_uint(text);
        } else if (same_text(key, "size")) {
            n.size = parse_uint(text);
        } else if (same_text(key, "type")) {
            copy_text(n.type, sizeof(n.type), text);
        } else if (same_text(key, "fileName")) {
            copy_title(n.name, sizeof(n.name), text);
        }
    }
}

constexpr JsonHandler kHandler = {nullptr, &on_open, &on_close, &on_value};

// --- Запросы ---

// Отбор программ раздела, года и жанра.
void add_context(Path& p, uint32_t sec, uint32_t year, uint32_t genre) {
    p.add("zxProdCategory=").add(genre != 0u ? genre : kSections[sec].category);
    if (year != 0u) p.add(";zxProdYear=").add(year_value(year));
}

void add_images(Path& p) {
    p.add(";zxReleaseFormat=");
    for (uint32_t i = 0; i < sizeof(kImageTypes) / sizeof(kImageTypes[0]); ++i) {
        if (i != 0u) p.add(",");
        p.add(kImageTypes[i]);
    }
}

void build_path() {
    Path& p             = s_z->rp.path;
    const Key k         = s_z->work.key;
    const uint32_t kind = kind_of(k);
    const uint32_t sec  = sec_of(k);
    p.clear();
    if (s_z->work.job == Job::Prefetch) {
        p.add("/api/language:eng/export:zxRelease/limit:1000/filter:zxProdId=");
        for (uint32_t i = 0; i < s_c.batch_n; ++i) {
            if (i != 0u) p.add(",");
            p.add(s_z->batch[i].id);
        }
        return;
    }
    if (kind == kFile) {
        p.add("/zxfile/id:").add(release_of(k)).add("/fileId:").add(node_of(k)).add("/f");
        return;
    }
    p.add("/api/language:eng/");
    switch (kind) {
        case kProd:
            p.add("export:zxRelease/limit:").add(kPageItems).add("/filter:zxProdId=").add(low_of(k));
            return;
        case kBest:
            p.add("export:zxProd/limit:").add(kProdPage).add("/order:votes,desc/filter:");
            add_context(p, sec, 0, 0);
            add_images(p);
            return;
        case kAuthor:
            // Без фильтра форматов: вместе с ним сайт молча бросает authorId и
            // отдаёт все программы.
            p.add("export:zxProd/limit:").add(kAuthorItems).add("/filter:authorId=").add(low_of(k));
            return;
        case kAuthors:
        case kAuthorsPart:
            p.add("export:author/start:").add(s_z->work.start).add("/limit:").add(kPageItems).add("/order:title,asc");
            if (sec != kRootSection) {
                p.add("/filter:");
                add_context(p, sec, year_of(k), genre_of(k));
                if (kind == kAuthorsPart) p.add(";authorFirstLetter=").add_escaped(kDictPrefixes[low_of(k)]);
            } else if (kind == kAuthorsPart) {
                p.add("/filter:authorFirstLetter=").add_escaped(kDictPrefixes[low_of(k)]);
            }
            return;
        default:
            break;
    }
    p.add("export:zxProd/start:").add(s_z->work.start).add("/limit:").add(kProdPage).add("/order:title,asc/filter:");
    const bool by_author = kind == kAuthorProds || kind == kAuthorSection;
    if (by_author) p.add("authorId=").add(low_of(k)).add(";");
    add_context(p, sec, kind == kAuthorSection ? 0u : year_of(k), kind == kAuthorSection ? 0u : genre_of(k));
    if (kind == kNamesPart) p.add(";zxProdFirstLetter=").add_escaped(kDictPrefixes[low_of(k)]);
    // У автора - все его программы: фильтр форматов сайт с authorId не
    // складывает; программа без образов - пустой каталог.
    if (!by_author) add_images(p);
}

// Конец работы: диску - итог листинга или файла; из неудачной пачки
// заранее принятое - вон.
void end_job(bool ok) {
    const Job job = s_z->work.job;
    s_z->work.job = Job::None;
    if (job == Job::List) vdisk::vdisk_list_end(ok);
    if (job == Job::File) vdisk::vdisk_file_end(ok);
    if (job == Job::Prefetch && !ok) batch_rollback();
}

// Пачка заранее не удалась: предзагрузка ждёт kRetryMs.
void drop_prefetch() {
    ++s_stats.prefetch_failed;
    s_c.pause = Hold{platform::mono_ms(), kRetryMs};
    end_job(false);
}

// Отказ: диску - ошибка, ключу - пауза до следующей попытки.
void fail_job() {
    if (s_z->work.job == Job::Prefetch) {
        drop_prefetch();
        return;
    }
    ++s_stats.failed;
    s_th.bad     = Hold{platform::mono_ms(), kRetryMs};
    s_th.bad_key = s_z->work.key;
    end_job(false);
}

// Обрыв или молчание: файл или страница листинга - заново, пока есть
// повторы. Файл сайт отдаёт только целиком, докачки нет.
bool retry_job() {
    if (s_z->work.job == Job::None || s_z->work.job == Job::Prefetch || s_z->work.retries >= kRetries) return false;
    ++s_z->work.retries;
    ++s_stats.retries;
    if (s_z->work.job == Job::File) {
        vdisk::vdisk_file_restart();
    } else if (s_z->work.records > s_z->work.skip) {
        // Сайт отдаёт страницу в том же порядке: принятое пропускается.
        s_z->work.skip = s_z->work.records;
    }
    s_z->work.need_request = true;
    s_z->work.begun_ms     = platform::mono_ms();
    return true;
}

// Транспорт занят или сети нет. Пачку заранее не ждать; листинг или файл
// - повторить на следующем витке, а дольше kQueueMs - отказ, иначе
// листинг ждёт без конца.
void send_refused(uint32_t now) {
    if (s_z->work.job == Job::Prefetch) {
        end_job(false);
    } else if (now - s_z->work.begun_ms >= kQueueMs) {
        ++s_stats.busy;
        fail_job();
    } else {
        s_z->work.need_request = true;
    }
}

// Послать отложенный запрос, если позволяют пауза и бюджет минуты.
void try_send(uint32_t now) {
    if (s_th.gap.active(now)) return;
    if (now - s_th.window_ms >= 60000u) {
        s_th.window_ms    = now;
        s_th.window_count = 0;
    }
    if (s_th.window_count >= kRequestsPerMinute) {
        if (now - s_z->work.begun_ms >= kQueueMs) {
            ++s_stats.throttled;
            fail_job();
        }
        return;
    }
    build_path();
    if (s_z->rp.path.overflow) {
        ++s_stats.format_errors;
        fail_job();
        return;
    }
    s_z->work.need_request = false;
    s_z->work.status       = 0;
    s_z->work.status_ok    = false;
    s_z->work.records      = 0;
    s_z->work.activity_ms  = now;
    s_json.reset(&kHandler);
    if (s_get == nullptr || !s_get(s_z->rp.path.text, s_z->work.job == Job::File)) {
        send_refused(now);
        return;
    }
    ++s_stats.requests;
    s_th.gap = Hold{now, kGapMs};
    ++s_th.window_count;
}

void add_year(uint32_t sec, uint32_t genre, uint32_t field) {
    const uint32_t y = year_value(field);
    char name[8]     = "unknown";
    if (y != 0u) {
        name[0] = static_cast<char>('0' + y / 1000u);
        name[1] = static_cast<char>('0' + y / 100u % 10u);
        name[2] = static_cast<char>('0' + y / 10u % 10u);
        name[3] = static_cast<char>('0' + y % 10u);
        name[4] = '\0';
    }
    const Key to = genre == 0u ? make_key(kYearMenu, sec, field) : make_key(kNames, sec, field, genre);
    add_entry(to, name, true, 0, static_cast<uint16_t>(y));
}

// Годы, в которых у раздела или жанра есть программы; позже выгрузки -
// все до текущего.
void add_years(uint32_t sec, uint32_t genre) {
    const uint64_t mask  = year_mask(sec, genre);
    const uint32_t built = kDictBuilt / 10000u;
    for (uint32_t y = kFirstYear; y <= s_year; ++y) {
        const uint32_t f = year_field(y);
        if (y > built || (f < 64u && ((mask >> f) & 1u) != 0u)) add_year(sec, genre, f);
    }
    if ((mask & (1ull << kYearUnknown)) != 0u) add_year(sec, genre, kYearUnknown);
}

// Листинги без сети. false - каталог из сети.
bool static_listing(Key k) {
    const uint32_t kind  = kind_of(k);
    const uint32_t sec   = sec_of(k);
    const uint32_t year  = year_of(k);
    const uint32_t genre = genre_of(k);
    switch (kind) {
        case kRootKind:
            add_entry(make_key(kAuthors, kRootSection), "authors", true);
            for (uint32_t i = 0; i < kSectionCount; ++i) {
                add_entry(make_key(kSection, i), kSections[i].name, true);
            }
            return true;
        case kSection:
            add_entry(make_key(kNames, sec), "names", true);
            add_entry(make_key(kAuthors, sec), "authors", true);
            add_entry(make_key(kYears, sec), "years", true);
            add_entry(make_key(kGenreList, sec), "genres", true);
            add_entry(make_key(kBest, sec), "best", true);
            return true;
        case kNames:
        case kAuthors: {
            const DictList* l = find_list(list_key(kind == kAuthors, sec, year, genre));
            if (l == nullptr) return false;
            if (kind == kNames) {
                add_letters(*l, k, kNamesLetter, kNamesPart);
            } else {
                add_letters(*l, k, kAuthorsLetter, kAuthorsPart);
            }
            return true;
        }
        case kNamesLetter:
            add_letter_parts(k, kNamesPart);
            return true;
        case kAuthorsLetter:
            add_letter_parts(k, kAuthorsPart);
            return true;
        case kYears:
            add_years(sec, genre);
            return true;
        case kYearMenu:
            add_entry(make_key(kNames, sec, year), "names", true);
            add_entry(make_key(kAuthors, sec, year), "authors", true);
            return true;
        case kGenreList:
            for (const Genre& g : kGenres) {
                if (g.section == sec) add_entry(make_key(kGenreMenu, sec, 0, g.id), g.name, true);
            }
            return true;
        case kGenreMenu:
            add_entry(make_key(kNames, sec, 0, genre), "names", true);
            add_entry(make_key(kAuthors, sec, 0, genre), "authors", true);
            add_entry(make_key(kYears, sec, 0, genre), "years", true);
            return true;
        case kProd:
            return cache_listing(low_of(k));
        default:
            return false;
    }
}

bool key_valid(Key k) {
    const uint32_t kind = kind_of(k);
    const uint32_t sec  = sec_of(k);
    switch (kind) {
        case kRootKind:
        case kProd:
        case kAuthor:
        case kFile:
            return true;
        case kAuthors:
        case kAuthorsLetter:
        case kAuthorsPart:
            if (sec != kRootSection && sec >= kSectionCount) return false;
            break;
        default:
            if (sec >= kSectionCount) return false;
            break;
    }
    if (kind == kNamesLetter || kind == kNamesPart || kind == kAuthorsLetter || kind == kAuthorsPart) return low_of(k) < kDictPrefixCount;
    return kind <= kFile;
}

void finish_listing() {
    end_job(true);
    ++s_stats.listings;
}

void list_page_done() {
    const uint32_t kind = kind_of(s_z->work.key);
    if (kind == kAuthor) {
        for (uint32_t i = 0; i < kSectionCount; ++i) {
            if ((s_z->work.sections & (1u << i)) != 0u) add_entry(make_key(kAuthorSection, i, 0, 0, low_of(s_z->work.key)), kSections[i].name, true);
        }
    } else if (paged(kind)) {
        const uint32_t limit = std::min(s_z->work.total, kDirEntriesMax);
        const uint32_t page  = page_items(kind);
        if (s_z->work.start + page < limit) {
            s_z->work.start        += page;
            s_z->work.need_request  = true;
            s_z->work.skip          = 0;
            s_z->work.retries       = 0;
            return;
        }
    }
    finish_listing();
}

void begin_work(Job job, Key key, uint32_t now) {
    s_z->work             = Work{};
    s_z->work.job         = job;
    s_z->work.key         = key;
    s_z->work.begun_ms    = now;
    s_z->work.activity_ms = now;
}

// Диск ничего не ждёт (vdisk_want пуст): следующая пачка релизов заранее.
// Предзагрузка берёт не больше половины бюджета минуты.
void start_prefetch(uint32_t now) {
    if (s_c.mem == nullptr || s_th.block.active(now) || s_c.pause.active(now) || s_th.gap.active(now)) return;
    if (now - s_th.window_ms < 60000u && s_th.window_count >= kRequestsPerMinute / 2u) return;
    if (!next_batch()) return;
    begin_work(Job::Prefetch, 0, now);
    s_z->work.need_request = true;
    batch_begin();
    try_send(now);
}

// Ключ недавно отказал или сайт велел ждать: отказ без запроса.
bool held_back(Key key, uint32_t now) {
    return s_th.block.active(now) || (key == s_th.bad_key && s_th.bad.active(now));
}

} // namespace

void zxart_init(HttpGet get, HttpCancel cancel, uint8_t* state, uint8_t* cache, platform::WriteAliasFn to_write) {
    s_z            = new (state) State{};
    s_get          = get;
    s_cancel       = cancel;
    s_c            = Cache{};
    s_c.mem        = cache != nullptr && to_write != nullptr ? cache : nullptr;
    s_c.alias      = to_write;
    s_th           = Throttle{};
    s_th.window_ms = platform::mono_ms();
    s_stats        = Stats{};
    s_year         = kDefaultYear;
}

void zxart_set_year(uint32_t year) {
    if (year > kDefaultYear && year < 2200u) s_year = year;
}

void zxart_poll() {
    if (s_z == nullptr) return;
    const uint32_t now = platform::mono_ms();
    // У диска есть запрос - пачка заранее бросается.
    if (s_z->work.job == Job::Prefetch && vdisk::vdisk_want().kind != vdisk::WantKind::None) {
        if (!s_z->work.need_request && s_cancel != nullptr) s_cancel();
        end_job(false);
    }
    if (s_z->work.job == Job::File && !s_z->work.need_request) {
        // vdisk_want - уже не этот файл: приём бросается, окно не нужно.
        const vdisk::Want w = vdisk::vdisk_want();
        if (w.kind != vdisk::WantKind::File || w.key != s_z->work.key) {
            if (s_cancel != nullptr) s_cancel();
            end_job(false);
            return;
        }
        // Окно полно: чтение секторов отстаёт от сети, соединение ждёт -
        // это не молчание.
        if (vdisk::vdisk_file_room() == 0u) s_z->work.activity_ms = now;
    }
    if (s_z->work.job != Job::None) {
        if (s_z->work.need_request) {
            try_send(now);
        } else if (now - s_z->work.activity_ms >= kQuietMs) {
            ++s_stats.timeouts;
            if (s_cancel != nullptr) s_cancel();
            if (!retry_job()) fail_job();
        }
        return;
    }
    const vdisk::Want w = vdisk::vdisk_want();
    if (w.kind == vdisk::WantKind::List) {
        if (!vdisk::vdisk_list_begin(w.key)) return;
        begin_work(Job::List, w.key, now);
        if (!key_valid(w.key)) {
            ++s_stats.format_errors;
            fail_job();
            return;
        }
        if (kind_of(w.key) == kProd) queue_focus(low_of(w.key));
        if (static_listing(w.key)) {
            finish_listing();
            return;
        }
        if (prods_kind(kind_of(w.key))) queue_reset(w.key);
    } else if (w.kind == vdisk::WantKind::File) {
        if (kind_of(w.key) != kFile || !vdisk::vdisk_file_begin(w.key)) return;
        begin_work(Job::File, w.key, now);
    } else {
        start_prefetch(now);
        return;
    }
    if (held_back(w.key, now)) {
        ++s_stats.throttled;
        fail_job();
        return;
    }
    s_z->work.need_request = true;
    try_send(now);
}

void zxart_http_status(int code) {
    if (s_z->work.job == Job::None) return;
    s_z->work.status      = code;
    s_z->work.activity_ms = platform::mono_ms();
    if (code == 403 || code == 429) {
        s_th.block = Hold{s_z->work.activity_ms, kBlockMs};
    }
}

void zxart_http_body(const uint8_t* p, uint32_t n) {
    if (s_z->work.job == Job::None || s_z->work.need_request || s_z->work.status != 200) return;
    s_z->work.activity_ms = platform::mono_ms();
    if (s_z->work.job == Job::List || s_z->work.job == Job::Prefetch) s_json.feed(p, n);
    if (s_z->work.job == Job::File && !vdisk::vdisk_file_data(p, n)) {
        // Окно отдано другому файлу: этот приём брошен.
        if (s_cancel != nullptr) s_cancel();
        end_job(false);
    }
}

uint32_t zxart_http_room() {
    if (s_z == nullptr || s_z->work.job != Job::File || s_z->work.need_request) return 0xffffffffu;
    return vdisk::vdisk_file_room();
}

void zxart_http_end(bool ok) {
    if (s_z->work.job == Job::None || s_z->work.need_request) return;
    if (s_z->work.job == Job::Prefetch) {
        if (ok && s_z->work.status == 200 && !s_json.failed() && s_json.done() && s_z->work.status_ok) {
            batch_commit();
            ++s_stats.prefetches;
            end_job(true);
        } else {
            drop_prefetch();
        }
        return;
    }
    if (!ok || s_z->work.status != 200) {
        // Повтор - после обрыва, ошибки сервера, 400 и 408; отказ сайта
        // (404, 403, 429) повтором не лечится. 400 на рабочий адрес бывает
        // от испорченного по дороге запроса: тот же файл со второго раза
        // отдаётся.
        const int st         = s_z->work.status;
        const bool transient = st == 200 || st == 0 || st == 400 || st == 408 || st >= 500;
        if (!transient || !retry_job()) fail_job();
        return;
    }
    if (s_z->work.job == Job::File) {
        end_job(true);
        ++s_stats.files;
        return;
    }
    if (s_json.failed() || !s_json.done()) {
        ++s_stats.json_errors;
        fail_job();
        return;
    }
    if (!s_z->work.status_ok) {
        ++s_stats.format_errors;
        fail_job();
        return;
    }
    list_page_done();
}

Stats zxart_stats() {
    Stats st  = s_stats;
    st.queued = s_c.queued;
    return st;
}

} // namespace net::zxart
