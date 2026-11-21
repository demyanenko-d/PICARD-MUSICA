// SPDX-License-Identifier: MIT
// Автомат ВГ93 (Vg93) против точной модели кристалла (Vg93w через BetaDisk):
// одни сценарии на одной дорожке TR-DOS. Сравниваются подъёмы DRQ (момент
// и байт), конец команды (момент, статус, TRK, SEC). Тип I - в пределах
// тика делителя шагов (1 мс): его фаза у кристалла плывёт после чтений.

#include "testing.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "vg93_ref/beta_disk.h"
#include "vg93_ref/disk_image.h"
#include "devices/bdi/vg93.h"
#include "vg93_ref/mfm.h"

using namespace devices::bdi;

namespace {

constexpr int kCyls = 84;
Track g_disk[kCyls][2];

void format_disk() {
    static uint8_t data[kCyls][2][16][256];
    for (int c = 0; c < kCyls; c++) {
        for (int h = 0; h < 2; h++) {
            SectorSpec s[16];
            for (int i = 0; i < 16; i++) {
                const int r = kTrdosOrder[i];
                for (int k = 0; k < 256; k++) {
                    data[c][h][r - 1][k] = static_cast<uint8_t>(c * 31 + h * 7 + r * 13 + k * 5 + (k >> 4));
                }
                s[i] = SectorSpec{static_cast<uint8_t>(c), static_cast<uint8_t>(h), static_cast<uint8_t>(r), 1, false, data[c][h][r - 1], 0};
            }
            if (c == 81) s[3].bad_crc = kBadDataCrc;
            if (c == 82) s[5].bad_crc = kBadIdCrc;
            if (c == 82) s[6].deleted = true;
            track_build(g_disk[c][h], s, 16, 54);
        }
    }
}

struct Oracle {
    struct Src final : DiskSource {
        Track t[kCyls][2];
        bool cylinder(int cyl, uint8_t* const cells[2], unsigned mask) override {
            for (int s = 0; s < 2; s++) {
                if ((mask & (1u << s)) == 0) continue;
                if (cyl < kCyls) {
                    track_to_cells(t[cyl][s], cells[s]);
                } else {
                    std::memset(cells[s], 0, kTrackBytes);
                }
            }
            return true;
        }
        bool written(int cyl, int side, const uint8_t* cells) override {
            track_from_cells(t[cyl][side], cells);
            return true;
        }
        bool write_protect() const override { return false; }
    };
    Src src;
    uint8_t cache[2 * kTrackBytes];
    BetaDisk b;
    void init() {
        std::memcpy(src.t, g_disk, sizeof g_disk);
        b.init(cache);
        b.insert(0, &src);
    }
    void adv(uint64_t us) {
        if (b.now() < us) b.advance(us);
    }
    void w(unsigned r, uint8_t v, uint64_t us) {
        adv(us);
        if (r == 4) {
            b.write_system(v);
        } else {
            b.write_reg(r, v);
        }
    }
    void rd(unsigned r, uint64_t us) {
        adv(us);
        (void)b.read_reg(r);
    }
    uint8_t st() { return b.fdc().status(); }
    uint8_t reg(unsigned r) { return b.fdc().peek(r); }
    uint8_t sys() { return b.read_system(); }
};

struct Fsm {
    struct Src final : Disk {
        Track t[kCyls][2];
        Track* cylinder(int cyl) override { return cyl < kCyls ? t[cyl] : nullptr; }
        void written(int, int) override {}
        bool write_protect() const override { return false; }
    };
    Src src;
    Vg93 f;
    uint64_t now = 0;
    void init() {
        std::memcpy(src.t, g_disk, sizeof g_disk);
        f = Vg93{};
        f.reset(0);
        f.insert(0, &src, 0);
    }
    void adv(uint64_t us) {
        while (f.due() <= 2 * us) {
            f.step();
        }
        now = 2 * us;
    }
    void w(unsigned r, uint8_t v, uint64_t us) {
        adv(us);
        f.write(r, v, 2 * us);
    }
    void rd(unsigned r, uint64_t us) {
        adv(us);
        f.read(r, 2 * us);
    }
    uint8_t st() { return f.status(now); }
    uint8_t reg(unsigned r) { return f.reg(r); }
    uint8_t sys() { return f.sys(); }
};

struct Ev {
    char kind; // D - DRQ, I - INTRQ, F - после Force Interrupt, T - не кончилась
    long t;
    uint8_t v, st, trk, sec;
};

struct Step {
    const char* name;
    std::vector<std::pair<unsigned, uint8_t>> regs; // перед командой; 4 - #FF
    uint8_t cmd;
    long at;      // мкс после регистров до команды
    int rd;       // мкс от DRQ до обслуживания; -1 - не обслуживать
    long late;    // Force Interrupt через late мкс (0 - нет)
    long tol = 0; // допуск по времени, мкс; 0 - по типу команды
};

template <class M>
std::vector<Ev> run(M& m, const Step& s, uint64_t t0, uint64_t& t_end) {
    constexpr long kMax = 1500000;
    std::vector<Ev> ev;
    for (const auto& r : s.regs) {
        m.w(r.first, r.second, t0 + 10);
    }
    const uint64_t tc = t0 + 100 + s.at;
    m.w(0, s.cmd, tc);
    bool drq    = false;
    long drq_at = -1;
    int n       = 0;
    for (long us = 1; us < kMax; us++) {
        m.adv(tc + us);
        if (s.late != 0 && us == s.late) m.w(0, 0xD0, tc + us);
        const uint8_t sys = m.sys();
        if ((sys & 0x40) != 0 && !drq) {
            ev.push_back(Ev{'D', us, m.reg(3), 0, 0, 0});
            drq_at = us;
        }
        drq = (sys & 0x40) != 0;
        if (s.rd >= 0 && drq_at >= 0 && us == drq_at + s.rd) {
            if ((s.cmd & 0xE0) == 0xA0 || (s.cmd & 0xF0) == 0xF0) {
                m.w(3, static_cast<uint8_t>(n * 7 + 3), tc + us);
            } else {
                m.rd(3, tc + us);
            }
            n++;
            drq_at = -1;
            drq    = (m.sys() & 0x40) != 0;
        }
        if ((sys & 0x80) != 0 || (s.late != 0 && us > s.late + 200)) {
            ev.push_back(Ev{(sys & 0x80) != 0 ? 'I' : 'F', us, 0, m.st(), m.reg(1), m.reg(2)});
            t_end = tc + us;
            m.rd(0, tc + us);
            return ev;
        }
    }
    ev.push_back(Ev{'T', kMax, 0, m.st(), m.reg(1), m.reg(2)});
    t_end = tc + kMax;
    return ev;
}

} // namespace

