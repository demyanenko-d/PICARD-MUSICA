// SPDX-License-Identifier: MIT
// Виртуальный диск FAT32: том читает настоящий FatFs, листинги и файлы
// отдаёт подставной поставщик.
#include "testing.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif
#include "ff.h"

#include "net/vdisk/vdisk.h"
#include "pc/fatfs_disk.h"

using namespace net::vdisk;

namespace {

// Ключи: старшие разряды - вид, младшие - номер.
constexpr Key kDirKey  = 1ull << 40;
constexpr Key kFileKey = 2ull << 40;

constexpr Key dir_key(uint32_t n) {
    return kDirKey | n;
}
constexpr Key file_key(uint32_t n) {
    return kFileKey | n;
}

constexpr uint32_t kGames     = 300;
constexpr uint32_t kBigDirs   = 40;
constexpr uint32_t kBigFiles  = 2000;
constexpr uint32_t kTrdBytes  = 655360;
constexpr uint32_t kBrokenDir = 6;

uint8_t content(Key key, uint32_t pos) {
    const uint64_t v = (key * 2654435761ull) ^ (pos * 40503ull) ^ (pos >> 9);
    return static_cast<uint8_t>(v >> 7);
}

struct Node {
    std::string name;
    Key key;
    uint32_t size;
    bool dir;
};

std::string game_name(uint32_t i) {
    char b[80];
    std::snprintf(b, sizeof b, "Game number %03u with a long title.trd", static_cast<unsigned>(i));
    return b;
}

uint32_t game_size(uint32_t i) {
    return i < 10u ? kTrdBytes : kTrdBytes - i * 1000u - 7u;
}

std::vector<Node> children(Key dir) {
    std::vector<Node> out;
    if (dir == kRoot) {
        out.push_back({"zxart", dir_key(1), 0, true});
    } else if (dir == dir_key(1)) {
        out.push_back({"games", dir_key(2), 0, true});
        out.push_back({"MUSIC", dir_key(3), 0, true});
        out.push_back({"big", dir_key(5), 0, true});
        out.push_back({"broken", dir_key(kBrokenDir), 0, true});
        out.push_back({"huge", dir_key(7), 0, true});
        out.push_back({"dups", dir_key(8), 0, true});
    } else if (dir == dir_key(2)) {
        for (uint32_t i = 0; i < kGames; ++i)
            out.push_back({game_name(i), file_key(i), game_size(i), false});
        out.push_back({"README.TXT", file_key(900), 100, false});
        out.push_back({"empty.txt", file_key(901), 0, false});
        out.push_back({"Too big.tzx", file_key(902), kFileMax + 1u, false});
    } else if (dir == dir_key(3)) {
        // Имена UTF-8: кириллица, надстрочные знаки, серия одной основы.
        for (const char* n :
             {"\xd0\xa7\xd1\x91\xd1\x80\xd0\xbd\xd1\x8b\xd0\xb9 \xd0\xb2\xd0\xbe\xd1\x80\xd0\xbe\xd0\xbd.trd",
              "Artiller\xc3\xad"
              "a",
              "Arkanoid", "Arkanoid II", "Arkanoid III", "Arkanoid IV", "Arkanoid V", "Arkanoid VI", "a+b=c.tap", "\xd0\xb8\xd0\xb3\xd1\x80\xd0\xb0"})
            out.push_back({n, file_key(950000 + static_cast<uint32_t>(out.size())), 10, false});
    } else if (dir == dir_key(7)) {
        for (uint32_t k = 0; k <= kDirEntriesMax; ++k)
            out.push_back({"e" + std::to_string(k), file_key(800000 + k), 0, false});
    } else if (dir == dir_key(8)) {
        for (const char* n : {"Same", "same", "Same", "Ab:c", "Ab?c", "games", "x.b.c", "x.b.c"})
            out.push_back({n, file_key(900000 + static_cast<uint32_t>(out.size())), 10, false});
    } else if (dir == dir_key(5)) {
        for (uint32_t j = 0; j < kBigDirs; ++j) {
            char b[32];
            std::snprintf(b, sizeof b, "dir %02u", static_cast<unsigned>(j));
            out.push_back({b, dir_key(100 + j), 0, true});
        }
    } else if (dir >= dir_key(100) && dir < dir_key(100 + kBigDirs)) {
        const uint32_t j = static_cast<uint32_t>(dir - dir_key(100));
        for (uint32_t k = 0; k < kBigFiles; ++k) {
            char b[48];
            std::snprintf(b, sizeof b, "file %04u of dir %02u.bin", static_cast<unsigned>(k), static_cast<unsigned>(j));
            out.push_back({b, file_key(10000 + j * kBigFiles + k), 1000 + k, false});
        }
    }
    return out;
}

std::map<Key, int> g_fetches;
bool g_manual = false; // поставщик молчит: тест отдаёт данные сам

// Приём файла в окно: не больше места, остаток - следующим вызовом.
struct Xfer {
    Key key       = 0;
    uint32_t size = 0, pos = 0;
    bool on = false;
} g_xfer;

uint32_t file_size(Key key) {
    if ((key & ~0xffffffffull) != kFileKey) return 0;
    const uint32_t n = static_cast<uint32_t>(key & 0xffffffffull);
    if (n < kGames) return game_size(n);
    if (n == 900u) return 100u;
    if (n >= 900000u) return 10u;
    return 1000u + (n - 10000u) % kBigFiles;
}

void feed() {
    std::vector<uint8_t> piece(1000);
    while (g_xfer.pos < g_xfer.size) {
        const uint32_t room = vdisk_file_room();
        if (room == 0u) return;
        uint32_t len = g_xfer.size - g_xfer.pos < 1000u ? g_xfer.size - g_xfer.pos : 1000u;
        if (len > room) len = room;
        for (uint32_t i = 0; i < len; ++i) {
            piece[i] = content(g_xfer.key, g_xfer.pos + i);
        }
        if (!vdisk_file_data(piece.data(), len)) {
            g_xfer.on = false;
            return;
        }
        g_xfer.pos += len;
    }
    g_xfer.on = false;
    vdisk_file_end(true);
}

void serve() {
    const Want w = vdisk_want();
    if (g_xfer.on) {
        if (w.kind == WantKind::File && w.key == g_xfer.key) {
            feed();
            return;
        }
        g_xfer.on = false; // машине нужно другое - приём брошен
        vdisk_file_end(false);
    }
    if (w.kind == WantKind::List) {
        ++g_fetches[w.key];
        CHECK(vdisk_list_begin(w.key));
        if (w.key == dir_key(kBrokenDir)) {
            vdisk_list_end(false);
            return;
        }
        for (const Node& n : children(w.key)) {
            Entry e;
            e.key  = n.key;
            e.size = n.size;
            e.dir  = n.dir;
            e.year = 1995;
            std::snprintf(e.name, sizeof e.name, "%s", n.name.c_str());
            vdisk_list_add(e);
        }
        vdisk_list_end(true);
    } else if (w.kind == WantKind::File) {
        ++g_fetches[w.key];
        CHECK(vdisk_file_begin(w.key));
        g_xfer = Xfer{w.key, file_size(w.key), 0, true};
        feed();
    }
}

uint32_t g_fat_reads = 0;

bool reader(unsigned long lba, unsigned char* out) {
    if (lba >= kFatStart && lba < kDataStart) ++g_fat_reads;
    for (int tries = 0; tries < 4; ++tries) {
        const ReadResult r = vdisk_read(static_cast<uint32_t>(lba), out);
        if (r == ReadResult::Ready) return true;
        if (r == ReadResult::Error) return false;
        if (g_manual) return false;
        serve();
    }
    return false;
}

uint8_t* identity(uint8_t* p, size_t) {
    return p;
}

std::vector<uint8_t> g_mem;
FATFS g_fs;

void mount() {
    g_mem.assign(kMemoryBytes, 0xcc);
    CHECK(vdisk_init(g_mem.data(), static_cast<uint32_t>(g_mem.size()), &identity));
    g_fetches.clear();
    g_manual = false;
    g_xfer   = Xfer{};
    platform_pc::mount_sector_reader(&reader);
    CHECK_EQ(f_mount(&g_fs, "", 1), FR_OK);
}

void unmount() {
    f_mount(nullptr, "", 0);
    platform_pc::mount_sector_reader(nullptr);
}

std::vector<FILINFO> list(const char* path, FRESULT* res = nullptr) {
    std::vector<FILINFO> out;
    DIR d;
    const FRESULT r = f_opendir(&d, path);
    if (res) *res = r;
    if (r != FR_OK) return out;
    FILINFO fi;
    for (;;) {
        const FRESULT rr = f_readdir(&d, &fi);
        if (rr != FR_OK && res) *res = rr;
        if (rr != FR_OK || fi.fname[0] == '\0') break;
        out.push_back(fi);
    }
    f_closedir(&d);
    return out;
}

bool file_matches(const char* path, Key key, uint32_t size) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    bool ok = f_size(&f) == size;
    std::vector<uint8_t> buf(size + 1u);
    UINT got = 0;
    if (f_read(&f, buf.data(), size + 1u, &got) != FR_OK || got != size) ok = false;
    for (uint32_t i = 0; ok && i < size; ++i)
        ok = buf[i] == content(key, i);
    f_close(&f);
    return ok;
}

