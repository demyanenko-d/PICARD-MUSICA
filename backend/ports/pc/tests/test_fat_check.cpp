// SPDX-License-Identifier: MIT
// Проверка тома FAT по секторам: тома собраны в памяти, пустые сектора -
// нули.
#include "testing.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>

#include "devices/storage/fat_check.h"
#include "devices/storage/partition.h"

using namespace devices::storage;

namespace {

using Sector = std::array<uint8_t, 512>;

struct Image {
    std::map<uint32_t, Sector> s;
    uint32_t writes = 0;

    uint8_t* at(uint32_t lba) {
        auto it = s.find(lba);
        if (it == s.end()) it = s.emplace(lba, Sector{}).first;
        return it->second.data();
    }
};

bool img_read(void* user, uint32_t lba, uint8_t* dst) {
    auto& img = *static_cast<Image*>(user);
    auto it   = img.s.find(lba);
    if (it == img.s.end()) {
        std::memset(dst, 0, 512);
    } else {
        std::memcpy(dst, it->second.data(), 512);
    }
    return true;
}

bool img_write(void* user, uint32_t lba, const uint8_t* src) {
    auto& img = *static_cast<Image*>(user);
    std::memcpy(img.at(lba), src, 512);
    ++img.writes;
    return true;
}

void put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<uint32_t>(p[3]) << 24);
}

void boot(uint8_t* b, uint32_t spc, uint32_t reserved, uint32_t root_ents) {
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    put16(b + 11, 512);
    b[13] = static_cast<uint8_t>(spc);
    put16(b + 14, reserved);
    b[16] = 2;
    put16(b + 17, root_ents);
    b[510] = 0x55;
    b[511] = 0xAA;
}

constexpr uint32_t kPart     = 2048;
constexpr uint32_t kClusters = 100000;
constexpr uint32_t kFatSize  = (kClusters + 2u) * 4u / 512u + 1u;
constexpr uint32_t kRsvd     = 32;

// FAT32 в разделе: корень, файл на два кластера, плохой кластер, битая
// ссылка; FSINFO врёт о свободном месте.
void make_fat32(Image& img, bool clean) {
    uint8_t* mbr = img.at(0);
    mbr[446 + 4] = 0x0C;
    put32(mbr + 446 + 8, kPart);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    uint8_t* b = img.at(kPart);
    boot(b, 8, kRsvd, 0);
    put32(b + 32, kRsvd + 2u * kFatSize + kClusters * 8u);
    put32(b + 36, kFatSize);
    put16(b + 48, 1);
    std::memcpy(b + 71, "TESTVOL    ", 11);

    uint8_t* fsi = img.at(kPart + 1);
    put32(fsi, 0x41615252u);
    put32(fsi + 484, 0x61417272u);
    put32(fsi + 488, 12345);
    put32(fsi + 492, 3);
    put32(fsi + 508, 0xAA550000u);

    for (uint32_t copy = 0; copy < 2; ++copy) {
        uint8_t* fat = img.at(kPart + kRsvd + copy * kFatSize);
        put32(fat + 0, 0x0FFFFFF8u);
        put32(fat + 4, clean ? 0x0FFFFFFFu : 0x07FFFFFFu);
        put32(fat + 8, 0x0FFFFFFFu);  // 2: корень
        put32(fat + 12, 4);           // 3 -> 4
        put32(fat + 16, 0x0FFFFFFFu); // 4: конец
        put32(fat + 20, 0x0FFFFFF7u); // 5: плохой
        put32(fat + 24, 0x00500000u); // 6: за пределами тома
    }
    // Копии расходятся в одном секторе.
    img.at(kPart + kRsvd + kFatSize + 10)[7] = 1;
}

devices::hal::SectorIo io_of(Image& img) {
    return devices::hal::SectorIo{&img_read, &img_write, &img};
}

void run_check(const devices::hal::SectorIo& io, const FatVolume& v, FatCheck& c) {
    uint8_t buf[1024];
    fat_check_begin(c);
    uint32_t steps = 0;
    while (fat_check_step(io, v, c, buf, 8)) {
        ++steps;
    }
    CHECK_EQ(c.sector, v.fat_sectors);
    CHECK(steps > 0);
}

