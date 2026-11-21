// SPDX-License-Identifier: MIT
// Beta Disk: кодирование дорожек и контроллер ВГ93 через регистры, как его
// видит Z80. Облегчённые события сверяются с полной моделью: один и тот же
// сценарий с fast = true и fast = false даёт одинаковое состояние после
// каждого обращения.
#include "testing.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/util/fnv1a.h"
#include "devices/bdi/bdi.h"
#include "vg93_ref/beta_disk.h"
#include "vg93_ref/disk_image.h"
#include "vg93_ref/image_source.h"
#include "player/nav/nav.h"
#include "player/nav/nav_page.h"
#include "devices/bdi/disk.h"
#include "vg93_ref/mfm.h"

using namespace devices::bdi;
namespace nav      = player::nav;
namespace nav_page = player::nav_page;

namespace {

// Номер регистра ВГ93 (адресные линии A0..A1 микросхемы).
enum : uint8_t { kRegCmd = 0, kRegTrk = 1, kRegSec = 2, kRegData = 3 };

// Диск в памяти: потоки дорожек по цилиндрам и сторонам.
struct MemDisk {
    std::vector<uint8_t> bits[kMaxCylinders][2];
    SlotDrive drive;

    static uint8_t* track_for(void* user, SlotDrive&, int cyl, int side, bool create) {
        auto* d = static_cast<MemDisk*>(user);
        if (cyl < 0 || cyl >= kMaxCylinders || side < 0 || side > 1) return nullptr;
        auto& t = d->bits[cyl][side];
        if (t.empty() && create) t.assign(kTrackBytes, 0);
        return t.empty() ? nullptr : t.data();
    }
    MemDisk() {
        drive.track_for = &MemDisk::track_for;
        drive.user      = this;
    }
    MemDisk(const MemDisk&)            = delete;
    MemDisk& operator=(const MemDisk&) = delete;
};

uint8_t pattern(int cyl, int side, int r, int i) {
    return static_cast<uint8_t>(cyl * 31 + side * 7 + r * 13 + i * 5 + (i >> 4));
}

// Дорожка TR-DOS: 16 секторов по 256 байт, порядок 1, 9, 2, 10, ...
void format_trdos(MemDisk& d, int cyl, int side) {
    static uint8_t data[16][256];
    SectorSpec s[16];
    for (int i = 0; i < 16; i++) {
        const int r = (i & 1) == 0 ? i / 2 + 1 : i / 2 + 9;
        for (int k = 0; k < 256; k++) {
            data[i][k] = pattern(cyl, side, r, k);
        }
        s[i] = SectorSpec{static_cast<uint8_t>(cyl), static_cast<uint8_t>(side), static_cast<uint8_t>(r), 1, false, data[i], 0};
    }
    auto& t = d.bits[cyl][side];
    t.assign(kTrackBytes, 0);
    build_mfm_track(t.data(), s, 16, 54);
}

void test_mfm_roundtrip() {
    std::printf("test_bdi_mfm_roundtrip\n");
    static uint8_t bits[kTrackBytes];
    static uint8_t data[16][256];
    SectorSpec s[16];
    for (int i = 0; i < 16; i++) {
        for (int k = 0; k < 256; k++) {
            data[i][k] = pattern(3, 1, i + 1, k);
        }
        s[i] = SectorSpec{3, 1, static_cast<uint8_t>(i + 1), 1, i == 5, data[i], 0};
    }
    s[7].bad_crc = kBadDataCrc;
    s[9].bad_crc = kBadIdCrc;
    build_mfm_track(bits, s, 16, 54);
    static FoundSector f[32];
    static uint8_t out[16 * 256];
    const int n = decode_mfm_track(bits, kSlotsPerRev, f, 32, out, sizeof out);
    CHECK_EQ(n, 16);
    for (int i = 0; i < n && i < 16; i++) {
        CHECK_EQ(f[i].r, i + 1);
        CHECK_EQ(f[i].id_crc_ok, i != 9);
        CHECK(f[i].has_data);
        CHECK_EQ(f[i].data_crc_ok, i != 7);
        CHECK_EQ(f[i].deleted, i == 5);
        CHECK(std::memcmp(out + f[i].data_off, data[i], 256) == 0);
    }
}

// Дорожка из одних синхрослов 0x4489 (WRITE TRACK сплошь байтом #F5):
// секторов нет, и разбор обязан кончиться.
void test_mfm_all_sync() {
    std::printf("test_bdi_mfm_all_sync\n");
    static uint8_t bits[kTrackBytes];
    std::memset(bits, 0, sizeof bits);
    for (uint32_t i = 0; i < kSlotsPerRev; i++) {
        if (((0x4489u >> (15u - i % 16u)) & 1u) != 0u) bits[i >> 3] = static_cast<uint8_t>(bits[i >> 3] | (1u << (i & 7u)));
    }
    static FoundSector f[32];
    static uint8_t out[16 * 256];
    CHECK_EQ(decode_mfm_track(bits, kSlotsPerRev, f, 32, out, sizeof out), 0);
}

// Поток ячеек build_mfm_track: свёртка FNV.
uint32_t mfm_hash(const SectorSpec* s, int count, int gap3) {
    static uint8_t bits[kTrackBytes];
    build_mfm_track(bits, s, count, gap3);
    return soundsinth::util::fnv1a(soundsinth::util::kFnvSeed, bits, kTrackBytes);
}

// Эталоны дорожки: TR-DOS с удалённым сектором, плохими CRC и
// заголовком без данных; пустая; переполненная (6 x 1024 при GAP3 1 длиннее
// оборота - хвост отброшен).
void test_mfm_golden() {
    std::printf("test_bdi_mfm_golden\n");
    static uint8_t data[16][1024];
    for (int i = 0; i < 16; i++) {
        for (int k = 0; k < 1024; k++) {
            data[i][k] = pattern(5, 0, i + 1, k);
        }
    }
    SectorSpec s[16];
    for (int i = 0; i < 16; i++) {
        s[i] = SectorSpec{5, 0, kTrdosOrder[i], 1, i == 2, i == 11 ? nullptr : data[i], 0};
    }
    s[4].bad_crc = kBadIdCrc;
    s[6].bad_crc = kBadDataCrc;
    CHECK_EQ(mfm_hash(s, 16, 54), 0x52894745u);
    CHECK_EQ(mfm_hash(s, 0, 0), 0x6318C89Fu);
    for (int i = 0; i < 6; i++) {
        s[i] = SectorSpec{5, 0, static_cast<uint8_t>(i + 1), 3, false, data[i], 0};
    }
    CHECK_EQ(mfm_hash(s, 6, 1), 0x9CDD6494u);
    // Пять секторов целы, у шестого данные за концом оборота - CRC не сходится.
    static uint8_t bits[kTrackBytes];
    static FoundSector f[8];
    static uint8_t out[6 * 1024];
    build_mfm_track(bits, s, 6, 1);
    CHECK_EQ(decode_mfm_track(bits, kSlotsPerRev, f, 8, out, sizeof out), 6);
    for (int i = 0; i < 6; i++) {
        CHECK(f[i].id_crc_ok);
        CHECK_EQ(f[i].data_crc_ok, i < 5);
    }
}

// Сектора дорожки по потоку ячеек: R -> данные. Возвращает число заголовков.
int decode(const uint8_t* bits, FoundSector* f, int cap, uint8_t* data, size_t data_cap) {
    return decode_mfm_track(bits, kSlotsPerRev, f, cap, data, data_cap);
}

const FoundSector* find_r(const FoundSector* f, int n, int r) {
    for (int i = 0; i < n; i++) {
        if (f[i].r == r) return &f[i];
    }
    return nullptr;
}

struct ImageArena {
    std::vector<uint8_t> mem = std::vector<uint8_t>(4u << 20);
    Arena arena;
    ImageArena() { arena = Arena{mem.data(), mem.size(), 0}; }
};

void test_image_trd() {
    std::printf("test_bdi_image_trd\n");
    std::vector<uint8_t> trd(655360);
    for (size_t i = 0; i < trd.size(); i++) {
        trd[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    }
    ImageArena a;
    DiskImage img;
    CHECK(detect_format(trd.data(), trd.size(), "x.trd") == ImageFormat::Trd);
    CHECK(img.open(ImageFormat::Trd, trd.data(), trd.size(), a.arena));
    CHECK_EQ(img.cylinders(), 80);
    static uint8_t bits[kTrackBytes];
    static FoundSector f[32];
    static uint8_t data[16 * 256];
    img.track_cells(37, 1, bits);
    const int n = decode(bits, f, 32, data, sizeof data);
    CHECK_EQ(n, 16);
    CHECK_EQ(f[0].r, 1);
    CHECK_EQ(f[1].r, 9); // порядок TR-DOS
    bool same = true;
    for (int r = 1; r <= 16; r++) {
        const FoundSector* s = find_r(f, n, r);
        same                 = same && s != nullptr && s->data_crc_ok && s->c == 37 && s->h == 1 &&
               std::memcmp(data + s->data_off, trd.data() + (37 * 2 + 1) * 4096 + (r - 1) * 256, 256) == 0;
    }
    CHECK(same);
    // Запись контроллером по разметке - в буфер файла.
    static uint8_t sec[16][256];
    SectorSpec spec[16];
    for (int i = 0; i < 16; i++) {
        const int r = f[i].r;
        std::memcpy(sec[i], data + f[i].data_off, 256);
        if (r == 4) std::memset(sec[i], 0xA5, 256);
        spec[i] = SectorSpec{37, 1, static_cast<uint8_t>(r), 1, false, sec[i], 0};
    }
    build_mfm_track(bits, spec, 16, 54);
    CHECK(img.store_cells(37, 1, bits, a.arena));
    CHECK(img.dirty());
    CHECK(img.can_save());
    CHECK(trd[(37 * 2 + 1) * 4096 + 3 * 256] == 0xA5 && trd[(37 * 2 + 1) * 4096 + 4 * 256 - 1] == 0xA5);
    // Не по разметке (15 секторов) - дорожка потоком, в TRD не сохранить.
    build_mfm_track(bits, spec, 15, 54);
    CHECK(img.store_cells(37, 1, bits, a.arena));
    CHECK(!img.can_save());
    static uint8_t back[kTrackBytes];
    img.track_cells(37, 1, back);
    CHECK(std::memcmp(back, bits, kTrackBytes) == 0);
}

void test_image_scl() {
    std::printf("test_bdi_image_scl\n");
    // Два файла: 3 и 17 секторов.
    std::vector<uint8_t> scl(9 + 2 * 14 + 20 * 256);
    std::memcpy(scl.data(), "SINCLAIR", 8);
    scl[8]          = 2;
    const char* n[] = {"FIRST   ", "SECOND  "};
    const int ns[]  = {3, 17};
    for (int i = 0; i < 2; i++) {
        uint8_t* h = scl.data() + 9 + 14 * i;
        std::memcpy(h, n[i], 8);
        h[8]  = 'C';
        h[13] = static_cast<uint8_t>(ns[i]);
    }
    for (size_t i = 0; i < 20 * 256; i++) {
        scl[9 + 28 + i] = static_cast<uint8_t>(i / 256 + 1);
    }
    ImageArena a;
    DiskImage img;
    CHECK(detect_format(scl.data(), scl.size(), "x.scl") == ImageFormat::Scl);
    CHECK(img.open(ImageFormat::Scl, scl.data(), scl.size(), a.arena));
    static uint8_t bits[kTrackBytes];
    static FoundSector f[32];
    static uint8_t data[16 * 256];
    img.track_cells(0, 0, bits);
    int cnt               = decode(bits, f, 32, data, sizeof data);
    const FoundSector* s1 = find_r(f, cnt, 1);
    const FoundSector* s9 = find_r(f, cnt, 9);
    CHECK(s1 != nullptr && s9 != nullptr);
    if (s1 != nullptr && s9 != nullptr) {
        const uint8_t* cat  = data + s1->data_off;
        const uint8_t* info = data + s9->data_off;
        CHECK(std::memcmp(cat, "FIRST   ", 8) == 0 && cat[14] == 0 && cat[15] == 1);
        CHECK(std::memcmp(cat + 16, "SECOND  ", 8) == 0 && cat[16 + 14] == 3 && cat[16 + 15] == 1);
        CHECK_EQ(info[0xE4], 2);
        CHECK_EQ(info[0xE3], 0x16);
        CHECK_EQ(info[0xE1], 4); // первый свободный: сектор 4 дорожки 2
        CHECK_EQ(info[0xE2], 2);
        CHECK_EQ(info[0xE5] | (info[0xE6] << 8), 2544 - 20);
    }
    // Файл SECOND с сектора 3 дорожки 1 (цилиндр 0, сторона 1).
    img.track_cells(0, 1, bits);
    cnt                  = decode(bits, f, 32, data, sizeof data);
    const FoundSector* s = find_r(f, cnt, 4);
    CHECK(s != nullptr && data[s->data_off] == 4);
    CHECK(!img.can_save());
}

void put16(std::vector<uint8_t>& v, size_t at, unsigned x) {
    v[at]     = static_cast<uint8_t>(x);
    v[at + 1] = static_cast<uint8_t>(x >> 8);
}

void test_image_fdi() {
    std::printf("test_bdi_image_fdi\n");
    // 2 цилиндра x 2 стороны, на дорожке 5 секторов по 1024: R 1..5, сектор 2
    // с ошибкой CRC, 3 - метка F8, 4 - без данных.
    const int cyls = 2, heads = 2, ns = 5;
    const size_t thdr = 7 + 7 * ns;
    const size_t hdr  = 14 + cyls * heads * thdr;
    const size_t tlen = 4 * 1024; // сектор 4 без данных
    std::vector<uint8_t> fdi(hdr + cyls * heads * tlen);
    std::memcpy(fdi.data(), "FDI", 3);
    put16(fdi, 4, cyls);
    put16(fdi, 6, heads);
    put16(fdi, 8, static_cast<unsigned>(hdr));
    put16(fdi, 10, static_cast<unsigned>(hdr));
    put16(fdi, 12, 0);
    size_t p = 14, trk = 0;
    for (int c = 0; c < cyls; c++) {
        for (int h = 0; h < heads; h++) {
            fdi[p]      = static_cast<uint8_t>(trk);
            fdi[p + 1]  = static_cast<uint8_t>(trk >> 8);
            fdi[p + 2]  = static_cast<uint8_t>(trk >> 16);
            fdi[p + 6]  = ns;
            p          += 7;
            size_t off  = 0;
            for (int r = 1; r <= ns; r++) {
                fdi[p]        = static_cast<uint8_t>(c);
                fdi[p + 1]    = static_cast<uint8_t>(h);
                fdi[p + 2]    = static_cast<uint8_t>(r);
                fdi[p + 3]    = 3;
                uint8_t flags = r == 2 ? 0 : 0x08;
                if (r == 3) flags |= 0x80;
                if (r == 4) flags = 0x40;
                fdi[p + 4] = flags;
                put16(fdi, p + 5, static_cast<unsigned>(off));
                if (r != 4) {
                    for (size_t k = 0; k < 1024; k++) {
                        fdi[hdr + trk + off + k] = static_cast<uint8_t>(c * 50 + h * 20 + r + k);
                    }
                    off += 1024;
                }
                p += 7;
            }
            trk += off;
        }
    }
    ImageArena a;
    DiskImage img;
    CHECK(detect_format(fdi.data(), fdi.size(), "x.fdi") == ImageFormat::Fdi);
    CHECK(img.open(ImageFormat::Fdi, fdi.data(), fdi.size(), a.arena));
    CHECK_EQ(img.cylinders(), 2);
    static uint8_t bits[kTrackBytes];
    static FoundSector f[32];
    static uint8_t data[8 * 1024];
    img.track_cells(1, 1, bits);
    const int n = decode(bits, f, 32, data, sizeof data);
    CHECK_EQ(n, 5);
    for (int r = 1; r <= 5; r++) {
        const FoundSector* s = find_r(f, n, r);
        CHECK(s != nullptr);
        if (s == nullptr) continue;
        CHECK_EQ(s->n, 3);
        CHECK_EQ(s->has_data, r != 4);
        if (r != 4) {
            CHECK_EQ(s->data_crc_ok, r != 2);
            CHECK_EQ(s->deleted, r == 3);
            CHECK_EQ(data[s->data_off + 7], static_cast<uint8_t>(50 + 20 + r + 7));
        }
    }
}

// Дорожка MFM байтами с картой меток, как в UDI: GAP, синхро, метки A1/C2.
size_t udi_track(uint8_t* bytes, uint8_t* marks, int cyl, int side) {
    size_t n       = 0;
    const auto put = [&](uint8_t b, bool mark) {
        if (mark) marks[n >> 3] = static_cast<uint8_t>(marks[n >> 3] | (1u << (n & 7)));
        bytes[n++] = b;
    };
    const auto run = [&](uint8_t b, int k) {
        for (int i = 0; i < k; i++)
            put(b, false);
    };
    run(0x4E, 80);
    run(0x00, 12);
    for (int i = 0; i < 3; i++)
        put(0xC2, true);
    put(0xFC, false);
    run(0x4E, 50);
    for (int r = 1; r <= 16; r++) {
        run(0x00, 12);
        for (int i = 0; i < 3; i++)
            put(0xA1, true);
        uint8_t id[8] = {0xA1, 0xA1, 0xA1, 0xFE, static_cast<uint8_t>(cyl), static_cast<uint8_t>(side), static_cast<uint8_t>(r), 1};
        for (int i = 3; i < 8; i++)
            put(id[i], false);
        uint16_t crc = crc16(0xFFFF, id, 8);
        put(static_cast<uint8_t>(crc >> 8), false);
        put(static_cast<uint8_t>(crc), false);
        run(0x4E, 22);
        run(0x00, 12);
        for (int i = 0; i < 3; i++)
            put(0xA1, true);
        uint8_t d[4 + 256] = {0xA1, 0xA1, 0xA1, 0xFB};
        for (int k = 0; k < 256; k++)
            d[4 + k] = pattern(cyl, side, r, k);
        for (int i = 3; i < 4 + 256; i++)
            put(d[i], false);
        crc = crc16(0xFFFF, d, sizeof d);
        put(static_cast<uint8_t>(crc >> 8), false);
        put(static_cast<uint8_t>(crc), false);
        run(0x4E, 54);
    }
    while (n < 6250)
        put(0x4E, false);
    return n;
}

void test_image_udi() {
    std::printf("test_bdi_image_udi\n");
    const int cyls = 2, heads = 2;
    std::vector<uint8_t> udi(16 + cyls * heads * (3 + 6250 + 782) + 4);
    std::memcpy(udi.data(), "UDI!", 4);
    udi[9]   = cyls - 1;
    udi[10]  = heads - 1;
    size_t p = 16;
    for (int c = 0; c < cyls; c++) {
        for (int h = 0; h < heads; h++) {
            udi[p]         = 0;
            uint8_t* bytes = udi.data() + p + 3;
            const size_t n = udi_track(bytes, bytes + 6250, c, h);
            put16(udi, p + 1, static_cast<unsigned>(n));
            p += 3 + n + (n + 7) / 8;
        }
    }
    ImageArena a;
    DiskImage img;
    CHECK(detect_format(udi.data(), udi.size(), "x.udi") == ImageFormat::Udi);
    CHECK(img.open(ImageFormat::Udi, udi.data(), udi.size(), a.arena));
    static uint8_t bits[kTrackBytes];
    static FoundSector f[32];
    static uint8_t data[16 * 256];
    img.track_cells(1, 0, bits);
    const int n = decode(bits, f, 32, data, sizeof data);
    CHECK_EQ(n, 16);
    bool same = true;
    for (int r = 1; r <= 16; r++) {
        const FoundSector* s = find_r(f, n, r);
        same                 = same && s != nullptr && s->id_crc_ok && s->data_crc_ok && data[s->data_off + 9] == pattern(1, 0, r, 9);
    }
    CHECK(same);
}

void test_image_td0() {
    std::printf("test_bdi_image_td0\n");
    // Без сжатия: одна дорожка, три сектора по 256 байт тремя способами.
    std::vector<uint8_t> td(12, 0);
    td[0]               = 0x54; // "TD"
    td[1]               = 0x44;
    td[9]               = 1;
    const uint8_t trk[] = {3, 5, 0, 0}; // секторов 3, цилиндр 5, головка 0
    td.insert(td.end(), trk, trk + 4);
    // R1: способ 0, данные как есть.
    const uint8_t h1[] = {5, 0, 1, 1, 0x00, 0, 0x01, 0x01, 0};
    td.insert(td.end(), h1, h1 + 9);
    for (int k = 0; k < 256; k++) {
        td.push_back(static_cast<uint8_t>(k));
    }
    // R2: метка F8, способ 1 - 128 раз AA 55.
    const uint8_t h2[] = {5, 0, 2, 1, 0x04, 0, 5, 0, 1, 128, 0, 0xAA, 0x55};
    td.insert(td.end(), h2, h2 + 13);
    // R3: ошибка CRC, способ 2 - 16 литералов, затем 120 раз 12 34.
    const uint8_t h3[] = {5, 0, 3, 1, 0x02, 0, 23, 0, 2, 0, 16};
    td.insert(td.end(), h3, h3 + 11);
    for (int k = 0; k < 16; k++) {
        td.push_back(static_cast<uint8_t>(k));
    }
    const uint8_t h3b[] = {1, 120, 0x12, 0x34, 0xFF};
    td.insert(td.end(), h3b, h3b + 5);
    ImageArena a;
    DiskImage img;
    CHECK(detect_format(td.data(), td.size(), "x.td0") == ImageFormat::Td0);
    CHECK(img.open(ImageFormat::Td0, td.data(), td.size(), a.arena));
    CHECK_EQ(img.cylinders(), 6);
    static uint8_t bits[kTrackBytes];
    static FoundSector f[32];
    static uint8_t data[4 * 256];
    img.track_cells(5, 0, bits);
    const int n = decode(bits, f, 32, data, sizeof data);
    CHECK_EQ(n, 3);
    const FoundSector* s1 = find_r(f, n, 1);
    const FoundSector* s2 = find_r(f, n, 2);
    const FoundSector* s3 = find_r(f, n, 3);
    CHECK(s1 != nullptr && s2 != nullptr && s3 != nullptr);
    if (s1 != nullptr && s2 != nullptr && s3 != nullptr) {
        CHECK(data[s1->data_off + 200] == 200 && s1->data_crc_ok && !s1->deleted);
        CHECK(data[s2->data_off + 10] == 0xAA && data[s2->data_off + 11] == 0x55 && s2->deleted);
        CHECK(data[s3->data_off + 15] == 15 && data[s3->data_off + 16] == 0x12 && data[s3->data_off + 255] == 0x34);
        CHECK(!s3->data_crc_ok);
    }
    CHECK(!img.can_save());
}

void test_image_hfe() {
    std::printf("test_bdi_image_hfe\n");
    // Две дорожки TR-DOS в HFE v1: стороны чередуются блоками по 256 байт.
    static MemDisk d;
    format_trdos(d, 0, 0);
    format_trdos(d, 0, 1);
    format_trdos(d, 1, 0);
    format_trdos(d, 1, 1);
    const size_t blocks = (2 * kTrackBytes + 511) / 512;
    std::vector<uint8_t> hfe(512 * (2 + 2 * blocks), 0xFF);
    std::memcpy(hfe.data(), "HXCPICFE", 8);
    hfe[8]  = 0;
    hfe[9]  = 2;
    hfe[10] = 2;
    put16(hfe, 18, 1);
    for (int c = 0; c < 2; c++) {
        const size_t off = 2 + static_cast<size_t>(c) * blocks;
        put16(hfe, 512 + 4 * c, static_cast<unsigned>(off));
        put16(hfe, 512 + 4 * c + 2, 2 * kTrackBytes);
        for (int s = 0; s < 2; s++) {
            for (size_t i = 0; i < kTrackBytes; i++) {
                hfe[off * 512 + (i / 256) * 512 + s * 256 + i % 256] = d.bits[c][s][i];
            }
        }
    }
    ImageArena a;
    DiskImage img;
    CHECK(detect_format(hfe.data(), hfe.size(), "x.hfe") == ImageFormat::Hfe);
    CHECK(img.open(ImageFormat::Hfe, hfe.data(), hfe.size(), a.arena));
    static uint8_t bits[kTrackBytes];
    img.track_cells(1, 1, bits);
    CHECK(std::memcmp(bits, d.bits[1][1].data(), kTrackBytes) == 0);
    bits[100] ^= 0x10;
    CHECK(img.store_cells(1, 1, bits, a.arena));
    static uint8_t back[kTrackBytes];
    img.track_cells(1, 1, back);
    CHECK(std::memcmp(back, bits, kTrackBytes) == 0);
    CHECK(img.can_save());
    CHECK(img.dirty());
}

// Команда через BetaDisk до INTRQ, DRQ по #FF раз в 10 мкс, как TR-DOS.
int bdi_command(BetaDisk& b, uint8_t cmd, uint8_t* in, int cap, const uint8_t* out, int len) {
    b.write_reg(kRegCmd, cmd);
    b.advance(b.now() + 12);
    int n = 0;
    for (long i = 0; i < 400000; i++) {
        b.advance(b.now() + 10);
        const uint8_t st = b.read_system();
        if ((st & 0x40) != 0) {
            if (out != nullptr) {
                b.write_reg(kRegData, n < len ? out[n] : 0x4E);
            } else {
                const uint8_t v = b.read_reg(kRegData);
                if (in != nullptr && n < cap) in[n] = v;
            }
            n++;
        }
        if ((st & 0x80) != 0) break;
    }
    return n;
}

void bdi_wr(BetaDisk& b, unsigned reg, uint8_t v) {
    b.write_reg(reg, v);
    b.advance(b.now() + 12);
}

// Сброс машины держит /MR секундами (меню 128 до входа в TR-DOS): ядро
// перескакивает его кругами по 8 слов. Тот же сброс, прогнанный по словам
// мелкими шагами, обязан дать то же время, головку и видимое - и после
// снятия /MR те же RESTORE, поиск и чтение сектора.
void test_reset_skip_exact() {
    std::printf("test_bdi_reset_skip_exact\n");
    std::vector<uint8_t> trd(655360);
    for (size_t i = 0; i < trd.size(); i++)
        trd[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    ImageArena a;
    static DiskImage img;
    CHECK(img.open(ImageFormat::Trd, trd.data(), trd.size(), a.arena));
    for (const uint8_t ff : {0x00, 0x08}) { // HLT 0 - индекс прижат; HLT 1 - дисковод не выбран
        static uint8_t cache[2][2 * kTrackBytes];
        static BetaDisk b[2];
        static ImageSource src[2];
        for (int k = 0; k < 2; k++) {
            b[k].init(cache[k]);
            src[k] = ImageSource(&img, &a.arena);
            b[k].insert(0, &src[k]);
            b[k].write_system(ff);
        }
        const uint64_t until = b[0].now() + 2000003;
        b[0].advance(until);
        while (b[1].now() < until)
            b[1].advance(b[1].now() + 10 < until ? b[1].now() + 10 : until);
        const uint64_t long_steps = b[0].fdc().steps;
        CHECK_EQ(b[0].fdc().t, b[1].fdc().t);
        CHECK_EQ(long_steps, b[1].fdc().steps);
        CHECK_EQ(b[0].fdc().fdd->pos, b[1].fdc().fdd->pos);
        uint32_t differ = 0;
        auto same       = [&]() {
            for (unsigned r = 0; r < 4; r++)
                differ += b[0].fdc().peek(r) != b[1].fdc().peek(r);
            differ += b[0].read_system() != b[1].read_system();
        };
        same();
        static uint8_t buf[2][256];
        for (BetaDisk& d : b)
            d.write_system(0x3C);
        for (int i = 0; i < 3000; i++) {
            for (BetaDisk& d : b)
                d.advance(d.now() + 10);
            same();
        }
        for (int k = 0; k < 2; k++) {
            bdi_command(b[k], 0x08, nullptr, 0, nullptr, 0);
            bdi_wr(b[k], kRegData, 5);
            bdi_command(b[k], 0x18, nullptr, 0, nullptr, 0);
            bdi_wr(b[k], kRegSec, 9);
            CHECK_EQ(bdi_command(b[k], 0x80, buf[k], 256, nullptr, 0), 256);
        }
        same();
        CHECK_EQ(differ, 0u);
        CHECK_EQ(b[0].fdc().t, b[1].fdc().t);
        CHECK(std::memcmp(buf[0], buf[1], 256) == 0 && std::memcmp(buf[0], trd.data() + 5 * 2 * 4096 + 8 * 256, 256) == 0);
        std::printf("  #FF %02X: reset 2 s, words %llu\n", ff, static_cast<unsigned long long>(long_steps));
    }
}

void test_beta_disk() {
    std::printf("test_bdi_beta_disk\n");
    std::vector<uint8_t> trd_a(655360), trd_b(655360);
    for (size_t i = 0; i < trd_a.size(); i++) {
        trd_a[i] = static_cast<uint8_t>(i * 3 + (i >> 9));
        trd_b[i] = static_cast<uint8_t>(i * 5 + 1 + (i >> 10));
    }
    ImageArena a;
    static DiskImage img_a, img_b;
    CHECK(img_a.open(ImageFormat::Trd, trd_a.data(), trd_a.size(), a.arena));
    CHECK(img_b.open(ImageFormat::Trd, trd_b.data(), trd_b.size(), a.arena));
    static uint8_t cache[2 * kTrackBytes];
    static BetaDisk b;
    b.init(cache);
    static ImageSource src_a, src_b;
    src_a = ImageSource(&img_a, &a.arena);
    src_b = ImageSource(&img_b, &a.arena);
    b.insert(0, &src_a);
    b.insert(1, &src_b);
    // После сброса машины #FF = 0: контроллер в сбросе.
    CHECK(b.fdc().mr);
    b.write_system(0x3C); // дисковод A, /MR снят, HLT, сторона 0
    // Конец /MR: микросхема сама выполняет RESTORE 03h без загрузки
    // головки. HLD = 0 - дисковод не выбран, TR00 не виден, 255 шагов по
    // 30 мс. Команда, записанная до конца, теряется - сначала Force
    // Interrupt.
    b.advance(b.now() + 100);
    CHECK((b.fdc().status() & 1) != 0);
    // Прерывание срабатывает на слове EI микрокода - в цикле шага не сразу.
    bdi_wr(b, kRegCmd, 0xD0);
    for (long i = 0; i < 10000 && (b.fdc().status() & 1) != 0; i++) {
        b.advance(b.now() + 10);
    }
    CHECK((b.fdc().status() & 1) == 0);
    b.read_reg(kRegCmd);
    bdi_command(b, 0x08, nullptr, 0, nullptr, 0); // RESTORE с загрузкой головки
    CHECK_EQ(b.fdc().peek(kRegTrk), 0);
    CHECK(b.fdc().hld);
    static uint8_t buf[256];
    // A: цилиндр 7, сторона 1, сектор 12.
    bdi_wr(b, kRegData, 7);
    bdi_command(b, 0x18, nullptr, 0, nullptr, 0);
    CHECK_EQ(b.fdc().peek(kRegTrk), 7);
    b.write_system(0x2C); // сторона 1
    bdi_wr(b, kRegSec, 12);
    CHECK_EQ(bdi_command(b, 0x80, buf, 256, nullptr, 0), 256);
    CHECK_EQ(b.fdc().status() & 0x1C, 0);
    CHECK(std::memcmp(buf, trd_a.data() + (7 * 2 + 1) * 4096 + 11 * 256, 256) == 0);
    // B: свой образ; цилиндр головки B ещё 0, TR-DOS переписывает TRK.
    b.write_system(0x3D);
    bdi_wr(b, kRegTrk, 0);
    bdi_command(b, 0x08, nullptr, 0, nullptr, 0);
    bdi_wr(b, kRegData, 2);
    bdi_command(b, 0x18, nullptr, 0, nullptr, 0);
    bdi_wr(b, kRegSec, 3);
    CHECK_EQ(bdi_command(b, 0x80, buf, 256, nullptr, 0), 256);
    CHECK(std::memcmp(buf, trd_b.data() + (2 * 2 + 0) * 4096 + 2 * 256, 256) == 0);
    // Запись на B, сброс кэша - в TRD.
    static uint8_t out[256];
    for (int k = 0; k < 256; k++) {
        out[k] = static_cast<uint8_t>(0xC0 ^ k);
    }
    bdi_wr(b, kRegSec, 5);
    CHECK_EQ(bdi_command(b, 0xA0, nullptr, 0, out, 256), 256);
    CHECK_EQ(b.fdc().status() & 0x5C, 0);
    CHECK(b.flush());
    CHECK(std::memcmp(trd_b.data() + (2 * 2 + 0) * 4096 + 4 * 256, out, 256) == 0);
    CHECK(img_b.dirty() && img_b.can_save());
    // Снова A: головка A осталась на 7-м цилиндре.
    b.write_system(0x3C);
    bdi_wr(b, kRegTrk, 7);
    bdi_wr(b, kRegSec, 1);
    CHECK_EQ(bdi_command(b, 0x80, buf, 256, nullptr, 0), 256);
    CHECK(std::memcmp(buf, trd_a.data() + (7 * 2 + 0) * 4096, 256) == 0);
    // Без диска сектор не находится.
    b.eject(0);
    bdi_wr(b, kRegSec, 1);
    b.write_reg(kRegCmd, 0x80);
    b.advance(b.now() + 3000000);
    CHECK((b.fdc().status() & 1) != 0);
    bdi_wr(b, kRegCmd, 0xD0); // Force Interrupt
    b.advance(b.now() + 100);
    CHECK((b.fdc().status() & 1) == 0);
}

// Шина глазами Z80 при BdiHost: чтения - из ячеек, поток приходит не чаще
// раза в lag полупериодов (так на плате отстаёт поток от шины), чтения его
// не будят. Цикл TR-DOS: #FF раз в 12 мкс, DRQ - чтение или запись DATA.
struct HostBus {
    BdiHost& h;
    uint64_t now  = 0;
    uint64_t lag  = 0;
    uint64_t next = 0;
    bool status   = false; // команды типа II ждут DRQ опросом статуса #1F, а не #FF
    void tick(uint64_t halves) {
        now += halves;
        if (now < next) return;
        h.service(now);
        next = now + lag;
    }
    uint8_t in(uint8_t port) {
        if (lag == 0) h.service(now);
        const uint8_t v = *h.cell(port);
        h.bus_read_done(port, static_cast<uint32_t>(now));
        tick(24);
        return v;
    }
    void out(uint8_t port, uint8_t v) {
        h.bus_write(port, v, static_cast<uint32_t>(now));
        tick(24);
    }
    int command(uint8_t cmd, uint8_t* buf, const uint8_t* src, int len) {
        out(0x1F, cmd);
        const bool by_status = status && (cmd & 0x80) != 0;
        int n                = 0;
        bool busy            = false;
        for (long i = 0; i < 2000000; i++) {
            const uint8_t st = in(by_status ? 0x1F : 0xFF);
            if ((st & (by_status ? 0x02 : 0x40)) != 0) {
                if (src != nullptr) {
                    out(0x7F, n < len ? src[n] : 0);
                } else {
                    const uint8_t v = in(0x7F);
                    if (buf != nullptr && n < len) buf[n] = v;
                }
                n++;
            }
            // Конец: INTRQ в #FF или снятый BUSY статуса, увиденный поднятым.
            if (by_status ? busy && (st & 1) == 0 : (st & 0x80) != 0) break;
            busy = busy || (st & 1) != 0;
        }
        (void)in(0x1F);
        return n;
    }
};

// Чтение и запись сектора циклом TR-DOS; start - время шины при включении,
// status - DRQ опросом статуса.
bool bdi_host_run(uint64_t start, uint64_t lag, bool status = false) {
    std::vector<uint8_t> trd(655360);
    for (size_t i = 0; i < trd.size(); i++) {
        trd[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    }
    ImageArena a;
    static DiskImage img;
    CHECK(img.open(ImageFormat::Trd, trd.data(), trd.size(), a.arena));
    static BdiHost h;
    h.init(start);
    static ImageTracks src;
    src = ImageTracks(&img, &a.arena);
    h.insert(0, &src, static_cast<uint32_t>(start));
    HostBus z{h, start, lag, 0, status};
    z.out(0xFF, 0x3C);
    z.tick(200);
    z.out(0x1F, 0xD0);
    z.tick(20000);
    (void)z.in(0x1F);
    z.command(0x08, nullptr, nullptr, 0);
    z.out(0x7F, 5);
    z.command(0x18, nullptr, nullptr, 0);
    bool ok = z.in(0x3F) == 5;
    static uint8_t buf[256], out[256];
    z.out(0x5F, 9);
    ok = ok && z.command(0x80, buf, nullptr, 256) == 256 && (*h.cell(0x1F) & 0x1C) == 0;
    ok = ok && std::memcmp(buf, trd.data() + (5 * 2) * 4096 + 8 * 256, 256) == 0;
    for (int k = 0; k < 256; k++) {
        out[k] = static_cast<uint8_t>(0x5A ^ k);
    }
    z.out(0x5F, 3);
    ok = ok && z.command(0xA0, nullptr, out, 256) == 256 && (*h.cell(0x1F) & 0x5C) == 0;
    ok = ok && !src.failed() && std::memcmp(trd.data() + (5 * 2) * 4096 + 2 * 256, out, 256) == 0;
    // Записанное читается обратно.
    ok = ok && z.command(0x80, buf, nullptr, 256) == 256 && std::memcmp(buf, out, 256) == 0;
    return ok && h.stats().lost == 0 && h.vg93().stats().lost == 0;
}

// Защёлка шины ВГ93 одна: DATA и команда в одном микрослове - регистр
// данных берёт байт команды, Seek уходит на дорожку 0x18. Метки равны: часы
// диска сдвинуты растяжкой, и границу слова по шине не угадать.
bool bdi_host_dal() {
    std::vector<uint8_t> trd(655360);
    ImageArena a;
    static DiskImage img;
    CHECK(img.open(ImageFormat::Trd, trd.data(), trd.size(), a.arena));
    static BdiHost h;
    h.init(1000);
    static ImageTracks src;
    src = ImageTracks(&img, &a.arena);
    h.insert(0, &src, 1000);
    HostBus z{h, 1000, 0, 0, false};
    z.out(0xFF, 0x3C);
    z.tick(200);
    z.command(0x08, nullptr, nullptr, 0);
    z.tick(1000);
    const uint64_t t = z.now + 16;
    h.bus_write(0x7F, 5, static_cast<uint32_t>(t));
    h.bus_write(0x1F, 0x18, static_cast<uint32_t>(t));
    z.now = t + 8;
    for (long i = 0; i < 2000000 && (z.in(0xFF) & 0x80) == 0; i++) {
    }
    return z.in(0x3F) == 0x18;
}

// Посреди чтения сектора машина 10 мс стоит в навигаторе: диск стоит с ней,
// сектор дочитывается без LOST.
bool g_bdi_paused = false;
bool bdi_host_pause() {
    std::vector<uint8_t> trd(655360);
    for (size_t i = 0; i < trd.size(); i++) {
        trd[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    }
    ImageArena a;
    static DiskImage img;
    CHECK(img.open(ImageFormat::Trd, trd.data(), trd.size(), a.arena));
    static BdiHost h;
    h.init(1000);
    h.use_pause(+[] { return g_bdi_paused; });
    static ImageTracks src;
    src = ImageTracks(&img, &a.arena);
    h.insert(0, &src, 1000);
    HostBus z{h, 1000, 0, 0, false};
    z.out(0xFF, 0x3C);
    z.tick(200);
    z.out(0x1F, 0xD0);
    z.tick(20000);
    (void)z.in(0x1F);
    z.command(0x08, nullptr, nullptr, 0);
    z.out(0x5F, 4);
    z.out(0x1F, 0x80);
    static uint8_t buf[256];
    int n = 0;
    for (long i = 0; i < 2000000; i++) {
        const uint8_t st = z.in(0xFF);
        if ((st & 0x40) != 0) {
            const uint8_t v = z.in(0x7F);
            if (n < 256) buf[n] = v;
            if (++n == 100) {
                g_bdi_paused = true;
                z.tick(20000);
                g_bdi_paused = false;
            }
        }
        if ((st & 0x80) != 0) break;
    }
    const uint8_t st = z.in(0x1F);
    return n == 256 && (st & 0x1C) == 0 && std::memcmp(buf, trd.data() + 3 * 256, 256) == 0 && h.stats().pauses == 1;
}

void test_bdi_host() {
    std::printf("test_bdi_host\n");
    CHECK(bdi_host_pause());
    CHECK(bdi_host_run(1000, 0));
    CHECK(bdi_host_dal());
    // Метка шины переходит через 2^32.
    CHECK(bdi_host_run(0xFFFFFFFFull - 5000, 0));
    // Поток отстаёт: на 10 мкс, дольше байта (32 мкс) и до 200 мкс. Часы
    // диска ждут его, потери данных нет.
    for (const uint64_t lag : {20, 30, 40, 60, 80, 200, 400}) {
        CHECK(bdi_host_run(1000, lag));
    }
    // То же, когда DRQ ждут опросом статуса.
    for (const uint64_t lag : {0, 40, 200, 400}) {
        CHECK(bdi_host_run(1000, lag, true));
    }

    // Обращение с меткой после now: обработчик шины вытеснил поток, когда
    // тот уже взял время. Поток откладывает его до следующего захода, а не
    // считает срок около 2^64 (на плате - Core1 в потоке навсегда).
    static BdiHost h;
    h.init(1000);
    h.bus_write(0xFF, 0x3C, 9000);
    CHECK(h.service(5000) != 0);
    CHECK_EQ(h.stats().events, 0u);
    h.service(9500);
    CHECK_EQ(h.stats().events, 1u);
}

// Кольцо 64: чтению с последствием не отдаются последние 16 мест, записи -
// отдаются; бит в ячейке снимается и у потерянного чтения.
void test_bdi_host_ring() {
    std::printf("test_bdi_host_ring\n");
    static BdiHost h;
    h.init(1000);
    h.bus_write(0xFF, 0x3C, 1000);
    h.bus_write(0x1F, 0xD8, 1100);
    h.service(2000);
    CHECK((*h.cell(0xFF) & kSysIntrqBit) != 0);
    for (int i = 0; i < 48; i++) {
        h.bus_write(0x3F, static_cast<uint8_t>(i), 2000);
    }
    h.bus_read_done(0x1F, 2000);
    CHECK_EQ(h.stats().lost, 1u);
    CHECK((*h.cell(0xFF) & kSysIntrqBit) == 0);
    for (int i = 0; i < 16; i++) {
        h.bus_write(0x3F, static_cast<uint8_t>(i), 2000);
    }
    CHECK_EQ(h.stats().lost, 1u);
    h.bus_write(0x3F, 0x77, 2000);
    CHECK_EQ(h.stats().lost, 2u);
    const uint32_t ev = h.stats().events;
    h.service(3000);
    CHECK_EQ(h.stats().events, ev + 64u);
    CHECK_EQ(*h.cell(0x3F), 15);
}

// Носитель в памяти: файл образа раскидан по трём участкам секторов.
struct Media {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(8000 * 512);
    MediaExtent ext[3]         = {{100, 300}, {2000, 500}, {5000, 1000}};
    static bool rd(void* u, uint32_t lba, uint8_t* d) {
        std::memcpy(d, static_cast<Media*>(u)->bytes.data() + static_cast<size_t>(lba) * 512, 512);
        return true;
    }
    static bool wr(void* u, uint32_t lba, const uint8_t* s) {
        std::memcpy(static_cast<Media*>(u)->bytes.data() + static_cast<size_t>(lba) * 512, s, 512);
        return true;
    }
    size_t at(size_t off) const {
        size_t sec = off / 512;
        for (const MediaExtent& e : ext) {
            if (sec < e.sectors) return (e.lba + sec) * 512 + off % 512;
            sec -= e.sectors;
        }
        return 0;
    }
    void put(const std::vector<uint8_t>& file) {
        for (size_t i = 0; i < file.size(); i++) {
            bytes[at(i)] = file[i];
        }
    }
    uint8_t get(size_t off) const { return bytes[at(off)]; }
};

// Образ на носителе против того же образа в памяти: все цилиндры, запись
// сектора - в файл на своё место, запись каталога SCL - в память.
// Арена образа ровно kArenaBytes: open проходит; на байт меньше - отказ.
// Размер блока дисковода на плате берётся из той же константы.
void test_media_image_arena() {
    std::printf("test_bdi_media_image_arena\n");
    std::vector<uint8_t> trd(655360, 0xE5);
    static Media media;
    media = Media{};
    media.put(trd);
    for (const size_t bytes : {MediaDisk::kArenaBytes, MediaDisk::kArenaBytes - 1u}) {
        std::vector<uint8_t> mem(bytes);
        Arena arena{mem.data(), mem.size(), 0};
        MediaDisk m;
        const bool ok =
            m.open(ImageFormat::Trd, devices::hal::SectorIo{&Media::rd, &Media::wr, &media}, media.ext, 3, static_cast<uint32_t>(trd.size()), false, arena);
        CHECK_EQ(ok, bytes == MediaDisk::kArenaBytes);
    }
}

void test_media_image() {
    std::printf("test_bdi_media_image\n");
    std::vector<uint8_t> trd(655360);
    for (size_t i = 0; i < trd.size(); i++) {
        trd[i] = static_cast<uint8_t>(i * 13 + (i >> 9));
    }
    // SCL: два файла по 3 и 20 секторов.
    std::vector<uint8_t> scl = {'S', 'I', 'N', 'C', 'L', 'A', 'I', 'R', 2};
    const uint8_t h1[14]     = {'O', 'N', 'E', ' ', ' ', ' ', ' ', ' ', 'B', 0, 3, 0, 3, 3};
    const uint8_t h2[14]     = {'T', 'W', 'O', ' ', ' ', ' ', ' ', ' ', 'C', 0, 0x50, 0, 0x14, 20};
    scl.insert(scl.end(), h1, h1 + 14);
    scl.insert(scl.end(), h2, h2 + 14);
    for (int i = 0; i < 23 * 256; i++) {
        scl.push_back(static_cast<uint8_t>(i * 7 + 1));
    }
    static uint8_t cells[kTrackBytes];
    static Track ref_t;
    auto same = [&](const Track& t, DiskImage& ref, int cyl, int side) {
        ref.track_cells(cyl, side, cells);
        track_from_cells(ref_t, cells);
        return std::memcmp(t.data, ref_t.data, kTrackLen) == 0 && std::memcmp(t.marks, ref_t.marks, sizeof(t.marks)) == 0;
    };
    // Сектор r дорожки: данные на месте, CRC пересчитан - как пишет автомат.
    auto put_sector = [](Track& t, uint8_t r, uint8_t v) {
        for (int k = 0; k < t.nid; k++) {
            if (t.id[k].r != r) continue;
            const uint32_t p      = t.id[k].dam + 1u;
            const uint8_t head[4] = {0xa1, 0xa1, 0xa1, t.id[k].mark};
            uint16_t crc          = crc16(0xffff, head, 4);
            for (uint32_t i = 0; i < 256; i++) {
                t.put((p + i) % kTrackLen, v, false);
                crc = crc16(crc, &v, 1);
            }
            t.put((p + 256) % kTrackLen, static_cast<uint8_t>(crc >> 8), false);
            t.put((p + 257) % kTrackLen, static_cast<uint8_t>(crc), false);
        }
        track_index(t);
    };
    for (const ImageFormat fmt : {ImageFormat::Trd, ImageFormat::Scl}) {
        std::vector<uint8_t>& file = fmt == ImageFormat::Trd ? trd : scl;
        static Media media;
        media = Media{};
        media.put(file);
        ImageArena a;
        static DiskImage ref;
        std::vector<uint8_t> copy = file;
        CHECK(ref.open(fmt, copy.data(), copy.size(), a.arena));
        MediaDisk m;
        CHECK(m.open(fmt, devices::hal::SectorIo{&Media::rd, &Media::wr, &media}, media.ext, 3, static_cast<uint32_t>(file.size()), false, a.arena));
        int ok = 0;
        for (int cyl = 0; cyl < 80; cyl++) {
            CHECK(m.cylinder(cyl) == nullptr);
            CHECK(m.service());
            Track* t = m.cylinder(cyl);
            CHECK(t != nullptr);
            ok += t != nullptr && same(t[0], ref, cyl, 0) && same(t[1], ref, cyl, 1) && t[0].nid == 16;
        }
        CHECK_EQ(ok, 80);
        // За краем образа - неразмеченная дорожка.
        CHECK(m.cylinder(82) == nullptr);
        CHECK(m.service());
        CHECK(m.cylinder(82) != nullptr && m.cylinder(82)[0].nid == 0);
        // Сектор 4 цилиндра 1, сторона 0: SCL - данные второго файла.
        const uint32_t v = (1 * 2 + 0) * 16 + 3;
        CHECK(m.cylinder(1) == nullptr);
        CHECK(m.service());
        put_sector(m.cylinder(1)[0], 4, 0xA5);
        m.written(2, 0); // дорожек цилиндра 2 нет: не принято
        CHECK(m.idle());
        m.written(1, 0);
        CHECK(!m.idle());
        CHECK(!m.service());
        CHECK(m.idle());
        const size_t off = fmt == ImageFormat::Trd ? v * 256 : 9 + 2 * 14 + (v - 16) * 256;
        CHECK(media.get(off) == 0xA5 && media.get(off + 255) == 0xA5);
        if (fmt == ImageFormat::Scl) {
            // Каталог: сектор 1 дорожки 0 - в памяти, файл SCL не меняется.
            const std::vector<uint8_t> before = media.bytes;
            CHECK(m.cylinder(0) == nullptr);
            CHECK(m.service());
            put_sector(m.cylinder(0)[0], 1, 'X');
            m.written(0, 0);
            CHECK(!m.service());
            CHECK(media.bytes == before);
            CHECK(m.cylinder(1) == nullptr);
            CHECK(m.service());
            CHECK(m.cylinder(0) == nullptr);
            CHECK(m.service());
            const Track& t0 = m.cylinder(0)[0];
            CHECK(t0.nid == 16 && t0.at(t0.id[0].dam + 1u) == 'X');
        }
        CHECK_EQ(m.write_errors(), 0u);
    }
}

// Диск длиннее 80 цилиндров и набор файлов носителя как диск TR-DOS.
void test_media_image_set() {
    std::printf("test_bdi_media_image_set\n");
    static Media media;
    media = Media{};
    std::memset(media.bytes.data(), 0xEE, media.bytes.size()); // за концом файла на носителе - мусор
    const devices::hal::SectorIo io{&Media::rd, &Media::wr, &media};
    auto place = [&](const std::vector<uint8_t>& file, const MediaExtent* ext, int n) {
        size_t off = 0;
        for (int i = 0; i < n; i++) {
            for (size_t k = 0; k < static_cast<size_t>(ext[i].sectors) * 512 && off < file.size(); k++, off++) {
                media.bytes[static_cast<size_t>(ext[i].lba) * 512 + k] = file[off];
            }
        }
    };
    auto pattern = [](size_t n, int mul) {
        std::vector<uint8_t> f(n);
        for (size_t i = 0; i < n; i++) {
            f[i] = static_cast<uint8_t>(i * static_cast<size_t>(mul) + (i >> 8) + 1);
        }
        return f;
    };
    // 256 байт сектора v диска.
    static uint8_t buf[256];
    auto sector = [&](MediaDisk& m, uint32_t v) -> const uint8_t* {
        const int cyl = static_cast<int>(v / 32), side = static_cast<int>(v / 16 % 2);
        if (m.cylinder(cyl) == nullptr) CHECK(m.service());
        const Track& t = m.cylinder(cyl)[side];
        for (int k = 0; k < t.nid; k++) {
            if (t.id[k].r != v % 16 + 1) continue;
            for (uint32_t i = 0; i < 256; i++) {
                buf[i] = t.at(t.id[k].dam + 1u + i);
            }
            return buf;
        }
        return nullptr;
    };
    ImageArena a;

    // TRD на 1 МБ: 128 цилиндров, за ними - неразмеченные дорожки.
    const std::vector<uint8_t> big = pattern(128u * 32 * 256, 11);
    const MediaExtent big_ext[2]   = {{100, 1000}, {3000, 1048}};
    place(big, big_ext, 2);
    {
        MediaDisk m;
        CHECK(m.open(ImageFormat::Trd, io, big_ext, 2, static_cast<uint32_t>(big.size()), false, a.arena));
        CHECK_EQ(m.cylinders(), 128);
        const uint32_t last = 128u * 32 - 1;
        const uint8_t* s    = sector(m, last);
        CHECK(s != nullptr && std::memcmp(s, big.data() + last * 256, 256) == 0);
        CHECK(m.cylinder(128) == nullptr);
        CHECK(m.service());
        CHECK_EQ(m.cylinder(128)[0].nid, 0);
    }

    // Набор: обычный файл, Hobeta, файл в 255 секторов на двух участках,
    // имя в CP866.
    const std::vector<uint8_t> f1 = pattern(300, 3);
    std::vector<uint8_t> f2       = {'b', 'o', 'o', 't', ' ', ' ', ' ', ' ', 'B', 0x34, 0x12, 0x00, 0x01, 0, 1, 0, 0};
    const std::vector<uint8_t> d2 = pattern(256, 5);
    f2.insert(f2.end(), d2.begin(), d2.end());
    const std::vector<uint8_t> f3 = pattern(65280, 7);
    const MediaExtent e1[1] = {{4000, 1}}, e2[1] = {{4010, 1}}, e3[2] = {{4100, 60}, {4300, 68}};
    place(f1, e1, 1);
    place(f2, e2, 1);
    place(f3, e3, 2);
    {
        MediaDisk m;
        CHECK(m.open_set(io, a.arena));
        CHECK(m.add("music.pt3", 300, nullptr, e1, 1));
        CHECK(!m.add("boot.$B", 273, nullptr, e2, 1)); // Hobeta без заголовка
        CHECK(m.add("boot.$B", 273, f2.data(), e2, 1));
        CHECK(m.add("Very long name here.Mod", 65280, nullptr, e3, 2));
        CHECK(!m.add("big.bin", 65281, nullptr, e3, 2)); // 256 секторов
        CHECK(!m.add("empty.bin", 0, nullptr, e3, 0));
        CHECK(m.add("\x8C\xA8\xE0.x", 256, nullptr, e1, 1));
        m.seal();
        CHECK(m.write_protect());
        CHECK_EQ(m.cylinders(), 80);
        // Каталог: имя 8, тип, начало, длина, секторов, сектор, дорожка.
        const uint8_t dir[4][16] = {
            {'m', 'u', 's', 'i', 'c', ' ', ' ', ' ', 'p', 0, 0, 0x2C, 0x01, 2, 0, 1},
            {'b', 'o', 'o', 't', ' ', ' ', ' ', ' ', 'B', 0x34, 0x12, 0x00, 0x01, 1, 2, 1},
            {'V', 'e', 'r', 'y', ' ', 'l', 'o', 'n', 'M', 0, 0, 0x00, 0xFF, 255, 3, 1},
            {'_', '_', '_', ' ', ' ', ' ', ' ', ' ', 'x', 0, 0, 0x00, 0x01, 1, 2, 17},
        };
        const uint8_t* s = sector(m, 0);
        CHECK(s != nullptr && std::memcmp(s, dir, sizeof(dir)) == 0 && s[64] == 0);
        // Служебный сектор: свободное место с 17:3, 4 файла, свободно 2285.
        s = sector(m, 8);
        CHECK(s != nullptr && s[0xE1] == 3 && s[0xE2] == 17 && s[0xE3] == 0x16 && s[0xE4] == 4 && s[0xE5] == (2285 & 0xFF) && s[0xE6] == (2285 >> 8) &&
              s[0xE7] == 0x10);
        // Данные: хвост последнего сектора файла - нули, Hobeta - без заголовка.
        s = sector(m, 16);
        CHECK(s != nullptr && std::memcmp(s, f1.data(), 256) == 0);
        s = sector(m, 17);
        CHECK(s != nullptr && std::memcmp(s, f1.data() + 256, 44) == 0 && s[44] == 0 && s[255] == 0);
        s = sector(m, 18);
        CHECK(s != nullptr && std::memcmp(s, d2.data(), 256) == 0);
        s = sector(m, 19 + 200);
        CHECK(s != nullptr && std::memcmp(s, f3.data() + 200 * 256, 256) == 0);
        s = sector(m, 19 + 255);
        CHECK(s != nullptr && std::memcmp(s, f1.data(), 256) == 0);
        s = sector(m, 19 + 256); // за последним файлом
        CHECK(s != nullptr && s[0] == 0 && s[255] == 0);
    }
    // Длинные имена с одним началом: TR-DOS открывает первый совпавший по
    // имени и типу, повторы получают счётчик в хвосте. Другой тип - не
    // повтор; короткое имя - счётчик за ним.
    {
        MediaDisk m;
        CHECK(m.open_set(io, a.arena));
        for (const char* n : {"Karbofos - Banana.pt3", "Karbofos - Cucumber.pt3", "Karbofos - Tomato.pt3", "Karbofos.mod", "Bony.pt3", "Bony.pt3"}) {
            CHECK(m.add(n, 300, nullptr, e1, 1));
        }
        m.seal();
        const char* const want[6] = {"Karbofosp", "Karbofo1p", "Karbofo2p", "Karbofosm", "Bony    p", "Bony1   p"};
        const uint8_t* s          = sector(m, 0);
        bool same                 = s != nullptr;
        for (int i = 0; i < 6 && same; i++) {
            same = std::memcmp(s + i * 16, want[i], 9) == 0;
        }
        std::printf("  same 8-char names: %.9s %.9s %.9s %.9s %.9s %.9s\n", s, s + 16, s + 32, s + 48, s + 64, s + 80);
        CHECK(same);
    }
    // Пределы: 128 файлов; 4080 секторов - диск на 128 цилиндров.
    {
        MediaDisk m;
        CHECK(m.open_set(io, a.arena));
        int ok = 0;
        for (int i = 0; i < 129; i++) {
            ok += m.add("a.b", 100, nullptr, e1, 1);
        }
        CHECK_EQ(ok, 128);
    }
    {
        MediaDisk m;
        CHECK(m.open_set(io, a.arena));
        int ok = 0;
        for (int i = 0; i < 17; i++) {
            ok += m.add("a.b", 65280, nullptr, e3, 2);
        }
        CHECK_EQ(ok, 16);
        CHECK(!m.add("a.b", 1, nullptr, e1, 1));
        m.seal();
        CHECK_EQ(m.cylinders(), 128);
        const uint8_t* s = sector(m, 8);
        CHECK(s != nullptr && s[0xE4] == 16 && s[0xE5] == 0 && s[0xE6] == 0);
        s = sector(m, 4095);
        CHECK(s != nullptr && std::memcmp(s, f3.data() + 254 * 256, 256) == 0);
    }
}

// Носитель навигатора: SD с каталогами, USB по флагу.
namespace fake_nav {
bool g_usb = false;
bool present(int vol) {
    return vol == 0 || (vol == 1 && g_usb);
}
void put(nav::Item* out, int& n, nav::Kind k, const char* name, uint32_t size = 0) {
    out[n] = nav::Item{k, 0, true, {}, false, size};
    std::strncpy(out[n].name, name, nav::kNameBytes - 1);
    n++;
}
int list(int vol, const char* path, nav::Item* out, int cap) {
    (void)cap;
    int n = 0;
    if (vol == 1) {
        put(out, n, nav::Kind::Image, "U.TRD");
    } else if (std::strcmp(path, "/") == 0) {
        put(out, n, nav::Kind::Image, "zeta.trd");
        put(out, n, nav::Kind::Dir, "Games");
        put(out, n, nav::Kind::Image, "Alpha.scl");
        put(out, n, nav::Kind::Dir, "demos");
        put(out, n, nav::Kind::Dir, "\x90\xA5\xA4"); // "Ред" в CP866
    } else if (std::strcmp(path, "/Games") == 0) {
        put(out, n, nav::Kind::Image, "DIZZY.TRD");
    } else if (std::strcmp(path, "/demos") == 0) {
        put(out, n, nav::Kind::Image, "z.trd");
        put(out, n, nav::Kind::File, "boot.$B", 273);
        put(out, n, nav::Kind::File, "big.mod", 300000);
        put(out, n, nav::Kind::Dir, "many");
        put(out, n, nav::Kind::File, "a.pt3", 3000);
        put(out, n, nav::Kind::Dir, "fat");
    } else if (std::strcmp(path, "/demos/many") == 0 || std::strcmp(path, "/demos/fat") == 0) {
        // many - 130 файлов по сектору; fat - 20 по 235 секторов.
        const bool many = path[7] == 'm';
        for (int k = 0; k < (many ? 130 : 20); k++) {
            char name[8] = {'f', static_cast<char>('0' + k / 100), static_cast<char>('0' + k / 10 % 10), static_cast<char>('0' + k % 10), '.', 'c', 0};
            put(out, n, nav::Kind::File, name, many ? 100 : 60000);
        }
    } else {
        return -1;
    }
    return n;
}
} // namespace fake_nav

void test_bdi_nav() {
    std::printf("test_bdi_nav\n");
    static nav::Item items[64];
    static nav::Memory mem;
    std::memset(&mem, 0xEE, sizeof(mem)); // мусор PSRAM при включении
    nav::Navigator n;
    fake_nav::g_usb = false;
    n.init(items, 64, &mem, nav::Media{&fake_nav::present, &fake_nav::list});
    // Один доступный пункт (SD) - сразу его корень: "..", каталоги, потом
    // образы.
    n.open();
    CHECK(n.level() != nav_page::Level::Top);
    CHECK_EQ(n.count(), 6);
    CHECK(std::strcmp(n.item(0).name, "..") == 0 && n.item(0).kind == nav::Kind::Dir);
    CHECK(std::strcmp(n.item(1).name, "demos") == 0);
    CHECK(std::strcmp(n.item(2).name, "Games") == 0);
    CHECK(std::strcmp(n.item(3).name, "\x90\xA5\xA4") == 0);
    CHECK(std::strcmp(n.item(4).name, "Alpha.scl") == 0);
    nav::Place picked{};
    CHECK(n.enter(2, picked) == nav::Pick::Moved);
    CHECK(std::strcmp(n.where().path, "/Games") == 0 && n.cursor() == 0);
    // ".." - уровень выше, курсор на покинутом каталоге.
    CHECK(n.enter(0, picked) == nav::Pick::Moved);
    CHECK(std::strcmp(n.where().path, "/") == 0 && n.cursor() == 2);
    CHECK(n.enter(2, picked) == nav::Pick::Moved);
    CHECK(n.enter(1, picked) == nav::Pick::Image);
    CHECK(std::strcmp(picked.path, "/Games/DIZZY.TRD") == 0);
    n.remember(picked);
    // "Сброс": новый навигатор на той же памяти открывает последний каталог.
    nav::Navigator m;
    fake_nav::g_usb = true;
    m.init(items, 64, &mem, nav::Media{&fake_nav::present, &fake_nav::list});
    m.open();
    CHECK(std::strcmp(m.where().path, "/Games") == 0);
    m.back();
    CHECK(std::strcmp(m.where().path, "/") == 0 && m.cursor() == 2);
    m.back();
    CHECK(m.level() == nav_page::Level::Top && m.cursor() == 0);
    CHECK_EQ(m.count(), 4); // SD, USB, WIFI, RECENT
    CHECK(m.item(1).enabled && !m.item(2).enabled && m.item(3).enabled);
    CHECK(m.enter(2, picked) == nav::Pick::None); // WIFI - заготовка
    CHECK(m.enter(3, picked) == nav::Pick::Moved);
    CHECK_EQ(m.count(), 2);
    CHECK(std::strcmp(m.item(1).name, "DIZZY.TRD") == 0);
    CHECK(m.enter(1, picked) == nav::Pick::Image && picked.vol == 0 && std::strcmp(picked.path, "/Games/DIZZY.TRD") == 0);
    // Из RECENT - на верхний уровень, курсор на RECENT.
    CHECK(m.enter(0, picked) == nav::Pick::Moved);
    CHECK(m.level() == nav_page::Level::Top && m.cursor() == 3);
    // Последних не больше десяти, повтор поднимается наверх.
    for (int k = 0; k < 12; k++) {
        nav::Place p{1, "/X"};
        p.path[2] = static_cast<char>('A' + k);
        m.remember(p);
    }
    m.remember(nav::Place{1, "/XC"});
    CHECK_EQ(mem.recent_count, 10);
    CHECK(std::strcmp(mem.recent[0].path, "/XC") == 0 && std::strcmp(mem.recent[1].path, "/XL") == 0);
}

// Плата навигатора: что вставлено куда.
namespace fake_board {
char g_name[4][16];
bool g_ro[4];
std::string g_set;      // имена файлов набора через ';'
const char* g_bad = ""; // файл с этим именем не открывается
uint8_t mount_set(int drive, const nav::Navigator& nv, int& bad) {
    g_set.clear();
    int files = 0;
    for (int i = 0; i < nv.count(); i++) {
        if (!nv.item(i).marked) continue;
        if (std::strcmp(nv.item(i).name, g_bad) == 0) {
            bad = i;
            return player::nav_page::NoFile;
        }
        g_set += std::string(nv.where().path) + "/" + nv.item(i).name + ";";
        files++;
    }
    std::snprintf(g_name[drive], sizeof(g_name[drive]), "FILES: %d", files);
    return player::nav_page::Ok;
}
bool mount(int drive, const nav::Place& p, bool read_only) {
    const char* s = std::strrchr(p.path, '/');
    std::strncpy(g_name[drive], s + 1, 15);
    g_ro[drive] = read_only;
    return true;
}
bool eject(int drive) {
    g_name[drive][0] = 0;
    return true;
}
const char* drive_name(int drive) {
    return g_name[drive];
}
} // namespace fake_board

// Страница обмена глазами ПЗУ навигатора: запрос - ack - строки.
void test_bdi_nav_page() {
    std::printf("test_bdi_nav_page\n");
    namespace pg = player::nav_page;
    static nav::Item items[200];
    static nav::Memory mem;
    static uint8_t page[0x2000]; // банк N0
    std::memset(&mem, 0, sizeof(mem));
    std::memset(page, 0, sizeof(page));
    std::memset(fake_board::g_name, 0, sizeof(fake_board::g_name));
    nav::Navigator n;
    nav::Place picked{};
    fake_nav::g_usb = false;
    n.init(items, 200, &mem, nav::Media{&fake_nav::present, &fake_nav::list});
    const nav::Board board{&fake_board::mount, &fake_board::eject, &fake_board::drive_name, &fake_board::mount_set};
    auto ask = [&](uint8_t cmd, unsigned arg, uint8_t drive = 0) {
        page[pg::kCmd]      = cmd;
        page[pg::kArgLo]    = static_cast<uint8_t>(arg);
        page[pg::kArgHi]    = static_cast<uint8_t>(arg >> 8);
        page[pg::kArgDrive] = drive;
        page[pg::kReq]++;
        nav::serve(n, picked, page, 0x17, board);
        CHECK_EQ(page[pg::kAck], page[pg::kReq]);
        return page[pg::kResult];
    };
    auto row       = [&](int r) { return reinterpret_cast<const char*>(page + pg::kRows + r * pg::kRowBytes + 1); };
    auto kind      = [&](int r) { return page[pg::kRows + r * pg::kRowBytes]; };
    auto marked    = [&] { return page[pg::kMarked] | (page[pg::kMarked + 1] << 8); };
    auto marked_kb = [&] { return page[pg::kMarkedKb] | (page[pg::kMarkedKb + 1] << 8); };

    CHECK_EQ(ask(pg::Open, 0), pg::Moved);
    CHECK(std::memcmp(page, pg::kMagicBytes, 4) == 0);
    CHECK_EQ(page[pg::kLevel], 1);
    CHECK_EQ(page[pg::kCount], 6);
    CHECK_EQ(page[pg::kPort7ffd], 0x17);
    CHECK_EQ(page[pg::kBank], 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(page + pg::kTitle), "SD:/") == 0);
    CHECK_EQ(page[pg::kRows], static_cast<uint8_t>(nav::Kind::Dir));
    CHECK(std::strcmp(row(0), "..") == 0 && std::strcmp(row(1), "demos") == 0 && row(6)[-1] == 0);
    // Повтор без нового запроса не трогает страницу.
    page[pg::kRows] = 0x55;
    nav::serve(n, picked, page, 0x17, board);
    CHECK_EQ(page[pg::kRows], 0x55);

    CHECK_EQ(ask(pg::Window, 4), pg::Ok);
    CHECK(std::strcmp(row(0), "Alpha.scl") == 0 && page[pg::kFirst] == 4);
    // Номер пункта - от начала списка, не окна; смена списка - окно с начала.
    CHECK_EQ(ask(pg::Enter, 2), pg::Moved);
    CHECK(std::strcmp(reinterpret_cast<const char*>(page + pg::kTitle), "SD:/Games") == 0 && page[pg::kFirst] == 0);
    CHECK_EQ(ask(pg::Enter, 1), pg::Image);
    CHECK_EQ(ask(pg::Mount, 0, 0x81), pg::Ok);
    CHECK(fake_board::g_ro[1] && std::strcmp(fake_board::g_name[1], "DIZZY.TRD") == 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(page + pg::kDrives + 16), "DIZZY.TRD") == 0);
    CHECK(mem.recent_count == 1 && std::strcmp(mem.recent[0].path, "/Games/DIZZY.TRD") == 0);
    CHECK_EQ(ask(pg::Eject, 0, 1), pg::Ok);
    CHECK_EQ(page[pg::kDrives + 16], 0);
    // Назад: курсор на каталоге, из которого вышли.
    CHECK_EQ(ask(pg::Back, 0), pg::Moved);
    CHECK_EQ(page[pg::kCount], 6);
    CHECK_EQ(page[pg::kCursor], 2);

    // Отметки: "..", fat, many, a.pt3, big.mod, boot.$B, z.trd.
    CHECK_EQ(ask(pg::Enter, 1), pg::Moved);
    CHECK(std::strcmp(row(3), "a.pt3") == 0 && kind(3) == pg::kKindFile && kind(6) == pg::kKindImage && kind(1) < pg::kKindImage);
    CHECK_EQ(ask(pg::Mark, 0), pg::Fail);    // ".."
    CHECK_EQ(ask(pg::Mark, 6), pg::Fail);    // образ
    CHECK_EQ(ask(pg::Mark, 4), pg::TooLong); // 300000 Б
    CHECK_EQ(ask(pg::Mark, 3), pg::Ok);      // 3000 Б - 12 секторов
    CHECK(kind(3) == (pg::kKindFile | pg::kRowMarkedBit) && marked() == 1 && marked_kb() == 3);
    CHECK_EQ(ask(pg::Mark, 5), pg::Ok); // Hobeta: 273 Б - сектор
    CHECK(marked() == 2 && marked_kb() == 4);
    CHECK_EQ(ask(pg::Mark, 3), pg::Ok); // снята
    CHECK(kind(3) == pg::kKindFile && marked() == 1 && marked_kb() == 1);
    // Файл набора не открылся: отметки на месте, курсор - на нём.
    CHECK_EQ(ask(pg::Mark, 3), pg::Ok);
    fake_board::g_bad = "boot.$B";
    CHECK_EQ(ask(pg::MountSet, 3, 2), pg::NoFile);
    CHECK(marked() == 2 && page[pg::kCursor] == 5);
    fake_board::g_bad = "";
    CHECK_EQ(ask(pg::Mark, 3), pg::Ok);
    // Диск из отмеченного: номер пункта не важен, отметки сняты.
    CHECK_EQ(ask(pg::MountSet, 3, 2), pg::Ok);
    CHECK(fake_board::g_set == "/demos/boot.$B;" && std::strcmp(fake_board::g_name[2], "FILES: 1") == 0 && marked() == 0);
    // Без отметок - из файла под курсором.
    CHECK_EQ(ask(pg::MountSet, 3, 0), pg::Ok);
    CHECK(fake_board::g_set == "/demos/a.pt3;" && marked() == 0 && kind(3) == pg::kKindFile);
    CHECK_EQ(ask(pg::MountSet, 4, 0), pg::TooLong);
    CHECK_EQ(ask(pg::MountSet, 6, 0), pg::Fail);
    // Предел каталога: 128 файлов.
    CHECK_EQ(ask(pg::Enter, 2), pg::Moved);
    int ok = 0;
    for (unsigned i = 1; i <= 128; i++) {
        ok += ask(pg::Mark, i) == pg::Ok;
    }
    CHECK(ok == 128 && marked() == 128 && marked_kb() == 32);
    CHECK_EQ(ask(pg::Mark, 129), pg::TooMany);
    CHECK_EQ(ask(pg::Unmark, 0), pg::Ok);
    CHECK_EQ(marked(), 0);
    CHECK_EQ(ask(pg::Mark, 129), pg::Ok);
    // Имя длиннее 47 знаков держится целиком: по нему файл открывается.
    CHECK(nav::kNameBytes >= 96);
    // Смена списка снимает отметки. Предел диска: 4080 секторов.
    CHECK_EQ(ask(pg::Back, 0), pg::Moved);
    CHECK(marked() == 0 && page[pg::kCursor] == 2);
    CHECK_EQ(ask(pg::Enter, 1), pg::Moved);
    ok = 0;
    for (unsigned i = 1; i <= 17; i++) {
        ok += ask(pg::Mark, i) == pg::Ok;
    }
    CHECK(ok == 17 && marked_kb() == 999);
    CHECK_EQ(ask(pg::Mark, 18), pg::NoRoom);
    CHECK_EQ(marked(), 17);
}

} // namespace

void run_bdi_tests() {
    test_reset_skip_exact();
    test_beta_disk();
    test_bdi_host();
    test_bdi_host_ring();
    test_media_image();
    test_media_image_arena();
    test_media_image_set();
    test_bdi_nav();
    test_bdi_nav_page();
    test_image_trd();
    test_image_scl();
    test_image_fdi();
    test_image_udi();
    test_image_td0();
    test_image_hfe();
    test_mfm_roundtrip();
    test_mfm_all_sync();
    test_mfm_golden();
}