void test_geometry() {
    std::printf("test_vdisk_geometry\n");
    CHECK_EQ(kFatSectors, 2016u);
    // Том в пределах геометрии CHS 16383 x 16 x 63 и трёхбайтового LBA.
    CHECK(kTotalSectors <= 16383u * 16u * 63u);
    CHECK(kTotalSectors <= (1u << 24));
    CHECK(static_cast<uint64_t>(kTotalSectors) * 512u > 7ull * 1024 * 1024 * 1024);
}

void test_mount_and_free() {
    std::printf("test_vdisk_mount_and_free\n");
    mount();
    FATFS* fs     = nullptr;
    DWORD free_cl = 1;
    g_fat_reads   = 0;
    CHECK_EQ(f_getfree("", &free_cl, &fs), FR_OK);
    CHECK_EQ(free_cl, 0u);
    CHECK_EQ(g_fs.fs_type, FS_FAT32);
    CHECK_EQ(g_fs.n_fatent, kClusters + 2u);
    CHECK_EQ(g_fs.csize, kClusterSectors);
    // Свободное из FSInfo: FAT не перебиралась.
    CHECK(g_fat_reads < 4u);
    unmount();
}

void test_listing_names() {
    std::printf("test_vdisk_listing_names\n");
    mount();
    const auto root = list("/");
    CHECK_EQ(root.size(), 1u);
    if (!root.empty()) CHECK(std::strcmp(root[0].fname, "zxart") == 0);
    const auto top = list("/zxart");
    CHECK_EQ(top.size(), 6u);
    if (top.size() == 6u) {
        CHECK(std::strcmp(top[1].fname, "MUSIC") == 0);
        CHECK((top[1].fattrib & AM_DIR) != 0);
    }
    const auto games = list("/zxart/games");
    // Файл больше ячейки не показывается.
    CHECK_EQ(games.size(), kGames + 2u);
    std::set<std::string> shorts;
    bool names_ok = true;
    for (uint32_t i = 0; i < kGames && i < games.size(); ++i) {
        names_ok = names_ok && game_name(i) == games[i].fname && games[i].fsize == game_size(i);
        shorts.insert(games[i].altname);
    }
    CHECK(names_ok);
    CHECK_EQ(shorts.size(), static_cast<size_t>(kGames));
    if (games.size() == kGames + 2u) {
        CHECK(std::strcmp(games[kGames].fname, "README.TXT") == 0);
        CHECK(std::strcmp(games[kGames + 1u].fname, "empty.txt") == 0);
        CHECK_EQ(games[kGames + 1u].fsize, 0u);
        CHECK_EQ(games[0].fdate, static_cast<WORD>(((1995 - 1980) << 9) | (1 << 5) | 1));
        CHECK((games[0].fattrib & AM_RDO) != 0);
    }
    CHECK_EQ(vdisk_stats().entries_dropped, 1u);
    unmount();
}