void test_fat32_counts_and_fixes() {
    Image img;
    make_fat32(img, true);
    const devices::hal::SectorIo io = io_of(img);
    uint8_t sector[512];
    FatVolume v;
    CHECK(fat_volume_open(io, sector, v));
    CHECK_EQ(v.part_lba, kPart);
    CHECK_EQ(v.fat_bits, 32u);
    CHECK_EQ(v.clusters, kClusters);
    CHECK_EQ(v.cluster_bytes, 4096u);
    CHECK_EQ(v.fsinfo_lba, kPart + 1u);
    CHECK_EQ(v.fsinfo_free, 12345u);
    CHECK(std::strcmp(v.label, "TESTVOL") == 0);

    FatCheck c;
    run_check(io, v, c);
    CHECK_EQ(c.free, kClusters - 5u);
    CHECK_EQ(c.first_free, 7u);
    CHECK_EQ(c.bad, 1u);
    CHECK_EQ(c.invalid, 1u);
    CHECK_EQ(c.copies_differ, 1u);
    CHECK(!c.dirty);
    CHECK(!c.io_error);
    CHECK(fat_fsinfo_wrong(v, c));

    CHECK(fat_fix_fsinfo(io, v, c, sector));
    CHECK_EQ(img.writes, 1u);
    CHECK_EQ(get32(img.at(kPart + 1) + 488), kClusters - 5u);
    CHECK_EQ(get32(img.at(kPart + 1) + 492), 7u);
    FatVolume again;
    CHECK(fat_volume_open(io, sector, again));
    CHECK_EQ(again.fsinfo_free, kClusters - 5u);
    CHECK(!fat_fsinfo_wrong(again, c));

    // Счётчик верный, подсказка пустая или за томом.
    for (const uint32_t next : {kFatNoValue, 1u, kClusters + 2u}) {
        again.fsinfo_next = next;
        CHECK(fat_fsinfo_next_wrong(again, c));
        CHECK(fat_fsinfo_wrong(again, c));
    }
}

// Подсказка перед занятым участком: поиск с неё шёл бы через него. До
// восьми секторов FAT - терпимо.
void test_fat32_hint_before_used_run() {
    for (const uint32_t used : {100u, 9u * 128u}) {
        Image img;
        make_fat32(img, true);
        put32(img.at(kPart + 1) + 492, 200);
        for (uint32_t copy = 0; copy < 2; ++copy) {
            for (uint32_t k = 200; k < 200 + used; ++k) {
                put32(img.at(kPart + kRsvd + copy * kFatSize + k / 128u) + (k % 128u) * 4u, 0x0FFFFFFFu);
            }
        }
        const devices::hal::SectorIo io = io_of(img);
        uint8_t sector[512];
        FatVolume v;
        CHECK(fat_volume_open(io, sector, v));
        FatCheck c;
        run_check(io, v, c);
        CHECK_EQ(c.free_after_hint, 200u + used);
        CHECK_EQ(fat_fsinfo_next_wrong(v, c), used > 8u * 128u);
        if (used <= 8u * 128u) continue;
        CHECK(fat_fix_fsinfo(io, v, c, sector));
        CHECK_EQ(get32(img.at(kPart + 1) + 492), 7u);
        CHECK(!fat_fsinfo_next_wrong(v, c));
    }
}

void test_fat32_dirty() {
    Image img;
    make_fat32(img, false);
    const devices::hal::SectorIo io = io_of(img);
    uint8_t sector[512];
    FatVolume v;
    CHECK(fat_volume_open(io, sector, v));
    FatCheck c;
    run_check(io, v, c);
    CHECK(c.dirty);
}

