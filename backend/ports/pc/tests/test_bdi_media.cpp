// SPDX-License-Identifier: MIT
// Автомат ВГ93 на образе с носителя (MediaDisk): цилиндр строит цикл не
// сразу, а раз в период опроса, как bdi_poll на плате. Чтение, запись,
// запись нескольких секторов, разметка TR-DOS доходят до файла; шаг туда и
// обратно до прохода цикла; смена диска посреди команды.

#include "testing.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "devices/bdi/disk.h"
#include "devices/bdi/drive.h"
#include "devices/bdi/vg93.h"

using namespace devices::bdi;

namespace {

constexpr uint32_t kTrdBytes = 655360;

// Носитель: образ TRD тремя отрезками файла.
struct Media {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(8000 * 512);
    MediaExtent ext[3]         = {{100, 300}, {2000, 500}, {5000, 1000}};
    unsigned writes            = 0;
    uint32_t bad_lba           = ~0u; // носитель не отдаёт этот блок
    static bool rd(void* u, uint32_t lba, uint8_t* d) {
        auto* m = static_cast<Media*>(u);
        if (lba == m->bad_lba) return false;
        std::memcpy(d, m->bytes.data() + static_cast<size_t>(lba) * 512, 512);
        return true;
    }
    static bool wr(void* u, uint32_t lba, const uint8_t* s) {
        auto* m = static_cast<Media*>(u);
        m->writes++;
        std::memcpy(m->bytes.data() + static_cast<size_t>(lba) * 512, s, 512);
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
    uint8_t get(size_t off) const { return bytes[at(off)]; }
    // Сектор r (1..16) стороны side цилиндра c в образе.
    static size_t sector(int c, int side, int r) { return static_cast<size_t>((c * 2 + side) * 16 + r - 1) * 256; }
};

uint8_t pattern(size_t i) {
    return static_cast<uint8_t>(i * 13 + (i >> 9));
}

// Дисковод A с образом и опросом цикла раз в poll полутактов.
struct Rig {
    Media media[2];
    std::vector<uint8_t> mem[2];
    MediaDisk disk[2];
    Vg93 f;
    uint64_t now = 0, poll = 2000, next = 0;
    bool polling = true; // false - цикл стоит (окно между проходами bdi_poll)