void test_file_contents() {
    std::printf("test_vdisk_file_contents\n");
    mount();
    CHECK(file_matches("/zxart/games/Game number 000 with a long title.trd", file_key(0), game_size(0)));
    CHECK(file_matches("/zxart/games/Game number 123 with a long title.trd", file_key(123), game_size(123)));
    CHECK(file_matches("/zxart/games/README.TXT", file_key(900), 100));
    FIL f;
    CHECK_EQ(f_open(&f, "/zxart/games/empty.txt", FA_READ), FR_OK);
    CHECK_EQ(f_size(&f), 0u);
    f_close(&f);
    // Окно держит только читаемый файл: к прежнему - снова из сети.
    CHECK(file_matches("/zxart/games/Game number 000 with a long title.trd", file_key(0), game_size(0)));
    CHECK_EQ(g_fetches[file_key(0)], 2);
    CHECK_EQ(g_fetches[dir_key(2)], 1);
    unmount();
}

// Образ 640 КБ идёт через окно 64 КБ: машина читает подряд, приём не
// обгоняет её больше окна. Возврат к началу файла - за окном, файл заново.
void test_stream_window() {
    std::printf("test_vdisk_stream_window\n");
    mount();
    char path[96];
    std::snprintf(path, sizeof path, "/zxart/games/%s", game_name(5).c_str());
    CHECK(file_matches(path, file_key(5), kTrdBytes));
    CHECK_EQ(g_fetches[file_key(5)], 1);
    CHECK(file_matches(path, file_key(5), kTrdBytes));
    CHECK_EQ(g_fetches[file_key(5)], 2);
    CHECK(vdisk_stats().stream_restarts >= 1u);
    unmount();
}