// FAT16 без таблицы разделов: FSINFO нет, чинить нечего.
void test_fat16_superfloppy() {
    Image img;
    constexpr uint32_t clusters = 10000;
    constexpr uint32_t fat_size = (clusters + 2u) * 2u / 512u + 1u;
    uint8_t* b                  = img.at(0);
    boot(b, 4, 1, 512);
    put16(b + 19, 1u + 2u * fat_size + 32u + clusters * 4u);
    put16(b + 22, fat_size);
    std::memcpy(b + 43, "SMALL      ", 11);
    for (uint32_t copy = 0; copy < 2; ++copy) {
        uint8_t* fat = img.at(1 + copy * fat_size);
        put16(fat + 0, 0xFFF8u);
        put16(fat + 2, 0x7FFFu); // том не закрыт
        put16(fat + 4, 0xFFFFu);
    }
    const devices::hal::SectorIo io = io_of(img);
    uint8_t sector[512];
    FatVolume v;
    CHECK(fat_volume_open(io, sector, v));
    CHECK_EQ(v.part_lba, 0u);
    CHECK_EQ(v.fat_bits, 16u);
    CHECK_EQ(v.clusters, clusters);
    CHECK_EQ(v.fsinfo_lba, 0u);
    CHECK(std::strcmp(v.label, "SMALL") == 0);
    FatCheck c;
    run_check(io, v, c);
    CHECK_EQ(c.free, clusters - 1u);
    CHECK_EQ(c.first_free, 3u);
    CHECK(c.dirty);
    CHECK_EQ(c.copies_differ, 0u);
    CHECK(!fat_fsinfo_wrong(v, c));
    CHECK(!fat_fix_fsinfo(io, v, c, sector));
}

void test_not_fat() {
    Image img;
    const devices::hal::SectorIo io = io_of(img);
    uint8_t sector[512];
    FatVolume v;
    CHECK(!fat_volume_open(io, sector, v));
}

// Где том: без таблицы, основной раздел после чужого, логический в цепочке
// расширенного, GPT; exFAT и NTFS - не FAT.
void test_find_volume() {
    uint8_t sector[512];
    auto entry = [](uint8_t* table, int i, uint8_t type, uint32_t lba) {
        table[446 + i * 16 + 4] = type;
        put32(table + 446 + i * 16 + 8, lba);
        table[510] = 0x55;
        table[511] = 0xAA;
    };
    {
        Image img;
        boot(img.at(0), 8, 32, 0);
        CHECK_EQ(find_fat_volume(&img_read, &img, sector), 0u);
    }
    {
        Image img;
        entry(img.at(0), 0, 0x83, 100); // чужой раздел
        entry(img.at(0), 1, 0x0C, 4096);
        boot(img.at(4096), 8, 32, 0);
        CHECK_EQ(find_fat_volume(&img_read, &img, sector), 4096u);
    }
    {
        // Расширенный с 1000: первая EBR - логический NTFS, вторая (с
        // 1000 + 5000) - логический FAT со смещением 63 от неё.
        Image img;
        entry(img.at(0), 0, 0x0F, 1000);
        entry(img.at(1000), 0, 0x07, 63);
        entry(img.at(1000), 1, 0x05, 5000);
        uint8_t* ntfs = img.at(1063);
        boot(ntfs, 8, 0, 0);
        ntfs[16] = 0;
        entry(img.at(6000), 0, 0x0C, 63);
        boot(img.at(6063), 8, 32, 0);
        CHECK_EQ(find_fat_volume(&img_read, &img, sector), 6063u);
    }
    {
        // GPT: таблица с LBA 2, запись 1 - "основные данные" с 34000.
        static const uint8_t kBasic[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
        Image img;
        entry(img.at(0), 0, 0xEE, 1);
        uint8_t* h = img.at(1);
        std::memcpy(h, "EFI PART", 8);
        put32(h + 72, 2);
        put32(h + 80, 128);
        put32(h + 84, 128);
        std::memcpy(img.at(2) + 128, kBasic, 16);
        put32(img.at(2) + 128 + 32, 34000);
        boot(img.at(34000), 8, 32, 0);
        CHECK_EQ(find_fat_volume(&img_read, &img, sector), 34000u);
        // exFAT: поле размера сектора обнулено.
        put16(img.at(34000) + 11, 0);
        CHECK_EQ(find_fat_volume(&img_read, &img, sector), kNoFatVolume);
    }
}

} // namespace

void run_fat_check_tests() {
    std::printf("test_fat_check\n");
    test_fat32_counts_and_fixes();
    test_fat32_hint_before_used_run();
    test_fat32_dirty();
    test_fat16_superfloppy();
    test_not_fat();
    test_find_volume();
}