void test_fdc_vs_vg93w() {
    std::printf("test_bdi_fdc_vs_vg93w\n");
    format_disk();
    static Oracle o;
    static Fsm f;
    o.init();
    f.init();
    o.w(4, 0x3C, 0);
    f.w(4, 0x3C, 0);
    o.w(0, 0xD0, 200);
    f.w(0, 0xD0, 200);
    o.rd(0, 400);
    f.rd(0, 400);
    // Format пишет мусор вместо разметки: после него на дорожке 0 заголовков нет.
    const std::vector<Step> steps = {
        {"restore h", {}, 0x08, 0, -1, 0},
        {"seek 5 r0", {{3, 5}}, 0x18, 0, -1, 0},
        {"seek 2 r3", {{3, 2}}, 0x1B, 300, -1, 0},
        {"seek 4 V r0", {{3, 4}}, 0x1C, 700, -1, 0},
        {"step-in u", {}, 0x58, 0, -1, 0},
        {"step-out", {}, 0x68, 0, -1, 0},
        {"restore V", {}, 0x0C, 0, -1, 0},
        {"read 1", {{2, 1}}, 0x80, 3200, 10, 0},
        {"read 1 next rev", {{2, 1}}, 0x80, 6400, 10, 0},
        {"read 9 lost", {{2, 9}}, 0x80, 3200, 40, 0},
        {"read 17 rnf", {{2, 17}}, 0x80, 0, 10, 0},
        {"read 1 E", {{2, 1}}, 0x84, 3200, 10, 0},
        {"read multi 15", {{2, 15}}, 0x90, 3200, 10, 0},
        {"read addr", {}, 0xC0, 3200, 10, 0},
        {"read addr a5000", {}, 0xC0, 160000, 10, 0},
        {"write 3", {{2, 3}}, 0xA0, 3200, 10, 0},
        // Кристалл начинает запись через 22.12 байта после заголовка, автомат -
        // по сетке байтов: записанное поле сдвинуто меньше чем на байт.
        {"read 3 back", {{2, 3}}, 0x80, 3200, 10, 0, 32},
        // Несколько секторов: 15 и 16, на 17 - RNF; второй сектор обязан дойти.
        {"write multi 15", {{2, 15}}, 0xB0, 3200, 10, 0},
        {"read 16 back", {{2, 16}}, 0x80, 3200, 10, 0, 32},
        {"write 3 nodata", {{2, 3}}, 0xA0, 3200, -1, 0},
        {"seek 1", {{3, 1}}, 0x18, 0, -1, 0},
        {"side1 read 5 C", {{4, 0x2C}, {2, 5}}, 0x8A, 0, 10, 0},
        {"side1 read 5 C wrong", {{2, 5}}, 0x82, 0, 10, 0},
        {"side0 back", {{4, 0x3C}, {2, 2}}, 0x80, 0, 10, 0},
        {"force mid search", {{2, 16}}, 0x80, 0, 10, 3000},
        {"seek 81", {{3, 81}}, 0x18, 0, -1, 0},
        {"read bad data crc 4", {{2, 4}}, 0x80, 0, 10, 0},
        {"seek 82", {{3, 82}}, 0x18, 0, -1, 0},
        {"read bad id 6", {{2, 6}}, 0x80, 0, 10, 0},
        {"read deleted 14", {{2, 14}}, 0x80, 0, 10, 0},
        {"verify 82", {}, 0x14, 0, -1, 0},
        {"step-in V no track", {}, 0x4C, 0, -1, 0},
        {"restore", {}, 0x08, 0, -1, 0},
        {"format", {}, 0xF0, 3200, 10, 0},
        {"read addr after format", {}, 0xC0, 3200, 10, 0},
        {"seek 0 again", {{3, 0}}, 0x18, 0, -1, 0},
        {"unload wait then read", {{2, 1}}, 0x80, 3100000, 10, 0},
    };
    uint64_t t0 = 1000000;
    for (const Step& s : steps) {
        uint64_t e1 = 0, e2 = 0;
        const auto eo  = run(o, s, t0, e1);
        const auto ef  = run(f, s, t0, e2);
        t0             = (e1 > e2 ? e1 : e2) + 1000;
        const long tol = s.tol != 0 ? s.tol : s.cmd < 0x80 ? 1100 : 3;
        bool same      = eo.size() == ef.size();
        // DATA при DRQ записи - последний байт, собранный при поиске: Z80 его
        // не читает, автомат его не ведёт.
        const bool data_matters = (s.cmd & 0xE0) != 0xA0 && (s.cmd & 0xF0) != 0xF0;
        for (size_t i = 0; same && i < eo.size(); i++) {
            const Ev& a = eo[i];
            const Ev& b = ef[i];
            same        = a.kind == b.kind && labs(a.t - b.t) <= tol && (!data_matters || a.v == b.v) &&
                   (a.kind == 'D' || (a.st == b.st && a.trk == b.trk && a.sec == b.sec));
            if (!same) {
                std::printf("  %s #%zu: vg93w %c+%ld v=%02X st=%02X trk=%u sec=%u, fsm %c+%ld v=%02X st=%02X trk=%u sec=%u\n", s.name, i, a.kind, a.t, a.v,
                            a.st, a.trk, a.sec, b.kind, b.t, b.v, b.st, b.trk, b.sec);
            }
        }
        if (eo.size() != ef.size()) std::printf("  %s: events vg93w %zu, fsm %zu\n", s.name, eo.size(), ef.size());
        CHECK(same);
    }
}