// Обход 40 каталогов по 2000 записей - вдесятеро больше ячеек. Ячейки и
// листинги вытесняются, но живое читается верно.
void test_slot_reuse() {
    std::printf("test_vdisk_slot_reuse\n");
    mount();
    char path[96];
    bool ok = true;
    for (uint32_t j = 0; j < kBigDirs; ++j) {
        std::snprintf(path, sizeof path, "/zxart/big/dir %02u", static_cast<unsigned>(j));
        const auto entries = list(path);
        ok                 = ok && entries.size() == kBigFiles;
        // Файл из только что прочитанного каталога.
        const uint32_t k = (j * 37u) % kBigFiles;
        std::snprintf(path, sizeof path, "/zxart/big/dir %02u/file %04u of dir %02u.bin", static_cast<unsigned>(j), static_cast<unsigned>(k),
                      static_cast<unsigned>(j));
        ok = ok && file_matches(path, file_key(10000 + j * kBigFiles + k), 1000 + k);
    }
    CHECK(ok);
    const Stats st = vdisk_stats();
    CHECK(st.slots_reused > 0u);
    CHECK(st.dirs_evicted > 0u);
    // Первый каталог давно вытеснен: читается заново и верно.
    const auto again = list("/zxart/big/dir 00");
    CHECK_EQ(again.size(), static_cast<size_t>(kBigFiles));
    CHECK_EQ(g_fetches[dir_key(100)], 2);
    CHECK(file_matches("/zxart/big/dir 00/file 1999 of dir 00.bin", file_key(10000 + 1999), 2999));
    CHECK(file_matches("/zxart/games/Game number 007 with a long title.trd", file_key(7), kTrdBytes));
    unmount();
}

void test_listing_failure() {
    std::printf("test_vdisk_listing_failure\n");
    mount();
    // Отказ - каталог с одной записью ERROR, не ошибка диска.
    CHECK_EQ(list("/zxart/broken").size(), static_cast<size_t>(1));
    CHECK_EQ(list("/zxart/broken").size(), static_cast<size_t>(1));
    CHECK_EQ(g_fetches[dir_key(kBrokenDir)], 2);
    CHECK_EQ(list("/zxart/games").size(), static_cast<size_t>(kGames + 2u));
    unmount();
}