    uint32_t bytes = kTrdBytes; // длина файла образа A
    bool open(int i, uint8_t fill_xor) {
        for (size_t k = 0; k < kTrdBytes; k++)
            media[i].bytes[media[i].at(k)] = pattern(k) ^ fill_xor;
        mem[i].assign(MediaDisk::kArenaBytes, 0);
        Arena arena{mem[i].data(), mem[i].size(), 0};
        return disk[i].open(ImageFormat::Trd, devices::hal::SectorIo{&Media::rd, &Media::wr, &media[i]}, media[i].ext, 3, i == 0 ? bytes : kTrdBytes, false,
                            arena);
    }
    void service() {
        for (MediaDisk& d : disk) {
            if (d.service()) f.source_ready(0, now);
        }
    }
    void adv_to(uint64_t t) {
        for (;;) {
            const uint64_t d = f.due(), p = polling ? next : kNever, n = d < p ? d : p;
            if (n > t) break;
            if (d <= p) {
                if (d > now) now = d;
                f.step();
            } else {
                if (p > now) now = p;
                service();
                next = p + poll;
            }
        }
        now = t;
    }
    void wr(unsigned r, uint8_t v) {
        adv_to(now + 40);
        f.write(r, v, now);
    }
    // Команда; DRQ - через 10 мкс; src - байты записи (nullptr - чтение в buf).
    // after/hook - вызов после after байтов DRQ.
    template <class Hook>
    int command(uint8_t cmd, uint8_t* buf, const std::vector<uint8_t>* src, uint8_t& st, int after, Hook hook) {
        adv_to(now + 200);
        f.write(0, cmd, now);
        int n = 0;
        for (long us = 0; us < 3000000; us++) {
            adv_to(now + 2);
            if ((f.sys() & kSysDrqBit) != 0) {
                adv_to(now + 20);
                if (src != nullptr) {
                    f.write(3, n < static_cast<int>(src->size()) ? (*src)[n] : 0x4e, now);
                } else {
                    if (buf != nullptr && n < 256) buf[n] = f.reg(3);
                    f.read(3, now);
                }
                if (++n == after) hook();
            }
            if ((f.sys() & kSysIntrqBit) != 0) break;
        }
        st = f.status(now);
        f.read(0, now);
        return n;
    }
    int command(uint8_t cmd, uint8_t* buf, const std::vector<uint8_t>* src, uint8_t& st) {
        return command(cmd, buf, src, st, -1, [] {});
    }
    void settle() {
        for (int i = 0; i < 100 && !(disk[0].idle() && disk[1].idle()); i++)
            service();
    }
    bool start(uint64_t poll_us) {
        poll = poll_us * 2;
        if (!open(0, 0) || !open(1, 0xff)) return false;
        f.reset(0);
        f.insert(0, &disk[0], 0);
        wr(4, 0x3C);
        adv_to(now + 400);
        wr(0, 0xD0);
        adv_to(now + 40000);
        f.read(0, now);
        uint8_t st = 0;
        command(0x08, nullptr, nullptr, st);
        return (st & kStSeekErrBit) == 0;
    }
    void seek(uint8_t c) {
        uint8_t st = 0;
        wr(3, c);
        command(0x18, nullptr, nullptr, st);
    }
};

bool same_sector(const Media& m, size_t at, const uint8_t* want) {
    for (int k = 0; k < 256; k++) {
        if (m.get(at + k) != want[k]) return false;
    }
    return true;
}

// Чтение, запись, запись двух секторов (0xB0), разметка - при опросе раз в
// 50 мкс, 1 и 5 мс.
void test_media_async(uint64_t poll_us) {
    const std::unique_ptr<Rig> rp(new Rig);
    Rig& r = *rp;
    CHECK(r.start(poll_us));
    uint8_t st = 0;
    r.seek(5);
    static uint8_t buf[256];
    std::vector<uint8_t> want(256);
    for (int k = 0; k < 256; k++)
        want[k] = pattern(Media::sector(5, 0, 9) + k);
    r.wr(2, 9);
    int n = r.command(0x80, buf, nullptr, st);
    CHECK_EQ(n, 256);
    CHECK_EQ(st, 0);
    CHECK(std::memcmp(buf, want.data(), 256) == 0);
    CHECK(r.f.stats().wait_max / 2 < poll_us + 1000);

    std::vector<uint8_t> out(256);
    for (int k = 0; k < 256; k++)
        out[k] = static_cast<uint8_t>(0x5A ^ k);
    r.wr(2, 3);
    n = r.command(0xA0, nullptr, &out, st);
    r.settle();
    CHECK_EQ(st, 0);
    CHECK(same_sector(r.media[0], Media::sector(5, 0, 3), out.data()));

    // 15 и 16, на 17 - RNF; оба сектора в файле.
    std::vector<uint8_t> two(512);
    for (int k = 0; k < 512; k++)
        two[k] = static_cast<uint8_t>(0xC3 ^ k ^ (k >> 8));
    r.wr(2, 15);
    n = r.command(0xB0, nullptr, &two, st);
    r.settle();
    CHECK_EQ(n, 512);
    CHECK(same_sector(r.media[0], Media::sector(5, 0, 15), two.data()));
    CHECK(same_sector(r.media[0], Media::sector(5, 0, 16), two.data() + 256));

    // Разметка TR-DOS цилиндра 6, сторона 0: в файле нули.
    r.seek(6);
    std::vector<uint8_t> fmt;
    auto run = [&](uint8_t v, int k) { fmt.insert(fmt.end(), k, v); };
    run(0x4e, 80);
    run(0x00, 12);
    run(0xf6, 3);
    fmt.push_back(0xfc);
    run(0x4e, 50);
    for (int i = 0; i < 16; i++) {
        run(0x00, 12);
        run(0xf5, 3);
        fmt.insert(fmt.end(), {0xfe, 6, 0, kTrdosOrder[i], 1, 0xf7});
        run(0x4e, 22);
        run(0x00, 12);
        run(0xf5, 3);
        fmt.push_back(0xfb);
        run(0x00, 256);
        fmt.push_back(0xf7);
        run(0x4e, 54);
    }
    r.command(0xF0, nullptr, &fmt, st);
    r.settle();
    bool zeros = true;
    for (int k = 0; k < 4096; k++)
        zeros = zeros && r.media[0].get(Media::sector(6, 0, 1) + k) == 0;
    CHECK(zeros);
    CHECK_EQ(r.disk[0].write_errors(), 0u);
    std::printf("  poll %u us: wait max %u us, media writes %u\n", static_cast<unsigned>(poll_us), r.f.stats().wait_max / 2, r.media[0].writes);
}

// Шаг внутрь и обратно до прохода цикла: дорожки остаются прежнего
// цилиндра, запись на него доходит до файла.
void test_media_step_back() {
    const std::unique_ptr<Rig> rp(new Rig);
    Rig& r = *rp;
    CHECK(r.start(1000));
    r.seek(2);
    uint8_t st = 0;
    r.polling  = false;
    r.command(0x58, nullptr, nullptr, st); // шаг внутрь: заказан 3
    r.command(0x78, nullptr, nullptr, st); // шаг наружу: снова 2
    r.polling = true;
    std::vector<uint8_t> out(256, 0x77);
    r.wr(2, 4);
    r.command(0xA0, nullptr, &out, st);
    r.settle();
    std::printf("  step in and back: write st %02X, file has it %d\n", st, same_sector(r.media[0], Media::sector(2, 0, 4), out.data()) ? 1 : 0);
    CHECK_EQ(st, 0);
    CHECK(same_sector(r.media[0], Media::sector(2, 0, 4), out.data()));
    static uint8_t buf[256];
    r.wr(2, 5);
    r.command(0x80, buf, nullptr, st);
    CHECK_EQ(st, 0);
    CHECK_EQ(buf[0], pattern(Media::sector(2, 0, 5)));
}

// Смена диска на выбранном дисководе посреди команды.
void test_media_insert_mid_command() {
    // (а) Чтение ждёт цилиндр: после вставки команда кончается.
    std::unique_ptr<Rig> rp(new Rig);

    Rig& r = *rp;
    CHECK(r.start(1000));
    uint8_t st = 0;
    r.seek(3);
    r.polling = false;
    r.command(0x58, nullptr, nullptr, st); // цилиндр 4 ещё не построен
    r.wr(2, 1);
    r.adv_to(r.now + 200);
    r.f.write(0, 0x80, r.now);
    r.adv_to(r.now + 20000);
    r.f.insert(0, &r.disk[1], r.now);
    r.polling = true;
    static uint8_t buf[256];
    int n     = 0;
    bool done = false;
    for (long us = 0; us < 3000000 && !done; us++) {
        r.adv_to(r.now + 2);
        if ((r.f.sys() & kSysDrqBit) != 0) {
            if (n < 256) buf[n] = r.f.reg(3);
            r.f.read(3, r.now);
            n++;
        }
        done = (r.f.sys() & kSysIntrqBit) != 0;
    }
    st = r.f.status(r.now);
    std::printf("  insert while waiting: done %d st %02X bytes %d\n", done ? 1 : 0, st, n);
    CHECK(done);
    CHECK(n == 256 && buf[0] == static_cast<uint8_t>(pattern(Media::sector(4, 0, 1)) ^ 0xff));

    // (б) Посреди записи сектора: новый образ не тронут, статус - ошибка.
    std::unique_ptr<Rig> wp(new Rig);
    Rig& w = *wp;
    CHECK(w.start(1000));
    w.seek(1);
    std::vector<uint8_t> out(256, 0xEE);
    w.wr(2, 1);
    w.command(0xA0, nullptr, &out, st, 64, [&] { w.f.insert(0, &w.disk[1], w.now); });
    w.settle();
    // Изменённые байты дорожки 1:0 в обоих образах.
    int a = 0, b = 0;
    for (size_t k = 0; k < 4096; k++) {
        const size_t at  = Media::sector(1, 0, 1) + k;
        a               += w.media[0].get(at) != pattern(at);
        b               += w.media[1].get(at) != static_cast<uint8_t>(pattern(at) ^ 0xff);
    }
    std::printf("  insert mid write: st %02X, changed in old %d, in new %d\n", st, a, b);
    CHECK((st & kStRnfBit) != 0);
    CHECK_EQ(b, 0);
}

// Сектор, не прочитанный с носителя: машина получает его с ошибкой CRC, а
// не нули под видом данных; соседний сектор того же блока - тоже.
void test_media_read_failure() {
    const std::unique_ptr<Rig> rp(new Rig);
    Rig& r = *rp;
    CHECK(r.start(1000));
    r.media[0].bad_lba = static_cast<uint32_t>(r.media[0].at(Media::sector(3, 0, 5)) / 512);
    r.seek(3);
    static uint8_t buf[256];
    uint8_t st = 0;
    r.wr(2, 5);
    r.command(0x80, buf, nullptr, st);
    uint8_t st_ok = 0;
    r.wr(2, 9);
    r.command(0x80, buf, nullptr, st_ok);
    std::printf("  unread sector: status %02X, healthy one %02X, read errors %u\n", st, st_ok, r.disk[0].read_errors());
    CHECK((st & kStCrcBit) != 0);
    CHECK_EQ(st_ok, 0);
    CHECK(r.disk[0].read_errors() >= 1u);
}

// Короткий TRD: запись в сектор за концом файла - ошибка записи (файл не
// дописывается), внутри файла - как обычно.
void test_media_short_image() {
    const std::unique_ptr<Rig> rp(new Rig);
    Rig& r  = *rp;
    r.bytes = 10u * 2u * 16u * 256u; // 10 цилиндров
    CHECK(r.start(1000));
    std::vector<uint8_t> out(256, 0x55);
    uint8_t inside = 0, beyond = 0;
    r.seek(5);
    r.wr(2, 1);
    r.command(0xA0, nullptr, &out, inside);
    r.seek(20);
    r.wr(2, 1);
    r.command(0xA0, nullptr, &out, beyond);
    r.settle();
    std::printf("  short TRD: write inside %02X, beyond the file %02X, write errors %u\n", inside, beyond, r.disk[0].write_errors());
    CHECK_EQ(inside, 0);
    CHECK_EQ(beyond, kStWrtFaultBit);
    CHECK(same_sector(r.media[0], Media::sector(5, 0, 1), out.data()));
}

} // namespace

void run_bdi_media_tests() {
    std::printf("test_bdi_media\n");
    for (const uint64_t poll : {50u, 1000u, 5000u})
        test_media_async(poll);
    test_media_step_back();
    test_media_insert_mid_command();
    test_media_read_failure();
    test_media_short_image();
}