// Упор головки - 83-й цилиндр; у образа длиннее - его последний.
void test_fdc_head_limit() {
    std::printf("test_bdi_fdc_head_limit\n");
    struct Long final : Disk {
        int cyls = 0;
        Track t[2];
        Track* cylinder(int) override { return t; }
        void written(int, int) override {}
        bool write_protect() const override { return true; }
        int cylinders() const override { return cyls; }
    };
    static Long src;
    track_build(src.t[0], nullptr, 0, 0);
    track_build(src.t[1], nullptr, 0, 0);
    for (const int cyls : {80, 128}) {
        src.cyls = cyls;
        Vg93 f;
        f.reset(0);
        f.insert(0, &src, 0);
        auto w = [&f](unsigned r, uint8_t v, uint64_t us) {
            while (f.due() <= 2 * us) {
                f.step();
            }
            f.write(r, v, 2 * us);
        };
        // Дисковод выбирает HLD: команды - с загрузкой головки.
        w(4, 0x3C, 0);
        w(0, 0xD0, 200);
        w(0, 0x08, 1000); // Restore
        w(3, 120, 500000);
        w(0, 0x18, 500100); // Seek на 120-й
        w(3, 120, 4000000);
        CHECK_EQ(f.cylinder(), cyls == 128 ? 120 : 83);
        CHECK_EQ(f.reg(1), 120);
    }
}