// Сектор, который уже пришёл, читается до конца приёма файла; место в окне
// убывает на принятое; машина ушла к другому файлу - приём брошен.
void test_progressive_file() {
    std::printf("test_vdisk_progressive_file\n");
    mount();
    FIL f;
    CHECK_EQ(f_open(&f, "/zxart/games/Game number 042 with a long title.trd", FA_READ), FR_OK);
    const uint32_t lba = kDataStart + (f.obj.sclust - kRootCluster) * kClusterSectors;
    f_close(&f);
    g_manual = true;
    uint8_t sec[kSectorBytes];
    CHECK(vdisk_read(lba, sec) == ReadResult::Pending);
    const Want w = vdisk_want();
    CHECK(w.kind == WantKind::File && w.key == file_key(42));
    CHECK(vdisk_file_begin(file_key(42)));
    std::vector<uint8_t> part(700);
    for (uint32_t i = 0; i < 700u; ++i)
        part[i] = content(file_key(42), i);
    CHECK(vdisk_file_data(part.data(), 700));
    CHECK_EQ(vdisk_file_room(), kStreamBytes - 700u);
    CHECK(vdisk_read(lba, sec) == ReadResult::Ready);
    CHECK(sec[0] == content(file_key(42), 0) && sec[511] == content(file_key(42), 511));
    CHECK(vdisk_read(lba + 1u, sec) == ReadResult::Pending);
    // Машина ушла читать другой файл посреди приёма: приём первого брошен.
    CHECK_EQ(f_open(&f, "/zxart/games/Game number 043 with a long title.trd", FA_READ), FR_OK);
    const uint32_t other_lba = kDataStart + (f.obj.sclust - kRootCluster) * kClusterSectors;
    f_close(&f);
    uint8_t other[kSectorBytes];
    CHECK(vdisk_read(other_lba, other) == ReadResult::Pending);
    CHECK(vdisk_want().key == file_key(43));
    CHECK(!vdisk_file_data(part.data(), 700));
    CHECK_EQ(vdisk_stats().files_dropped, 1u);
    // Отказ приёма получает тот, кто ждал: одно чтение - Error, следующее
    // просит заново.
    CHECK(vdisk_file_begin(file_key(43)));
    vdisk_file_end(false);
    CHECK(vdisk_read(other_lba, other) == ReadResult::Error);
    CHECK(vdisk_read(other_lba, other) == ReadResult::Pending);
    unmount();
}

// Каталог ещё в пути: с placeholder первый сектор - заглушка, ожидание
// заведено; когда листинг пришёл - настоящий каталог.
void test_placeholder() {
    std::printf("test_vdisk_placeholder\n");
    mount();
    g_manual = true;
    uint8_t sec[kSectorBytes];
    const uint32_t root = kDataStart;
    CHECK(vdisk_read(root, sec, false) == ReadResult::Pending);
    CHECK(vdisk_read(root, sec, true) == ReadResult::Ready);
    // Корень: метка тома, затем LOADING.
    CHECK(std::memcmp(sec, "ZX-ART     ", 11) == 0);
    CHECK(std::memcmp(sec + 32, "LOADING    ", 11) == 0);
    CHECK(vdisk_want().kind == WantKind::List && vdisk_want().key == kRoot);
    CHECK_EQ(vdisk_stats().placeholders, 1u);
    serve();
    CHECK(vdisk_read(root, sec, true) == ReadResult::Ready);
    CHECK(std::memcmp(sec + 32, "ZXART      ", 11) == 0);
    unmount();
}

// Листинг принимается: заглушка - метка тома, пришедшие записи, LOADING.
void test_placeholder_partial() {
    std::printf("test_vdisk_placeholder_partial\n");
    mount();
    g_manual = true;
    uint8_t sec[kSectorBytes];
    CHECK(vdisk_read(kDataStart, sec, false) == ReadResult::Pending);
    CHECK(vdisk_list_begin(kRoot));
    Entry e;
    e.key = dir_key(1);
    e.dir = true;
    std::snprintf(e.name, sizeof e.name, "%s", "ZXART");
    CHECK(vdisk_list_add(e));
    CHECK(vdisk_read(kDataStart, sec, true) == ReadResult::Ready);
    CHECK(std::memcmp(sec, "ZX-ART     ", 11) == 0 && sec[11] == 0x08);
    CHECK(std::memcmp(sec + 32, "ZXART      ", 11) == 0 && sec[32 + 11] == 0x10);
    CHECK(std::memcmp(sec + 64, "LOADING    ", 11) == 0 && sec[64 + 11] == 0x21);
    CHECK(sec[96] == 0u);
    vdisk_list_end(true);
    CHECK(vdisk_read(kDataStart, sec, false) == ReadResult::Ready);
    CHECK(std::memcmp(sec + 32, "ZXART      ", 11) == 0 && sec[64] == 0u);
    unmount();
}

// Каталог, который ещё не читали: заглушка с "." и "..".
void test_placeholder_subdir() {
    std::printf("test_vdisk_placeholder_subdir\n");
    mount();
    list("/zxart");
    FILINFO fi;
    CHECK_EQ(f_stat("/zxart/games", &fi), FR_OK);
    DIR d;
    CHECK_EQ(f_opendir(&d, "/zxart/games"), FR_OK);
    const uint32_t lba = kDataStart + (d.obj.sclust - kRootCluster) * kClusterSectors;
    f_closedir(&d);
    g_manual = true;
    uint8_t sec[kSectorBytes];
    CHECK(vdisk_read(lba, sec, true) == ReadResult::Ready);
    CHECK(sec[0] == '.' && sec[32] == '.' && sec[33] == '.');
    CHECK(std::memcmp(sec + 64, "LOADING    ", 11) == 0);
    unmount();
}

// После листинга заранее ничего не берётся: файл качается, когда его
// читают, а машина ждёт на /WAIT.
void test_no_ahead() {
    std::printf("test_vdisk_no_ahead\n");
    mount();
    list("/zxart/games");
    g_manual = true;
    CHECK(vdisk_want().kind == WantKind::None);
    g_manual = false;
    CHECK(file_matches("/zxart/games/Game number 000 with a long title.trd", file_key(0), game_size(0)));
    unmount();
}

uint32_t first_cluster(const char* path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    const uint32_t c = f.obj.sclust;
    f_close(&f);
    return c;
}

// Повторный листинг возвращает те же кластеры; образ, который читают по
// запомненному адресу, переживает обход больших каталогов.
void test_stable_clusters() {
    std::printf("test_vdisk_stable_clusters\n");
    mount();
    const uint32_t c0      = first_cluster("/zxart/big/dir 00/file 0005 of dir 00.bin");
    const uint32_t mounted = first_cluster("/zxart/games/Game number 005 with a long title.trd");
    CHECK(c0 != 0u && mounted != 0u);
    const uint32_t lba = kDataStart + (mounted - kRootCluster) * kClusterSectors;
    uint8_t sec[kSectorBytes];
    char path[96];
    bool image_ok = true;
    for (uint32_t j = 0; j < kBigDirs; ++j) {
        std::snprintf(path, sizeof path, "/zxart/big/dir %02u", static_cast<unsigned>(j));
        list(path);
        // Эмулятор дисковода читает смонтированный образ.
        image_ok = image_ok && reader(lba + 3u, sec) && sec[0] == content(file_key(5), 3u * 512u);
        // Пока ячейки не кончились, повторный листинг - те же кластеры.
        if (j == 2u) {
            CHECK_EQ(first_cluster("/zxart/big/dir 00/file 0005 of dir 00.bin"), c0);
            CHECK(vdisk_stats().slots_kept > 0u);
        }
    }
    CHECK(image_ok);
    unmount();
}

void test_names() {
    std::printf("test_vdisk_names\n");
    mount();
    std::string joined;
    for (const auto& fi : list("/zxart/dups")) {
        joined += (joined.empty() ? "" : "|") + std::string(fi.fname);
    }
    // Повторы без учёта регистра нумеруются, запрещённые знаки - '_'; номер
    // встаёт перед последней точкой.
    CHECK_EQ(joined, std::string("Same|same (2)|Same (3)|Ab_c|Ab_c (2)|games|x.b.c|x.b (2).c"));
    // Имя 8.3 в одном регистре - без хвоста ~N.
    const auto top = list("/zxart");
    if (top.size() == 6u) {
        CHECK(std::strcmp(top[0].fname, "games") == 0);
        CHECK(std::strchr(top[0].altname, '~') == nullptr);
    }
    // Каталог больше kDirEntriesMax обрезается.
    CHECK_EQ(list("/zxart/huge").size(), static_cast<size_t>(kDirEntriesMax));
    CHECK(vdisk_stats().entries_dropped >= 1u);
    unmount();
}

// Записи каталога как лежат: длинное имя UTF-16 и короткое 11 байт.
struct RawEntry {
    std::u16string lfn;
    std::string sfn;
};

std::vector<RawEntry> raw_dir(const char* path) {
    static constexpr uint8_t kPos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    std::vector<RawEntry> out;
    DIR d;
    if (f_opendir(&d, path) != FR_OK) return out;
    const uint32_t cluster = d.obj.sclust;
    f_closedir(&d);
    std::u16string lfn;
    uint8_t sec[kSectorBytes];
    for (uint32_t s = 0; s < kClusterSectors; ++s) {
        if (!reader(kDataStart + (cluster - kRootCluster) * kClusterSectors + s, sec)) break;
        for (uint32_t o = 0; o < kSectorBytes; o += 32u) {
            const uint8_t* e = sec + o;
            if (e[0] == 0u) return out;
            if (e[11] == 0x0fu) {
                std::u16string part;
                for (uint8_t p : kPos) {
                    const auto ch = static_cast<char16_t>(e[p] | (e[p + 1u] << 8));
                    if (ch == 0u || ch == 0xffffu) break;
                    part += ch;
                }
                lfn = part + lfn;
                continue;
            }
            if (e[0] != '.') out.push_back({lfn, std::string(reinterpret_cast<const char*>(e), 11)});
            lfn.clear();
        }
    }
    return out;
}

// Длинные имена - UTF-16 с кириллицей, надстрочные знаки - основой
// ASCII; короткие - как у Windows в cp866: точная основа без хвоста,
// иначе ~1..~4, дальше два знака и хэш.
void test_names_unicode() {
    std::printf("test_vdisk_names_unicode\n");
    mount();
    const auto e = raw_dir("/zxart/MUSIC");
    CHECK_EQ(e.size(), 10u);
    if (e.size() == 10u) {
        CHECK(e[0].lfn == std::u16string({0x427, 0x451, 0x440, 0x43d, 0x44b, 0x439, ' ', 0x432, 0x43e, 0x440, 0x43e, 0x43d, '.', 't', 'r', 'd'}));
        CHECK(e[0].sfn == std::string("\x97\xf0\x90\x8d\x9b\x89~1TRD", 11));
        CHECK(e[1].lfn == u"Artilleria");
        CHECK(e[1].sfn == "ARTILL~1   ");
        CHECK(e[2].lfn == u"Arkanoid");
        CHECK(e[2].sfn == "ARKANOID   ");
        CHECK(e[3].sfn == "ARKANO~1   ");
        CHECK(e[4].sfn == "ARKANO~2   ");
        CHECK(e[5].sfn == "ARKANO~3   ");
        CHECK(e[6].sfn == "ARKANO~4   ");
        CHECK(e[7].sfn == "ARA706~1   "); // хэш длинного имени

        CHECK(e[8].sfn == "A_B_C~1 TAP");
        CHECK(e[9].lfn == std::u16string({0x438, 0x433, 0x440, 0x430}));
        CHECK(e[9].sfn == std::string("\x88\x83\x90\x80       ", 11));
    }
    unmount();
}

} // namespace

void run_vdisk_tests() {
    test_names_unicode();
    test_geometry();
    test_mount_and_free();
    test_listing_names();
    test_file_contents();
    test_stream_window();
    test_slot_reuse();
    test_listing_failure();
    test_progressive_file();
    test_stable_clusters();
    test_names();
    test_placeholder();
    test_placeholder_partial();
    test_placeholder_subdir();
    test_no_ahead();
}
