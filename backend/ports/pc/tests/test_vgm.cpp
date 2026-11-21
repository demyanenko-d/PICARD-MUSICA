// SPDX-License-Identifier: MIT
// Плеер VGM: разбор заголовка, записи по отсчётам, независимость выхода от
// нарезки вызовов, стерео чипов, петля, обрыв на неизвестной команде,
// загрузка трека в память трека из файла и из обёртки gzip.

#include "testing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <utility>
#include <vector>

#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "core/model/song.h"
#include "player/protocol/track_meta.h"
#include "player/vgm/vgm_track.h"

namespace {

using player::vgm::Chip;
using player::vgm::Info;
using player::vgm::Player;

// Файл VGM 1.71 с заголовком 0x100 байт.
struct File {
    std::vector<uint8_t> b = std::vector<uint8_t>(0x100);
    uint32_t samples       = 0;
    File() {
        std::memcpy(b.data(), "Vgm ", 4);
        put32(0x08, 0x171);
        put32(0x34, 0x100 - 0x34);
    }
    void put32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) {
            b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
        }
    }
    File& cmd(std::initializer_list<int> bytes) {
        for (const int v : bytes) {
            b.push_back(static_cast<uint8_t>(v));
        }
        return *this;
    }
    File& wait(uint32_t n) {
        samples += n;
        return cmd({0x61, static_cast<int>(n & 0xff), static_cast<int>(n >> 8)});
    }
    // Петля - с этого места.
    File& loop_here() {
        put32(0x1c, static_cast<uint32_t>(b.size()) - 0x1c);
        loop_at = samples;
        return *this;
    }
    File& end() {
        put32(0x18, samples);
        if (b[0x1c] != 0 || b[0x1d] != 0) put32(0x20, samples - loop_at);
        return cmd({0x66});
    }
    uint32_t loop_at = 0;
};

struct Sound {
    std::vector<int32_t> l, r;
    bool ended = false;
};

// Весь файл кусками по chunk отсчётов, всего frames.
Sound play(const File& f, uint32_t frames, uint32_t chunk, uint32_t loops = 1) {
    Info info;
    Sound s;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
    std::vector<uint8_t> mem(Player::memory_need(info));
    Player p;
    CHECK(p.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), loops));
    s.l.assign(frames, 0);
    s.r.assign(frames, 0);
    for (uint32_t at = 0; at < frames; at += chunk) {
        const uint32_t n = frames - at < chunk ? frames - at : chunk;
        s.ended          = !p.render_add(s.l.data() + at, s.r.data() + at, n);
    }
    return s;
}

bool silent(const std::vector<int32_t>& v, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) {
        if (v[i] != 0) return false;
    }
    return true;
}

// Нота на канале 0 OPL3: несущая - синус в полный уровень, выходы pan.
void opl3_note(File& f, int pan) {
    f.cmd({0x5f, 0x05, 0x01});                         // режим OPL3
    f.cmd({0x5e, 0x20, 0x21}).cmd({0x5e, 0x23, 0x21}); // удержание, множитель 1
    f.cmd({0x5e, 0x40, 0x3f}).cmd({0x5e, 0x43, 0x00}); // модулятор молчит
    f.cmd({0x5e, 0x60, 0xf0}).cmd({0x5e, 0x63, 0xf0});
    f.cmd({0x5e, 0x80, 0x0f}).cmd({0x5e, 0x83, 0x0f});
    f.cmd({0x5e, 0xc0, pan});
    f.cmd({0x5e, 0xa0, 0x98}).cmd({0x5e, 0xb0, 0x31}); // нажатие, блок 4
}

void test_vgm_header() {
    std::printf("test_vgm_header\n");
    File f;
    f.put32(0x0c, 3579545 | 0x40000000u); // два SN76489
    f.b[0x28] = 0x03, f.b[0x2a] = 15, f.b[0x2b] = 0x01;
    f.put32(0x5c, 14318180);
    f.put32(0x74, 2000000 | 0x40000000u);
    f.b[0x78] = 0x10, f.b[0x79] = 0x10; // YM2149, вход делится на два
    f.put32(0xc8, 8000000);
    f.put32(0x38, 4000000); // Sega PCM плеер не играет
    f.wait(100).loop_here().wait(50).end();
    Info info;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
    CHECK(info.version == 0x171 && info.data_start == 0x100 && info.total_samples == 150 && info.loop_samples == 50);
    CHECK_EQ(info.loop_start, 0x103u);
    CHECK_EQ(info.count, 6u);
    CHECK(info.chips[0].kind == Chip::Sn76489 && !info.chips[0].second && info.chips[0].cmd == 0x50 && info.chips[1].second && info.chips[1].cmd == 0x30);
    CHECK(info.chips[0].sn.taps == 3 && info.chips[0].sn.width == 15 && info.chips[0].sn.freq0_max && info.chips[0].sn.divide8);
    CHECK(info.chips[2].kind == Chip::Opl3 && info.chips[2].clock == 14318180 && info.chips[2].cmd == 0x5e);
    CHECK(info.chips[3].kind == Chip::Ay8910 && info.chips[3].ym && info.chips[3].clock == 1000000 && info.chips[4].second);
    CHECK(info.chips[5].kind == Chip::Saa1099);
    CHECK(info.unsupported != nullptr && std::strcmp(info.unsupported, "Sega PCM") == 0);
    // Версия 1.50: заголовок 0x40 байт, полей чипов дальше него нет.
    std::vector<uint8_t> old(f.b.begin(), f.b.begin() + 0x40);
    old[0x08] = 0x50, old[0x09] = 0x01, old[0x34] = 0x0c;
    std::memset(old.data() + 0x38, 0, 4); // Sega PCM лежит в первых 0x40 байтах
    old.push_back(0x66);
    CHECK(player::vgm::parse(old.data(), static_cast<uint32_t>(old.size()), info));
    CHECK(info.data_start == 0x40 && info.count == 2 && info.unsupported == nullptr);
    old[0] = 'X';
    CHECK(!player::vgm::parse(old.data(), static_cast<uint32_t>(old.size()), info));
    // OPL2: пара YM3812 и YM3526 - имя по номеру чипа, у второго в паре тоже.
    File o;
    o.put32(0x50, 3579545 | 0x40000000u);
    o.put32(0x54, 3579545 | 0x40000000u);
    o.wait(100).end();
    CHECK(player::vgm::parse(o.b.data(), static_cast<uint32_t>(o.b.size()), info) && info.count == 4);
    CHECK(std::strcmp(player::vgm::part_name(info.chips[1]), "YM3812") == 0);
    CHECK(std::strcmp(player::vgm::part_name(info.chips[2]), "YM3526") == 0);
    CHECK(std::strcmp(player::vgm::part_name(info.chips[3]), "YM3526") == 0);
}

// Набор чипов разом: выход не зависит от того, какими кусками его просят.
void test_vgm_chunks() {
    std::printf("test_vgm_chunks\n");
    File f;
    f.put32(0x0c, 3579545);
    f.put32(0x44, 3993600);
    f.put32(0x50, 3579545);
    f.put32(0x5c, 14318180);
    f.put32(0x74, 1789772);
    f.put32(0xc8, 8000000);
    f.wait(10);
    f.cmd({0x50, 0x80 | 0x0e}).cmd({0x50, 0x01}).cmd({0x50, 0x90}); // SN: тон 0, громкость
    f.wait(3);
    f.cmd({0xa0, 0x00, 0x40}).cmd({0xa0, 0x07, 0x3e}).cmd({0xa0, 0x08, 0x0f}); // AY: тон A
    f.wait(70);
    f.cmd({0xbd, 0x1c, 0x01}).cmd({0xbd, 0x00, 0x8f}).cmd({0xbd, 0x08, 0x40}).cmd({0xbd, 0x10, 0x03}).cmd({0xbd, 0x14, 0x01}); // SAA: тон 0
    f.wait(1);
    opl3_note(f, 0x31);
    f.wait(200);
    // YM2203: тон SSG и нота FM на канале 0.
    f.cmd({0x55, 0x00, 0x80}).cmd({0x55, 0x07, 0x3e}).cmd({0x55, 0x08, 0x0c});
    f.cmd({0x55, 0x30, 0x01}).cmd({0x55, 0x40, 0x00}).cmd({0x55, 0x50, 0x1f}).cmd({0x55, 0x80, 0x0f});
    f.cmd({0x55, 0x3c, 0x01}).cmd({0x55, 0x4c, 0x00}).cmd({0x55, 0x5c, 0x1f}).cmd({0x55, 0x8c, 0x0f});
    f.cmd({0x55, 0xb0, 0x07}).cmd({0x55, 0xa4, 0x22}).cmd({0x55, 0xa0, 0x69}).cmd({0x55, 0x28, 0xf0});
    f.wait(130);
    // OPL2: нота на канале 0 с формой волны, разрешённой регистром 1.
    f.cmd({0x5a, 0x01, 0x20}).cmd({0x5a, 0x23, 0x21}).cmd({0x5a, 0x43, 0x00}).cmd({0x5a, 0x63, 0xf0}).cmd({0x5a, 0x83, 0x0f});
    f.cmd({0x5a, 0xe3, 0x01}).cmd({0x5a, 0xc0, 0x01}).cmd({0x5a, 0xa0, 0x98}).cmd({0x5a, 0xb0, 0x31});
    f.wait(2000).end();
    const uint32_t frames = f.samples + 300;
    const Sound whole     = play(f, frames, frames);
    CHECK(whole.ended);
    CHECK(silent(whole.l, 0, 2));
    CHECK(!silent(whole.l, 2000, frames) && !silent(whole.r, 2000, frames));
    for (const uint32_t chunk : {1u, 7u, 64u, 100u, 441u}) {
        const Sound parts = play(f, frames, chunk);
        CHECK(parts.l == whole.l && parts.r == whole.r);
    }
}

// Разряды выхода канала OPL3 действуют ровно с отсчёта записи.
void test_vgm_opl3_outputs() {
    std::printf("test_vgm_opl3_outputs\n");
    File f;
    f.put32(0x5c, 14318180);
    opl3_note(f, 0x11); // только левая сторона
    f.wait(1000);
    f.cmd({0x5e, 0xc0, 0x21}); // только правая
    f.wait(1000).end();
    const Sound s = play(f, 2000, 333);
    CHECK(!silent(s.l, 100, 1000) && silent(s.r, 0, 1000));
    CHECK(silent(s.l, 1000, 2000) && !silent(s.r, 1000, 2000));
    // Без режима OPL3 канал идёт в обе стороны.
    File g;
    g.put32(0x5c, 14318180);
    opl3_note(g, 0x01);
    g.b[0x100 + 2] = 0x00; // режим OPL3 не включён
    g.wait(1000).end();
    const Sound both = play(g, 1000, 1000);
    CHECK(!silent(both.l, 100, 1000) && both.l == both.r);
    // Выходы A и C сводятся в одну сторону: два выхода - уровень вдвое.
    // Без режима OPL3 канал идёт во все четыре.
    File h;
    h.put32(0x5c, 14318180);
    opl3_note(h, 0x51); // выходы A и C
    h.wait(1000).end();
    const Sound ac = play(h, 1000, 1000);
    int64_t one = 0, two = 0, four = 0;
    for (size_t i = 0; i < 1000; i++) {
        one  += std::abs(s.l[i]);
        two  += std::abs(ac.l[i]);
        four += std::abs(both.l[i]);
    }
    CHECK(one > 0 && silent(ac.r, 0, 1000));
    CHECK(two > one * 2 - one / 50 && two < one * 2 + one / 50);
    CHECK(four > one * 2 - one / 50 && four < one * 2 + one / 50);
}

// Стерео Game Gear и второй чип пары AY по разряду 7 регистра.
void test_vgm_stereo_and_pairs() {
    std::printf("test_vgm_stereo_and_pairs\n");
    File f;
    f.put32(0x0c, 3579545);
    f.cmd({0x4f, 0x10});                                            // тон 0 только слева
    f.cmd({0x50, 0x80 | 0x0e}).cmd({0x50, 0x01}).cmd({0x50, 0x90}); // тон 0 в полную громкость
    f.wait(1000).end();
    const Sound gg = play(f, 1000, 64);
    CHECK(!silent(gg.l, 100, 1000) && silent(gg.r, 0, 1000));

    // Один AY из пары молчит, пока пишут в другой.
    File a;
    a.put32(0x74, 1789772 | 0x40000000u);
    a.cmd({0xa0, 0x80, 0x40}).cmd({0xa0, 0x87, 0x3e}).cmd({0xa0, 0x88, 0x0f});
    a.wait(500).end();
    File b;
    b.put32(0x74, 1789772);
    b.cmd({0xa0, 0x00, 0x40}).cmd({0xa0, 0x07, 0x3e}).cmd({0xa0, 0x08, 0x0f});
    b.wait(500).end();
    const Sound second = play(a, 500, 64), single = play(b, 500, 64);
    CHECK(!silent(single.l, 100, 500));
    // Громкость чипа пары - половина одиночного.
    bool half = true;
    for (size_t i = 0; i < single.l.size(); i++) {
        half = half && second.l[i] * 2 == single.l[i];
    }
    CHECK(half);
}

// Громкость чипа: основная по виду, у пары - половина, набор нормируется
// удвоениями, усиление заголовка - 2^(n/32), поправки - в дополнительном
// заголовке.
void test_vgm_volumes() {
    std::printf("test_vgm_volumes\n");
    auto parsed = [](const File& f) {
        Info info;
        CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
        return info;
    };
    auto volume = [](const Info& info, uint32_t i) { return uint32_t{info.chips[i].volume}; };
    auto ssg    = [](const Info& info, uint32_t i) { return uint32_t{info.chips[i].ssg_volume}; };
    {
        File f; // один OPL3: его вес в сумме - две единицы, сдвига нет
        f.put32(0x5c, 14318180);
        CHECK_EQ(volume(parsed(f), 0), 0x100u);
        f.b[0x7c] = 32;
        CHECK_EQ(volume(parsed(f), 0), 0x200u);
        f.b[0x7c] = 16;
        CHECK_EQ(volume(parsed(f), 0), 362u);
        f.b[0x7c] = 0xe0; // -32
        CHECK_EQ(volume(parsed(f), 0), 0x80u);
        f.b[0x7c] = 0xc1; // -64
        CHECK_EQ(volume(parsed(f), 0), 0x40u);
    }
    {
        File f; // два SN76489 по 0x40: сумма 0x80 удваивается дважды
        f.put32(0x0c, 3579545 | 0x40000000u);
        const Info info = parsed(f);
        CHECK(volume(info, 0) == 0x100 && volume(info, 1) == 0x100);
    }
    {
        File f; // OPL2 и AY8910: сумма 0x400 делится пополам
        f.put32(0x50, 3579545);
        f.put32(0x74, 1789772);
        const Info info = parsed(f);
        CHECK(volume(info, 0) == 0x80 && volume(info, 1) == 0x80);
    }
    {
        File f; // YM2203: FM 0x100 и SSG 0x80, сумма 0x180 удваивается
        f.put32(0x44, 3993600);
        Info info = parsed(f);
        CHECK(volume(info, 0) == 0x200 && ssg(info, 0) == 0x100);
        // Дополнительный заголовок за полем 0xBC: SSG - половина основной.
        f.put32(0xbc, 4);
        f.put32(0xc0, 12);
        f.put32(0xc8, 4);
        f.b[0xcc] = 1, f.b[0xcd] = 0x86, f.b[0xcf] = 0x80, f.b[0xd0] = 0x80;
        info = parsed(f);
        CHECK(info.count == 1 && volume(info, 0) == 0x200 && ssg(info, 0) == 0x80);
        // FM - своя громкость 0x200: сумма 0x240, сдвига нет.
        f.b[0xcc] = 2, f.b[0xd1] = 0x06, f.b[0xd3] = 0x00, f.b[0xd4] = 0x02;
        info = parsed(f);
        CHECK(volume(info, 0) == 0x200 && ssg(info, 0) == 0x40);
    }
    // Усиление заголовка +32 - выход вдвое.
    File a, b;
    for (File* f : {&a, &b}) {
        f->put32(0x5c, 14318180);
        opl3_note(*f, 0x30);
        f->wait(2000).end();
    }
    b.b[0x7c]      = 32;
    const Sound sa = play(a, 2000, 64), sb = play(b, 2000, 64);
    CHECK(!silent(sa.l, 100, 2000));
    bool twice = true;
    for (size_t i = 0; i < sa.l.size(); i++) {
        twice = twice && sb.l[i] == sa.l[i] * 2;
    }
    CHECK(twice);
}

void test_vgm_loop_and_end() {
    std::printf("test_vgm_loop_and_end\n");
    File f;
    f.put32(0x0c, 3579545);
    f.cmd({0x50, 0x80 | 0x0e}).cmd({0x50, 0x01});
    f.wait(100).loop_here();
    f.cmd({0x50, 0x90}).wait(50).cmd({0x50, 0x9f}).wait(50).end();
    Info info;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
    std::vector<uint8_t> mem(Player::memory_need(info));
    for (const uint32_t loops : {1u, 3u}) {
        Player p;
        CHECK(p.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), loops));
        const uint32_t total = 100 + 100 * loops;
        CHECK_EQ(p.total_frames(), total);
        // Петля в 100 отсчётов - отметка конца, а не петля: длина трека -
        // длина журнала, затухание - четверть.
        CHECK(!player::vgm::looped(p.info()));
        CHECK_EQ(player::vgm::track_frames(p), 200u);
        CHECK_EQ(player::vgm::track_fade_frames(p), 50u);
        std::vector<int32_t> l(total + 10), r(total + 10);
        CHECK(p.render_add(l.data(), r.data(), total));
        CHECK(!p.render_add(l.data() + total, r.data() + total, 10));
        CHECK(!p.broken());
        // Звучит первая половина каждого прохода петли (с задержкой ступеньки).
        CHECK(!silent(l, total - 80, total - 60) && silent(l, total - 20, total - 5));
    }
    // Петля от секунды: трек - вступление, петля два раза и время затухания.
    File lf;
    lf.put32(0x0c, 3579545);
    lf.wait(100).loop_here().cmd({0x50, 0x90}).wait(44100).end();
    Player lp;
    CHECK(lp.open(lf.b.data(), static_cast<uint32_t>(lf.b.size()), mem.data(), mem.size(), 0));
    CHECK(player::vgm::looped(lp.info()));
    CHECK_EQ(player::vgm::track_frames(lp), 100u + 2u * 44100u + player::vgm::kFadeFrames);
    CHECK_EQ(player::vgm::track_fade_frames(lp), (100u + 2u * 44100u + player::vgm::kFadeFrames) / 4u);
    Player endless;
    CHECK(endless.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), 0));
    CHECK_EQ(endless.total_frames(), 0u);
    std::vector<int32_t> l(5000), r(5000);
    CHECK(endless.render_add(l.data(), r.data(), 5000));
    // Памяти меньше нужного - отказ.
    Player small;
    CHECK(!small.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size() - 1, 1));

    // Петля без пауз не зацикливает разбор; неизвестная команда 0x60-0x9F
    // обрывает журнал, незанятые 0x00-0x2F пропускаются по байту (VGMPlay).
    File dry;
    dry.put32(0x0c, 3579545);
    dry.wait(10).loop_here().cmd({0x50, 0x90});
    dry.put32(0x20, 7);
    dry.cmd({0x66});
    const Sound d = play(dry, 100, 64, 0);
    CHECK(d.ended);
    File bad;
    bad.put32(0x0c, 3579545);
    bad.wait(10).cmd({0x65}).wait(10).end();
    Player q;
    std::vector<int32_t> ql(100), qr(100);
    CHECK(q.open(bad.b.data(), static_cast<uint32_t>(bad.b.size()), mem.data(), mem.size(), 1));
    CHECK(!q.render_add(ql.data(), qr.data(), 100) && q.broken());
    File odd;
    odd.put32(0x0c, 3579545);
    odd.wait(10).cmd({0x00}).cmd({0x21}).cmd({0x50, 0x90}).wait(200).end();
    Player o;
    std::vector<int32_t> ol(210), orr(210);
    CHECK(o.open(odd.b.data(), static_cast<uint32_t>(odd.b.size()), mem.data(), mem.size(), 1));
    CHECK(o.render_add(ol.data(), orr.data(), 210) || !o.broken());
    CHECK(!o.broken());
    CHECK(!silent(ol, 100, 200));
    // Блок данных и команды чужих чипов пропускаются по длине.
    File skip;
    skip.put32(0x0c, 3579545);
    skip.cmd({0x67, 0x66, 0x00, 3, 0, 0, 0, 1, 2, 3}).cmd({0x52, 0x2a, 0x80}).cmd({0xe0, 0, 0, 0, 0}).cmd({0x90, 0, 2, 0, 0x2a}).cmd({0x81});
    skip.samples += 1;
    skip.cmd({0x50, 0x80 | 0x0e}).cmd({0x50, 0x01}).cmd({0x50, 0x90}).wait(500).end();
    Player s;
    std::vector<int32_t> sl(600), sr(600);
    CHECK(s.open(skip.b.data(), static_cast<uint32_t>(skip.b.size()), mem.data(), mem.size(), 1));
    CHECK(s.render_add(sl.data(), sr.data(), 500) && !s.broken() && !silent(sl, 100, 500));
}

// Снятие и взятие ноты в одном отсчёте перезапускают её: так пишут журналы
// эмуляторы, и между двумя записями нет ни одного такта чипа.
void test_vgm_restart_in_one_sample() {
    std::printf("test_vgm_restart_in_one_sample\n");
    auto peak = [](const std::vector<int32_t>& v, size_t from, size_t to) {
        int32_t m = 0;
        for (size_t i = from; i < to; i++) {
            m = std::max(m, v[i] < 0 ? -v[i] : v[i]);
        }
        return m;
    };
    for (const bool restart : {true, false}) {
        File f;
        f.put32(0x5c, 14318180);
        opl3_note(f, 0x31);
        // Несущая без удержания: после атаки спад до тишины.
        f.cmd({0x5e, 0x23, 0x01}).cmd({0x5e, 0x63, 0xfa}).cmd({0x5e, 0x83, 0xff});
        f.wait(30000);
        if (restart) f.cmd({0x5e, 0xb0, 0x11}).cmd({0x5e, 0xb0, 0x31});
        f.wait(3000).end();
        const Sound s      = play(f, 33000, 64);
        const int32_t loud = peak(s.l, 100, 1000), faded = peak(s.l, 29000, 30000), again = peak(s.l, 30100, 31000);
        CHECK(loud > 0 && faded < loud / 64);
        CHECK(restart ? again > loud / 2 : again <= faded);
    }
}

// Мощность сигнала на частоте по Гёрцелю, без первых 0.1 с.
double tone_power(const std::vector<int32_t>& v, double hz) {
    const double k = 2.0 * std::cos(2.0 * 3.14159265358979 * hz / 44100.0);
    double a = 0, b = 0;
    for (size_t i = 4410; i < v.size(); i++) {
        const double x = v[i] + k * a - b;
        b              = a;
        a              = x;
    }
    return a * a + b * b - k * a * b;
}

// То же в полосе 600 Гц вокруг частоты: самовозбуждение - гроздь линий с
// шагом в частоту ноты.
double band_power(const std::vector<int32_t>& v, double hz) {
    double sum = 0;
    for (int d = -300; d <= 300; d += 20) {
        sum += tone_power(v, hz + d);
    }
    return sum;
}

// Такт чипа - из частоты в заголовке: петля обратной связи оператора идёт по
// отсчётам чипа. При обратной связи 5 и полном уровне она самовозбуждается
// с периодом 3 отсчёта - это треть частоты чипа (вход / 72 / 3), а не треть
// частоты выхода, при любой частоте входа; высота ноты идёт за ней же.
void test_vgm_feedback_at_chip_rate() {
    std::printf("test_vgm_feedback_at_chip_rate\n");
    for (const uint32_t clock : {2500000u, 3579545u, 4000000u}) {
        File f;
        f.put32(0x50, clock);
        f.cmd({0x5a, 0x20, 0x21}).cmd({0x5a, 0x40, 0x00}).cmd({0x5a, 0x60, 0xf0}).cmd({0x5a, 0x80, 0x0f}); // оператор 1 в полный уровень
        f.cmd({0x5a, 0x23, 0x21}).cmd({0x5a, 0x43, 0x3f}).cmd({0x5a, 0x63, 0xf0}).cmd({0x5a, 0x83, 0x0f}); // оператор 2 молчит
        f.cmd({0x5a, 0xc0, 0x0b});                                                                         // обратная связь 5, сложение
        f.cmd({0x5a, 0xa0, 0x81}).cmd({0x5a, 0xb0, 0x2d});
        f.wait(44100).end();
        const Sound s     = play(f, 44100, 64);
        const double chip = band_power(s.l, clock / 72.0 / 3.0), out = band_power(s.l, 44100.0 / 3.0);
        CHECK(chip > 0 && chip > 64.0 * out);
    }
    // OPL3 на 20 МГц вместо 14.31818: отсчёт чипа 69444 Гц, нота выше в 1.397 раза.
    for (const uint32_t clock : {14318180u, 20000000u}) {
        File f;
        f.put32(0x5c, clock);
        opl3_note(f, 0x31);
        f.wait(44100).end();
        const Sound s = play(f, 44100, 64);
        // Номер частоты 0x198, блок 4: частота = номер * отсчёт чипа / 2^16.
        const double own = 0x198 * (clock / 288.0) / 65536.0, other = 0x198 * ((clock == 20000000u ? 14318180.0 : 20000000.0) / 288.0) / 65536.0;
        CHECK(tone_power(s.l, own) > 64.0 * tone_power(s.l, other));
    }
}

// Трек из источника байтов: журнал в PSRAM трека, плеер и чипы в арене;
// звук тот же, что у плеера поверх файла в памяти.
void test_vgm_track() {
    std::printf("test_vgm_track\n");
    File f;
    f.put32(0x5c, 14318180);
    opl3_note(f, 0x31);
    f.wait(600).end();
    const Sound direct = play(f, 600, 64);
    CHECK(!silent(direct.l, 100, 600));

    // Та же запись в обёртке gzip одним блоком без сжатия.
    const uint32_t n        = static_cast<uint32_t>(f.b.size());
    std::vector<uint8_t> gz = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff, 0x01};
    for (const uint32_t v : {n, ~n}) {
        gz.push_back(static_cast<uint8_t>(v));
        gz.push_back(static_cast<uint8_t>(v >> 8));
    }
    gz.insert(gz.end(), f.b.begin(), f.b.end());
    for (const uint32_t v : {0u, n}) { // сумма не проверяется, длина - из хвоста
        for (int i = 0; i < 4; i++) {
            gz.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }
    CHECK(player::vgm::sniff(f.b.data(), 4) && player::vgm::sniff(gz.data(), 4) && !player::vgm::sniff(reinterpret_cast<const uint8_t*>("IMPM"), 4));

    auto* mem = new soundsinth::memory::TrackMemory();
    soundsinth::memory::track_memory_create(*mem);
    for (const std::vector<uint8_t>* file : {&f.b, &gz}) {
        soundsinth::formats::MemoryByteSource src(file->data(), static_cast<uint32_t>(file->size()));
        const char* error = nullptr;
        Player* const p   = player::vgm::load_track(src.as_byte_source(), *mem, &error);
        CHECK(p != nullptr);
        if (p == nullptr) continue;
        CHECK_EQ(player::vgm::track_frames(*p), 600u);
        CHECK_EQ(player::vgm::track_fade_frames(*p), 150u);
        // Журнал лежит в PSRAM трека, плеер - в арене; сценарий распаковки снят.
        CHECK(reinterpret_cast<uint8_t*>(p) >= mem->resident.base && reinterpret_cast<uint8_t*>(p) < mem->resident.base + mem->resident.capacity);
        CHECK(mem->scratch.tenant == soundsinth::memory::Scratch::None);
        std::vector<int32_t> l(600), r(600);
        CHECK(p->render_add(l.data(), r.data(), 600));
        CHECK(l == direct.l && r == direct.r);
    }
    // Обрезанная обёртка, не журнал внутри, не тот файл - отказ с причиной.
    for (const size_t cut : {gz.size() - 100, size_t{20}}) {
        soundsinth::formats::MemoryByteSource src(gz.data(), static_cast<uint32_t>(cut));
        const char* error = nullptr;
        CHECK(player::vgm::load_track(src.as_byte_source(), *mem, &error) == nullptr && error != nullptr);
    }
    std::vector<uint8_t> junk(0x100, 0x55);
    std::memcpy(junk.data(), "Vgm ", 4);
    soundsinth::formats::MemoryByteSource src(junk.data(), static_cast<uint32_t>(junk.size()));
    const char* error = nullptr;
    CHECK(player::vgm::load_track(src.as_byte_source(), *mem, &error) == nullptr && error != nullptr);
    soundsinth::memory::track_memory_destroy(*mem);
    delete mem;
}

} // namespace

// YM2612: FM по сторонам, ЦАП вместо канала 6, отсчёты ЦАП из блока данных,
// ступенька ЦАП у YM2612 и её отсутствие у YM3438.
void test_vgm_ym2612() {
    std::printf("test_vgm_ym2612\n");
    // Вес единицы отсчёта канала (9 разрядов): один чип в файле - громкость
    // 0x200.
    const int32_t unit = 16384;
    // Канал 1, схема 7, звучит один оператор в полный уровень.
    auto note = [](File& f, int pan) {
        f.cmd({0x52, 0xb0, 0x07}).cmd({0x52, 0xb4, pan});
        for (int op = 0; op < 4; op++) {
            const int r = 4 * op;
            f.cmd({0x52, 0x30 + r, 0x01}).cmd({0x52, 0x40 + r, op == 3 ? 0x00 : 0x7f}).cmd({0x52, 0x50 + r, 0x1f});
            f.cmd({0x52, 0x60 + r, 0x00}).cmd({0x52, 0x70 + r, 0x00}).cmd({0x52, 0x80 + r, 0x0f});
        }
        f.cmd({0x52, 0xa4, 0x22}).cmd({0x52, 0xa0, 0x69}).cmd({0x52, 0x28, 0xf0});
    };
    // YM3438 (разряд 31 частоты): тишина - ноль.
    File f;
    f.put32(0x2c, 7670453u | 0x80000000u);
    note(f, 0x80); // только левая сторона
    f.wait(2000).end();
    Info info;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Ym2612 && info.chips[0].ym && info.chips[0].clock == 7670453 && info.unsupported == nullptr);
    const Sound s = play(f, 2000, 64);
    CHECK(!silent(s.l, 100, 2000) && silent(s.r, 0, 2000));
    int32_t peak = 0;
    for (const int32_t v : s.l) {
        peak = std::max(peak, std::abs(v));
    }
    CHECK(peak > 200 * unit && peak <= 256 * unit);

    // ЦАП: прямая запись, потом отсчёты банка с места 1; за концом банка и
    // после выключения - ноль.
    File d;
    d.put32(0x2c, 7670453u | 0x80000000u);
    d.cmd({0x67, 0x66, 0x00, 4, 0, 0, 0, 0x80 + 10, 0x80 + 20, 0x80 - 30, 0x80 + 40});
    d.cmd({0x52, 0x2b, 0x80}).cmd({0x52, 0x2a, 0xff}).wait(10);
    d.cmd({0xe0, 1, 0, 0, 0}).cmd({0x83}).cmd({0x82}).cmd({0x81}).cmd({0x85});
    d.cmd({0x52, 0x2a, 0xff}).cmd({0x52, 0x2b, 0x00}).wait(10).end();
    const Sound dac = play(d, 40, 7);
    CHECK(dac.l == dac.r);
    CHECK(dac.l[0] == 254 * unit && dac.l[9] == 254 * unit);
    CHECK(dac.l[10] == 40 * unit && dac.l[12] == 40 * unit && dac.l[13] == -60 * unit && dac.l[14] == -60 * unit && dac.l[15] == 80 * unit);
    CHECK(silent(dac.l, 16, 40));

    // YM2612: ступенька - разрыв у нуля, отрицательный отсчёт сдвинут на -7;
    // молчащий трек - ноль, без постоянной.
    File y;
    y.put32(0x2c, 7670453u);
    y.wait(10).cmd({0x52, 0x2b, 0x80}).cmd({0x52, 0x2a, 0xff}).wait(10).cmd({0x52, 0x2a, 0x00}).wait(10).end();
    const Sound lad = play(y, 30, 30);
    CHECK(lad.l[5] == 0 && lad.l[15] == 254 * unit && lad.l[25] == (-256 - 7) * unit && lad.l == lad.r);

    // Блок данных внутри петли входит в банк один раз: третье чтение - за
    // концом банка и на втором круге.
    File g;
    g.put32(0x2c, 7670453u | 0x80000000u);
    g.cmd({0x52, 0x2b, 0x80}).loop_here();
    g.cmd({0x67, 0x66, 0x00, 2, 0, 0, 0, 0x80 + 10, 0x80 + 20});
    g.cmd({0xe0, 0, 0, 0, 0}).cmd({0x81}).cmd({0x81}).cmd({0x81}).wait(7).end();
    const Sound loop = play(g, 20, 64, 2);
    CHECK(loop.l[0] == 20 * unit && loop.l[1] == 40 * unit && loop.l[2] == 0);
    CHECK(loop.l[10] == 20 * unit && loop.l[11] == 40 * unit && loop.l[12] == 0);

    // Поток ЦАП: четыре отсчёта банка на 22050 Гц - по два отсчёта выхода
    // на каждый, последний держится. Потом блок 1 банка на 44100 Гц, стоп
    // на втором его отсчёте.
    File t;
    t.put32(0x2c, 7670453u | 0x80000000u);
    t.cmd({0x67, 0x66, 0x00, 4, 0, 0, 0, 0x80 + 10, 0x80 + 20, 0x80 + 30, 0x80 + 40});
    t.cmd({0x67, 0x66, 0x00, 3, 0, 0, 0, 0x80 - 5, 0x80 - 6, 0x80 - 7});
    t.cmd({0x52, 0x2b, 0x80});
    t.cmd({0x90, 0, 0x02, 0x00, 0x2a}).cmd({0x91, 0, 0x00, 1, 0}).cmd({0x92, 0, 0x22, 0x56, 0, 0});
    t.cmd({0x93, 0, 0, 0, 0, 0, 0x01, 4, 0, 0, 0}).wait(10);
    t.cmd({0x92, 0, 0x44, 0xac, 0, 0}).cmd({0x95, 0, 1, 0, 0}).wait(2).cmd({0x94, 0}).wait(8).end();
    const Sound st       = play(t, 20, 5);
    const int32_t want[] = {20, 40, 40, 60, 60, 80, 80, 80, 80, 80, -10, -12, -12, -12};
    bool stream          = st.l == st.r;
    for (size_t i = 0; i < std::size(want); i++) {
        stream = stream && st.l[i] == want[i] * unit;
    }
    CHECK(stream);
}

// До версии 1.10 частота FM в заголовке одна на YM2413, YM2612 и YM2151:
// чип файла виден по командам журнала.
void test_vgm_old_version_fm() {
    std::printf("test_vgm_old_version_fm\n");
    auto old = [](uint32_t clock) {
        File f;
        f.b.resize(0x40);
        f.put32(0x08, 0x101);
        f.put32(0x34, 0);
        f.put32(0x10, clock);
        return f;
    };
    Info info;
    auto parse = [&](const File& f) { return player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info); };
    // Команды YM2612: чип один, громкость одиночного - 0x200.
    File a = old(7670453);
    a.wait(10).cmd({0x52, 0x28, 0x00}).cmd({0x53, 0x30, 0x01}).wait(100).end();
    CHECK(parse(a) && info.count == 1 && info.chips[0].kind == Chip::Ym2612 && info.chips[0].clock == 7670453 && info.chips[0].volume == 0x200);
    CHECK(info.unsupported == nullptr);
    // Команды YM2413 рядом с SN76489.
    File b = old(3579545);
    b.put32(0x0c, 3579545);
    b.cmd({0x50, 0x9f}).cmd({0x51, 0x10, 0x80}).wait(100).end();
    CHECK(parse(b) && info.count == 2 && info.chips[0].kind == Chip::Sn76489 && info.chips[1].kind == Chip::Ym2413 && info.unsupported == nullptr);
    // Команды YM2151.
    File c = old(3579545);
    c.cmd({0x54, 0x08, 0x00}).wait(100).end();
    CHECK(parse(c) && info.count == 1 && info.chips[0].kind == Chip::Ym2151 && info.chips[0].clock == 3579545 && info.unsupported == nullptr);
    // Команд FM нет: частота в заголовке чипа не заводит.
    File d = old(3579545);
    d.put32(0x0c, 3579545);
    d.cmd({0x50, 0x9f}).wait(100).end();
    CHECK(parse(d) && info.count == 1 && info.chips[0].kind == Chip::Sn76489);
    // С версии 1.10 поле 0x10 - частота YM2413, что бы ни писал журнал.
    File e;
    e.put32(0x10, 3579545);
    e.cmd({0x52, 0x28, 0x00}).wait(100).end();
    CHECK(parse(e) && info.count == 1 && info.chips[0].kind == Chip::Ym2413);
}

// YM2413 с разрядом 31 частоты - VRC7: тембр 1 другой, ритма нет.
void test_vgm_vrc7() {
    std::printf("test_vgm_vrc7\n");
    auto note = [](uint32_t clock) {
        File f;
        f.put32(0x10, clock);
        // Канал 1: тембр 1, полная громкость, нота.
        f.cmd({0x51, 0x30, 0x10}).cmd({0x51, 0x10, 0xac}).cmd({0x51, 0x20, 0x18}).wait(4000).end();
        return f;
    };
    const File opll = note(3579545), vrc7 = note(3579545 | 0x80000000u);
    Info info;
    CHECK(player::vgm::parse(vrc7.b.data(), static_cast<uint32_t>(vrc7.b.size()), info));
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Ym2413 && info.chips[0].ym && info.chips[0].clock == 3579545);
    const Sound a = play(opll, 4000, 64), b = play(vrc7, 4000, 64);
    CHECK(!silent(a.l, 500, 4000) && !silent(b.l, 500, 4000));
    uint32_t differ = 0;
    for (size_t i = 500; i < 4000; i++) {
        if (a.l[i] != b.l[i]) differ++;
    }
    CHECK(differ > 1000);
    // Ритм: у YM2413 бочка звучит, у VRC7 регистра ритма нет.
    auto drum = [](uint32_t clock) {
        File f;
        f.put32(0x10, clock);
        f.cmd({0x51, 0x16, 0x20}).cmd({0x51, 0x26, 0x05}).cmd({0x51, 0x36, 0x00}).cmd({0x51, 0x0e, 0x30}).wait(4000).end();
        return f;
    };
    const Sound c = play(drum(3579545), 4000, 64), d = play(drum(3579545 | 0x80000000u), 4000, 64);
    CHECK(!silent(c.l, 0, 4000) && silent(d.l, 0, 4000));
}

// Тег GD3: строки UTF-16LE по номеру, символ вне ASCII - вопрос.
void test_vgm_gd3() {
    std::printf("test_vgm_gd3\n");
    File f;
    f.put32(0x0c, 3579545);
    f.cmd({0x50, 0x9f}).wait(100).end();
    f.put32(0x14, static_cast<uint32_t>(f.b.size()) - 0x14);
    const size_t tag = f.b.size();
    f.cmd({'G', 'd', '3', ' ', 0, 1, 0, 0, 0, 0, 0, 0});
    for (const char* s : {"Title", "", "Game", "", "System"}) {
        for (; *s != 0; s++) {
            f.cmd({*s, 0});
        }
        f.cmd({0, 0});
    }
    // Шестая строка - японская, седьмая - с буквой вне ASCII.
    f.cmd({0x42, 0x30, 0, 0}).cmd({'A', 0, 0xe9, 0, 'b', 0, 0, 0});
    f.put32(tag + 8, static_cast<uint32_t>(f.b.size() - tag - 12));
    char out[8];
    const uint8_t* d = f.b.data();
    const uint32_t n = static_cast<uint32_t>(f.b.size());
    CHECK(player::vgm::gd3_text(d, n, 0, out, sizeof(out)) && std::strcmp(out, "Title") == 0);
    CHECK(!player::vgm::gd3_text(d, n, 1, out, sizeof(out)) && out[0] == 0);
    CHECK(player::vgm::gd3_text(d, n, 2, out, sizeof(out)) && std::strcmp(out, "Game") == 0);
    CHECK(player::vgm::gd3_text(d, n, 4, out, 4) && std::strcmp(out, "Sys") == 0);
    CHECK(player::vgm::gd3_text(d, n, 5, out, sizeof(out)) && std::strcmp(out, "?") == 0);
    CHECK(player::vgm::gd3_text(d, n, 6, out, sizeof(out)) && std::strcmp(out, "A?b") == 0);
    CHECK(!player::vgm::gd3_text(d, n, 7, out, sizeof(out)));
    // Тега нет или смещение за файлом.
    f.put32(0x14, 0);
    CHECK(!player::vgm::gd3_text(d, n, 0, out, sizeof(out)));
    f.put32(0x14, n);
    CHECK(!player::vgm::gd3_text(d, n, 0, out, sizeof(out)));
}

// Перемотка вперёд: записи пропущенного куска доходят до чипа, журнал
// продолжается с нового места.
void test_vgm_skip() {
    std::printf("test_vgm_skip\n");
    // SN76489: секунда тона с периодом 0x100, затем тон с периодом 0x40.
    File f;
    f.put32(0x0c, 3579545);
    f.cmd({0x50, 0x80}).cmd({0x50, 0x10}).cmd({0x50, 0x90}).wait(44100);
    f.cmd({0x50, 0x80}).cmd({0x50, 0x04}).wait(44100).end();
    Info info;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
    std::vector<uint8_t> mem(Player::memory_need(info));
    auto crossings_after = [&](uint32_t skip) {
        Player p;
        CHECK(p.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), 1));
        if (skip != 0) p.skip(skip);
        std::vector<int32_t> l(4000), r(4000);
        for (uint32_t at = 0; at < 4000; at += 64) {
            p.render_add(l.data() + at, r.data() + at, 4000 - at < 64 ? 4000 - at : 64);
        }
        // Переходы через средний уровень: выход SN76489 однополярный.
        int64_t mean = 0;
        for (size_t i = 500; i < 4000; i++) {
            mean += l[i];
        }
        mean           /= 3500;
        uint32_t count  = 0;
        for (size_t i = 501; i < 4000; i++) {
            count += l[i - 1] < mean && l[i] >= mean ? 1u : 0u;
        }
        return count;
    };
    const uint32_t low = crossings_after(0), high = crossings_after(50000);
    CHECK(low >= 30 && high >= 4 * low - 4 && high <= 4 * low + 4);
    // За конец журнала: он кончается, рендер это видит.
    Player p;
    CHECK(p.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), 1));
    p.skip(200000);
    std::vector<int32_t> l(64), r(64);
    CHECK(!p.render_add(l.data(), r.data(), 64));
}

// Параметры петли заголовка: множитель в 1/16 (0x7F) и база (0x7E) меняют
// число повторов, меньше одного оно не бывает.
void test_vgm_loop_parameters() {
    std::printf("test_vgm_loop_parameters\n");
    auto loops = [](int base, int modifier) {
        File f;
        f.put32(0x0c, 3579545);
        f.b[0x7e] = static_cast<uint8_t>(base);
        f.b[0x7f] = static_cast<uint8_t>(modifier);
        f.cmd({0x50, 0x9f}).wait(100).loop_here().wait(200).end();
        Info info;
        CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
        return player::vgm::track_loops(info);
    };
    CHECK(loops(0, 0) == 2 && loops(0, 0x10) == 2);
    CHECK(loops(0, 8) == 1 && loops(0, 0x20) == 4 && loops(0, 0x80) == 16);
    CHECK(loops(1, 0) == 1 && loops(1, 0x40) == 7);
    CHECK(loops(9, 0) == 1 && loops(-1, 0) == 3);
}

// YM2151: канал звучит в своих сторонах, повтор записей звука не меняет.
void test_vgm_ym2151() {
    std::printf("test_vgm_ym2151\n");
    // Канал 1, схема 7: звучит первый оператор в полный уровень. sides -
    // разряды 7-6 регистра 0x20 (правая, левая).
    auto note = [](int sides, bool again) {
        File f;
        f.put32(0x30, 3579545);
        auto set = [&f, sides] {
            f.cmd({0x54, 0x20, sides | 0x07}).cmd({0x54, 0x28, 0x4a}).cmd({0x54, 0x30, 0x00});
            for (int op = 0; op < 4; op++) {
                const int r = 8 * op;
                f.cmd({0x54, 0x40 + r, 0x01}).cmd({0x54, 0x60 + r, op == 0 ? 0x00 : 0x7f}).cmd({0x54, 0x80 + r, 0x1f});
                f.cmd({0x54, 0xa0 + r, 0x00}).cmd({0x54, 0xc0 + r, 0x00}).cmd({0x54, 0xe0 + r, 0x0f});
            }
            f.cmd({0x54, 0x08, 0x78});
        };
        set();
        f.wait(1000);
        // Те же значения ещё раз посреди ноты, со своим таймером.
        if (again) {
            set();
            f.cmd({0x54, 0x14, 0x15});
        }
        f.wait(1000).end();
        return f;
    };
    const File both = note(0xc0, false);
    Info info;
    CHECK(player::vgm::parse(both.b.data(), static_cast<uint32_t>(both.b.size()), info));
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Ym2151 && info.chips[0].clock == 3579545 && info.chips[0].volume == 0x200 &&
          info.unsupported == nullptr);
    const Sound s = play(both, 2000, 64);
    CHECK(!silent(s.l, 100, 2000) && s.l == s.r);
    const Sound left = play(note(0x40, false), 2000, 64), right = play(note(0x80, false), 2000, 64);
    CHECK(left.l == s.l && silent(left.r, 0, 2000));
    CHECK(right.r == s.r && silent(right.l, 0, 2000));
    const Sound twice = play(note(0xc0, true), 2000, 64);
    CHECK(twice.l == s.l && twice.r == s.r);
    // Уровень: оператор в полный уровень - 8191 единиц из 32767 канала.
    int32_t peak = 0;
    for (size_t i = 500; i < 2000; i++) {
        peak = std::max(peak, std::abs(s.l[i]));
    }
    const int32_t full = 8191 * 256 * 2; // усиление 256, громкость одиночного чипа 0x200
    CHECK(peak > full * 9 / 10 && peak <= full);
}

// YM2610: FM в каналах 2, 3, 5 и 6 (у YM2610B - во всех шести), SSG, ADPCM-A и
// ADPCM-B из образов в блоках данных журнала.
void test_vgm_ym2610() {
    std::printf("test_vgm_ym2610\n");
    // Нота FM в канале движка ch (0-5): схема 7, звучит один оператор.
    auto fm = [](File& f, int ch) {
        const int cmd = ch < 3 ? 0x58 : 0x59, c = ch % 3;
        f.cmd({cmd, 0xb0 + c, 0x07}).cmd({cmd, 0xb4 + c, 0xc0});
        for (int op = 0; op < 4; op++) {
            const int r = 4 * op + c;
            f.cmd({cmd, 0x30 + r, 0x01}).cmd({cmd, 0x40 + r, op == 3 ? 0x00 : 0x7f}).cmd({cmd, 0x50 + r, 0x1f});
            f.cmd({cmd, 0x60 + r, 0x00}).cmd({cmd, 0x70 + r, 0x00}).cmd({cmd, 0x80 + r, 0x0f});
        }
        f.cmd({cmd, 0xa4 + c, 0x22}).cmd({cmd, 0xa0 + c, 0x69}).cmd({0x58, 0x28, 0xf0 | (ch < 3 ? ch : ch + 1)});
    };
    // Блок образа памяти: размер образа, адрес начала, данные.
    auto block = [](File& f, int type, size_t bytes) {
        const uint32_t len = static_cast<uint32_t>(bytes) + 8u;
        f.cmd({0x67, 0x66, type, static_cast<int>(len & 0xff), static_cast<int>(len >> 8), 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0});
        for (size_t i = 0; i < bytes; i++) {
            f.cmd({(i / 16) % 2 != 0 ? 0x08 : 0x70});
        }
    };
    auto chip = [](uint32_t flags) {
        File f;
        f.put32(0x4c, 8000000u | flags);
        return f;
    };
    File a = chip(0);
    fm(a, 1);
    a.wait(2000).end();
    Info info;
    CHECK(player::vgm::parse(a.b.data(), static_cast<uint32_t>(a.b.size()), info));
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Ym2610 && !info.chips[0].ym && info.chips[0].clock == 8000000 && info.unsupported == nullptr);
    CHECK(info.chips[0].volume == 0x200 && info.chips[0].ssg_volume == 0x100);
    const Sound sa = play(a, 2000, 64);
    CHECK(!silent(sa.l, 200, 2000) && sa.l == sa.r);
    // Канала 1 у YM2610 нет, у YM2610B (разряд 31 частоты) есть.
    File first = chip(0), firstb = chip(0x80000000u);
    fm(first, 0);
    fm(firstb, 0);
    first.wait(2000).end();
    firstb.wait(2000).end();
    CHECK(silent(play(first, 2000, 64).l, 0, 2000) && !silent(play(firstb, 2000, 64).l, 200, 2000));
    // SSG: тон канала A.
    File ssg = chip(0);
    ssg.cmd({0x58, 0x00, 0x40}).cmd({0x58, 0x01, 0x00}).cmd({0x58, 0x07, 0x3e}).cmd({0x58, 0x08, 0x0f}).wait(2000).end();
    CHECK(!silent(play(ssg, 2000, 64).l, 200, 2000));
    // ADPCM-A: канал 1 играет образ из блока 0x82; без блока читаются нули.
    auto adpcm_a = [&](bool rom) {
        File f = chip(0);
        if (rom) block(f, 0x82, 0x400);
        f.cmd({0x59, 0x01, 0x3f}).cmd({0x59, 0x08, 0xdf}).cmd({0x59, 0x10, 0x00}).cmd({0x59, 0x18, 0x00});
        f.cmd({0x59, 0x20, 0x03}).cmd({0x59, 0x28, 0x00}).cmd({0x59, 0x00, 0x01}).wait(2000).end();
        return play(f, 2000, 64);
    };
    CHECK(!silent(adpcm_a(true).l, 100, 2000) && adpcm_a(true).l != adpcm_a(false).l);
    // ADPCM-B: образ из блока 0x83.
    auto adpcm_b = [&](bool rom) {
        File f = chip(0);
        if (rom) block(f, 0x83, 0x400);
        f.cmd({0x58, 0x10, 0x00}).cmd({0x58, 0x11, 0xc0}).cmd({0x58, 0x12, 0x00}).cmd({0x58, 0x13, 0x00}).cmd({0x58, 0x14, 0x03}).cmd({0x58, 0x15, 0x00});
        f.cmd({0x58, 0x19, 0x00}).cmd({0x58, 0x1a, 0x40}).cmd({0x58, 0x1b, 0xff}).cmd({0x58, 0x10, 0x80}).wait(2000).end();
        return play(f, 2000, 64);
    };
    CHECK(!silent(adpcm_b(true).l, 100, 2000) && adpcm_b(true).l != adpcm_b(false).l);
}

// YM2608: каналы FM 4-6 слышны с разрядом 7 регистра 0x29, SSG, ADPCM-B из
// образа в блоке 0x81, ритм - из образа ПЗУ сэмплов, без него молчит.
void test_vgm_ym2608() {
    std::printf("test_vgm_ym2608\n");
    auto fm = [](File& f, int ch) {
        const int cmd = ch < 3 ? 0x56 : 0x57, c = ch % 3;
        f.cmd({cmd, 0xb0 + c, 0x07}).cmd({cmd, 0xb4 + c, 0xc0});
        for (int op = 0; op < 4; op++) {
            const int r = 4 * op + c;
            f.cmd({cmd, 0x30 + r, 0x01}).cmd({cmd, 0x40 + r, op == 3 ? 0x00 : 0x7f}).cmd({cmd, 0x50 + r, 0x1f});
            f.cmd({cmd, 0x60 + r, 0x00}).cmd({cmd, 0x70 + r, 0x00}).cmd({cmd, 0x80 + r, 0x0f});
        }
        f.cmd({cmd, 0xa4 + c, 0x22}).cmd({cmd, 0xa0 + c, 0x69}).cmd({0x56, 0x28, 0xf0 | (ch < 3 ? ch : ch + 1)});
    };
    auto chip = [] {
        File f;
        f.put32(0x48, 7987200);
        return f;
    };
    File a = chip();
    fm(a, 0);
    a.wait(2000).end();
    Info info;
    CHECK(player::vgm::parse(a.b.data(), static_cast<uint32_t>(a.b.size()), info));
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Ym2608 && info.chips[0].clock == 7987200 && info.unsupported == nullptr);
    CHECK(info.chips[0].volume == 0x200 && info.chips[0].ssg_volume == 0x100);
    const Sound sa = play(a, 2000, 64);
    CHECK(!silent(sa.l, 200, 2000) && sa.l == sa.r);
    // Канал 4: слышен только в режиме шести каналов.
    File three = chip(), six = chip();
    fm(three, 3);
    six.cmd({0x56, 0x29, 0x80});
    fm(six, 3);
    three.wait(2000).end();
    six.wait(2000).end();
    CHECK(silent(play(three, 2000, 64).l, 0, 2000) && !silent(play(six, 2000, 64).l, 200, 2000));
    // SSG: тон канала A.
    File ssg = chip();
    ssg.cmd({0x56, 0x00, 0x40}).cmd({0x56, 0x01, 0x00}).cmd({0x56, 0x07, 0x3e}).cmd({0x56, 0x08, 0x0f}).wait(2000).end();
    CHECK(!silent(play(ssg, 2000, 64).l, 200, 2000));
    // ADPCM-B: образ памяти из блока 0x81.
    auto adpcm_b = [&](bool rom) {
        File f = chip();
        if (rom) {
            f.cmd({0x67, 0x66, 0x81, 0x08, 0x04, 0, 0, 0, 0, 0x04, 0, 0, 0, 0, 0});
            for (int i = 0; i < 0x400; i++) {
                f.cmd({(i / 16) % 2 != 0 ? 0x08 : 0x70});
            }
        }
        f.cmd({0x57, 0x00, 0x01}).cmd({0x57, 0x00, 0x00}).cmd({0x57, 0x01, 0xc0}).cmd({0x57, 0x02, 0x00}).cmd({0x57, 0x03, 0x00});
        f.cmd({0x57, 0x04, 0x1f}).cmd({0x57, 0x05, 0x00}).cmd({0x57, 0x09, 0x00}).cmd({0x57, 0x0a, 0x40}).cmd({0x57, 0x0b, 0xff});
        f.cmd({0x57, 0x00, 0xa0}).wait(2000).end();
        return play(f, 2000, 64);
    };
    CHECK(!silent(adpcm_b(true).l, 100, 2000) && adpcm_b(true).l != adpcm_b(false).l);
    // Ритм: бочка из образа ПЗУ сэмплов.
    File drum = chip();
    drum.cmd({0x56, 0x11, 0x3f}).cmd({0x56, 0x18, 0xdf}).cmd({0x56, 0x10, 0x01}).wait(2000).end();
    CHECK(silent(play(drum, 2000, 64).l, 0, 2000));
    static uint8_t rom[player::vgm::kRhythmRomBytes];
    for (size_t i = 0; i < sizeof(rom); i++) {
        rom[i] = (i / 16) % 2 != 0 ? 0x08 : 0x70;
    }
    player::vgm::set_rhythm_rom(rom);
    CHECK(!silent(play(drum, 2000, 64).l, 100, 2000));
    player::vgm::set_rhythm_rom(nullptr);

    // Образ от загрузки трека (на плате - файл на карте): зовётся только у
    // файла с YM2608; nullptr - ритм молчит.
    static int s_asked           = 0;
    static const uint8_t* s_give = nullptr;
    auto give                    = [](soundsinth::memory::TrackMemory&) -> const uint8_t* {
        ++s_asked;
        return s_give;
    };
    auto* mem = new soundsinth::memory::TrackMemory();
    soundsinth::memory::track_memory_create(*mem);
    auto drum_via_load = [&](const File& f) {
        soundsinth::formats::MemoryByteSource src(f.b.data(), static_cast<uint32_t>(f.b.size()));
        const char* error = nullptr;
        Player* const p   = player::vgm::load_track(src.as_byte_source(), *mem, &error, nullptr, nullptr, give);
        std::vector<int32_t> l(2000), r(2000);
        CHECK(p != nullptr && p->render_add(l.data(), r.data(), 2000));
        return l;
    };
    s_give = rom;
    CHECK(!silent(drum_via_load(drum), 100, 2000));
    CHECK_EQ(s_asked, 1);
    s_give = nullptr;
    CHECK(silent(drum_via_load(drum), 0, 2000));
    CHECK_EQ(s_asked, 2);
    File other;
    other.put32(0x0c, 3579545); // SN76489
    other.cmd({0x50, 0x90}).wait(100).end();
    soundsinth::formats::MemoryByteSource osrc(other.b.data(), static_cast<uint32_t>(other.b.size()));
    const char* oerr = nullptr;
    CHECK(player::vgm::load_track(osrc.as_byte_source(), *mem, &oerr, nullptr, nullptr, give) != nullptr);
    CHECK_EQ(s_asked, 2);
    player::vgm::set_rhythm_rom(nullptr);
    soundsinth::memory::track_memory_destroy(*mem);
    delete mem;
}

// Чип заголовка, которому журнал ни разу не пишет, не заводится; громкость
// остальных - как с ним.
void test_vgm_unused_chips() {
    std::printf("test_vgm_unused_chips\n");
    Info declared;
    auto opened = [&declared](const File& f) {
        CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), declared));
        std::vector<uint8_t> mem(Player::memory_need(declared));
        Player p;
        CHECK(p.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), 1));
        return p.info();
    };
    // SN76489 и AY8910, пишут только AY.
    File a;
    a.put32(0x0c, 3579545);
    a.put32(0x74, 1789772);
    a.cmd({0xa0, 0x07, 0x38}).wait(100).end();
    Info info = opened(a);
    CHECK(declared.count == 2 && info.count == 1 && info.chips[0].kind == Chip::Ay8910 && info.chips[0].volume == declared.chips[1].volume);
    // Пара AY8910, пишут только второму (разряд 7 номера регистра).
    File b;
    b.put32(0x74, 1789772 | 0x40000000u);
    b.cmd({0xa0, 0x87, 0x38}).wait(100).end();
    info = opened(b);
    CHECK(declared.count == 2 && info.count == 1 && info.chips[0].kind == Chip::Ay8910 && info.chips[0].second);
    // Отсчёт ЦАП из банка - запись YM2612; стерео Game Gear - запись SN76489.
    File c;
    c.put32(0x0c, 3579545);
    c.put32(0x2c, 7670453);
    c.put32(0x74, 1789772);
    c.cmd({0x81}).cmd({0x4f, 0xff}).wait(100).end();
    info = opened(c);
    CHECK(declared.count == 3 && info.count == 2 && info.chips[0].kind == Chip::Sn76489 && info.chips[1].kind == Chip::Ym2612);
    // Не пишут никому: остаётся первый чип, журнал играет тишину до конца.
    File d;
    d.put32(0x0c, 3579545);
    d.put32(0x74, 1789772);
    d.wait(100).end();
    info = opened(d);
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Sn76489);
    const Sound s = play(d, 200, 64);
    CHECK(silent(s.l, 0, 200) && silent(s.r, 0, 200) && s.ended);
}

// Делитель входа YM2203 (регистры 0x2D-0x2F): с делителем 3 тон FM и тон SSG
// вдвое выше, чем с делителем 6, с делителем 2 - FM втрое, SSG вчетверо.
void test_vgm_ym2203_divider() {
    std::printf("test_vgm_ym2203_divider\n");
    // Переходов через среднее вверх за отсчёты 1000-9000.
    auto crossings = [](const std::vector<int32_t>& v) {
        int64_t mean = 0;
        for (size_t i = 1000; i < 9000; i++) {
            mean += v[i];
        }
        mean           /= 8000;
        uint32_t count  = 0;
        for (size_t i = 1001; i < 9000; i++) {
            count += v[i - 1] < mean && v[i] >= mean ? 1u : 0u;
        }
        return count;
    };
    // divider: 6 - как после сброса, 3 - записи 0x2D и 0x2E, 2 - запись 0x2F.
    auto tone = [&](int divider, bool fm) {
        File f;
        f.put32(0x44, 3993600);
        if (divider == 3) f.cmd({0x55, 0x2d, 0}).cmd({0x55, 0x2e, 0});
        if (divider == 2) f.cmd({0x55, 0x2f, 0});
        if (fm) {
            f.cmd({0x55, 0x30, 0x01}).cmd({0x55, 0x40, 0x7f}).cmd({0x55, 0x50, 0x1f}).cmd({0x55, 0x80, 0x0f});
            f.cmd({0x55, 0x3c, 0x01}).cmd({0x55, 0x4c, 0x00}).cmd({0x55, 0x5c, 0x1f}).cmd({0x55, 0x8c, 0x0f});
            f.cmd({0x55, 0xb0, 0x07}).cmd({0x55, 0xa4, 0x22}).cmd({0x55, 0xa0, 0x69}).cmd({0x55, 0x28, 0xf0});
        } else {
            f.cmd({0x55, 0x00, 0x80}).cmd({0x55, 0x01, 0x00}).cmd({0x55, 0x07, 0x3e}).cmd({0x55, 0x08, 0x0f});
        }
        f.wait(9000).end();
        return crossings(play(f, 9000, 64).l);
    };
    auto near          = [](uint32_t got, uint32_t base, uint32_t times) { return got + 2 >= base * times && got <= base * times + 2 + base * times / 50; };
    const uint32_t fm6 = tone(6, true), ssg6 = tone(6, false);
    CHECK(fm6 > 40 && fm6 < 55);     // 261 Гц
    CHECK(ssg6 > 170 && ssg6 < 185); // 975 Гц
    CHECK(near(tone(3, true), fm6, 2) && near(tone(2, true), fm6, 3));
    CHECK(near(tone(3, false), ssg6, 2) && near(tone(2, false), ssg6, 4));
    // Запись того же выбора ничего не меняет: 0x2D после сброса - делитель 6.
    File same;
    same.put32(0x44, 3993600);
    same.cmd({0x55, 0x2d, 0}).cmd({0x55, 0x00, 0x80}).cmd({0x55, 0x07, 0x3e}).cmd({0x55, 0x08, 0x0f}).wait(9000).end();
    CHECK_EQ(crossings(play(same, 9000, 64).l), ssg6);
}

// YM2413: тон своим тембром и ритм идут одним выходом в обе стороны;
// громкость чипа в файле - 0x400 (основная 0x200, вес в сумме - половина).
void test_vgm_ym2413() {
    std::printf("test_vgm_ym2413\n");
    File f;
    f.put32(0x10, 3579545);
    // Свой тембр: несущая - синус в полный уровень, модулятор заглушен.
    f.cmd({0x51, 0x00, 0x21}).cmd({0x51, 0x01, 0x21}).cmd({0x51, 0x02, 0x3f}).cmd({0x51, 0x03, 0x00});
    f.cmd({0x51, 0x04, 0xf0}).cmd({0x51, 0x05, 0xf0}).cmd({0x51, 0x06, 0x0f}).cmd({0x51, 0x07, 0x0f});
    f.cmd({0x51, 0x10, 0xac}).cmd({0x51, 0x30, 0x00}).cmd({0x51, 0x20, 0x18});
    f.wait(4000).cmd({0x51, 0x20, 0x08}).cmd({0x51, 0x30, 0x0f}).wait(2000);
    // Ритм: бочка.
    f.cmd({0x51, 0x16, 0x20}).cmd({0x51, 0x26, 0x05}).cmd({0x51, 0x36, 0x00}).cmd({0x51, 0x0e, 0x30}).wait(3000).end();
    Info info;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Ym2413 && info.chips[0].cmd == 0x51 && info.chips[0].volume == 0x400 && info.unsupported == nullptr);
    const Sound a = play(f, 9000, 64), b = play(f, 9000, 333);
    CHECK(a.l == a.r && a.l == b.l);
    // Тон 261 Гц: 11-12 периодов на отсчётах 2000-4000, размах - больше
    // половины полного уровня канала (256 единиц по 8192).
    int32_t peak     = 0;
    uint32_t periods = 0;
    for (size_t i = 2000; i < 4000; i++) {
        peak     = std::max(peak, std::abs(a.l[i]));
        periods += a.l[i - 1] < 0 && a.l[i] >= 0 ? 1u : 0u;
    }
    CHECK(peak > 128 * 8192 && peak <= 256 * 8192);
    CHECK(periods >= 11 && periods <= 13);
    CHECK(!silent(a.l, 6100, 9000));
}

// SCC: волна из 32 отсчётов на частоте вход / (16 * (период + 1)); канал 5
// через порт волн SCC играет волну канала 4, через порт SCC+ - свою; канал
// с периодом меньше 9 стоит.
void test_vgm_scc() {
    std::printf("test_vgm_scc\n");
    // Меандр в канал ch (0-4): порт волн port, период 253 - 440 Гц.
    auto tone = [](int port, int wave, int ch) {
        File f;
        f.put32(0x9c, 1789772);
        for (int i = 0; i < 32; i++) {
            f.cmd({0xd2, port, 32 * wave + i, i < 16 ? 0x7f : 0x80});
        }
        f.cmd({0xd2, 1, 2 * ch, 0xfd}).cmd({0xd2, 1, 2 * ch + 1, 0x00}).cmd({0xd2, 2, ch, 0x0f}).cmd({0xd2, 3, 0, 1 << ch});
        f.wait(9000).end();
        return f;
    };
    const File f = tone(0, 0, 0);
    Info info;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Scc && info.chips[0].clock == 1789772 && info.unsupported == nullptr);
    const Sound a = play(f, 9000, 64), b = play(f, 9000, 501);
    CHECK(a.l == a.r && a.l == b.l);
    // 440 Гц: 79-81 период на отсчётах 1000-9000; размах - волна 127 на
    // громкость 15 при громкости чипа 0x280 (вес единицы 819 * 0x280 / 256).
    int32_t top = 0, bottom = 0;
    uint32_t periods = 0;
    for (size_t i = 1000; i < 9000; i++) {
        top      = std::max(top, a.l[i]);
        bottom   = std::min(bottom, a.l[i]);
        periods += a.l[i - 1] < 0 && a.l[i] >= 0 ? 1u : 0u;
    }
    const int32_t unit = 819 * 0x280 / 256;
    CHECK(periods >= 79 && periods <= 81);
    CHECK(top == 127 * 15 * unit && bottom == -128 * 15 * unit);
    // Канал 5: волна 4 порта SCC звучит, своя волна есть только у SCC+.
    CHECK(!silent(play(tone(0, 3, 4), 2000, 64).l, 100, 2000));
    CHECK(!silent(play(tone(4, 4, 4), 2000, 64).l, 100, 2000));
    CHECK(silent(play(tone(4, 3, 4), 2000, 64).l, 0, 2000));
    // Период 8 - канал стоит.
    File halt = tone(0, 0, 0);
    halt.b.pop_back();
    halt.cmd({0xd2, 1, 0, 0x08}).wait(2000).end();
    CHECK(silent(play(halt, 11000, 64).l, 9100, 11000));
}

// Y8950: FM как у YM3526 и ADPCM-B из образа памяти в блоке данных журнала.
void test_vgm_y8950() {
    std::printf("test_vgm_y8950\n");
    // Образ: 1024 байта с адреса 0, в полубайтах - шаги вверх и вниз.
    auto image = [](File& f) {
        f.cmd({0x67, 0x66, 0x88, 8, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0});
        for (int i = 0; i < 1024; i++) {
            f.cmd({(i / 8) % 2 != 0 ? 0xcc : 0x44});
        }
    };
    // ADPCM-B: сброс, адреса 0..0xff, шаг delta-N 0x4000, полный уровень,
    // пуск из памяти.
    auto adpcm = [](File& f) {
        f.cmd({0x5c, 0x07, 0x01}).cmd({0x5c, 0x07, 0x00}).cmd({0x5c, 0x08, 0x00});
        f.cmd({0x5c, 0x09, 0x00}).cmd({0x5c, 0x0a, 0x00}).cmd({0x5c, 0x0b, 0xff}).cmd({0x5c, 0x0c, 0x00});
        f.cmd({0x5c, 0x10, 0x00}).cmd({0x5c, 0x11, 0x40}).cmd({0x5c, 0x12, 0xff}).cmd({0x5c, 0x07, 0xa0});
    };
    File f;
    f.put32(0x58, 3579545);
    image(f);
    adpcm(f);
    f.wait(3000).end();
    Info info;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
    CHECK(info.count == 1 && info.chips[0].kind == Chip::Y8950 && info.chips[0].cmd == 0x5c && info.unsupported == nullptr);
    const Sound a = play(f, 3000, 64), b = play(f, 3000, 333);
    CHECK(!silent(a.l, 100, 3000) && a.l == a.r && a.l == b.l);
    // Без образа память читается нулями: шаги только вверх, звук другой.
    File bare;
    bare.put32(0x58, 3579545);
    adpcm(bare);
    bare.wait(3000).end();
    CHECK(play(bare, 3000, 64).l != a.l);
    // FM: нота на канале 1, форм волны у чипа нет.
    File fm;
    fm.put32(0x58, 3579545);
    fm.cmd({0x5c, 0x20, 0x21}).cmd({0x5c, 0x23, 0x21}).cmd({0x5c, 0x40, 0x3f}).cmd({0x5c, 0x43, 0x00});
    fm.cmd({0x5c, 0x60, 0xf0}).cmd({0x5c, 0x63, 0xf0}).cmd({0x5c, 0x80, 0x0f}).cmd({0x5c, 0x83, 0x0f});
    fm.cmd({0x5c, 0xa0, 0x98}).cmd({0x5c, 0xb0, 0x31}).wait(2000).end();
    const Sound tone = play(fm, 2000, 64);
    CHECK(!silent(tone.l, 100, 2000) && tone.l == tone.r);
    File waved = fm;
    waved.b.pop_back();
    waved.b.resize(waved.b.size() - 3); // без ожидания и конца
    waved.cmd({0x5c, 0x01, 0x20}).cmd({0x5c, 0xe3, 0x01}).wait(2000).end();
    CHECK(play(waved, 4000, 64).l == play(fm, 4000, 64).l);
}

// Повторная запись того же значения в регистр FM звука не меняет и батч не
// дробит; старший байт частоты действует с записью младшего, даже если
// младший повторяет прежнее значение.
void test_vgm_repeated_writes() {
    std::printf("test_vgm_repeated_writes\n");
    // Нота YM2612 на канале 1; noise - на каждом 4-м отсчёте повторы тембра,
    // частоты, клавиши и запись таймера.
    auto song = [](bool noise) {
        File f;
        f.put32(0x2c, 7670453u | 0x80000000u);
        f.cmd({0x52, 0xb0, 0x07}).cmd({0x52, 0xb4, 0xc0});
        for (int op = 0; op < 4; op++) {
            const int r = 4 * op;
            f.cmd({0x52, 0x30 + r, 0x01}).cmd({0x52, 0x40 + r, op == 3 ? 0x00 : 0x7f}).cmd({0x52, 0x50 + r, 0x1f});
            f.cmd({0x52, 0x60 + r, 0x00}).cmd({0x52, 0x70 + r, 0x00}).cmd({0x52, 0x80 + r, 0x0f});
        }
        f.cmd({0x52, 0xa4, 0x22}).cmd({0x52, 0xa0, 0x69}).cmd({0x52, 0x28, 0xf0});
        for (int i = 0; i < 500; i++) {
            if (noise) {
                f.cmd({0x52, 0x3c, 0x01}).cmd({0x52, 0x4c, 0x00}).cmd({0x52, 0xb4, 0xc0}).cmd({0x52, 0xa4, 0x22}).cmd({0x52, 0xa0, 0x69});
                f.cmd({0x52, 0x28, 0xf0}).cmd({0x52, 0x27, 0x15 + (i & 1)}).cmd({0x52, 0x24, i & 0xff});
            }
            f.wait(4);
        }
        f.end();
        return f;
    };
    const File plain = song(false), noisy = song(true);
    CHECK(play(plain, 2000, 64).l == play(noisy, 2000, 64).l);
    Info info;
    CHECK(player::vgm::parse(noisy.b.data(), static_cast<uint32_t>(noisy.b.size()), info));
    std::vector<uint8_t> mem(Player::memory_need(info));
    Player p;
    CHECK(p.open(noisy.b.data(), static_cast<uint32_t>(noisy.b.size()), mem.data(), mem.size(), 1));
    std::vector<int32_t> l(2048), r(2048);
    p.render_add(l.data(), r.data(), 2048);
    CHECK(p.fm_pieces() <= 2048 / 64 + 2);

    // Защёлка: новый старший байт (октава выше), младший тот же - тон вдвое
    // выше; старший байт канала 2 каналу 1 не достаётся без записи младшего.
    auto crossings = [](const std::vector<int32_t>& v, size_t from, size_t to) {
        uint32_t count = 0;
        for (size_t i = from + 1; i < to; i++) {
            count += v[i - 1] < 0 && v[i] >= 0 ? 1u : 0u;
        }
        return count;
    };
    File up = song(false);
    up.b.pop_back();
    up.cmd({0x52, 0xa4, 0x2a}).cmd({0x52, 0xa0, 0x69}).wait(4000);
    up.cmd({0x52, 0xa5, 0x32}).wait(4000).end();
    const Sound s      = play(up, 10000, 64);
    const uint32_t low = crossings(s.l, 200, 2000), high = crossings(s.l, 2200, 4000), kept = crossings(s.l, 6200, 8000);
    CHECK(low >= 10 && high >= 2 * low - 1 && high <= 2 * low + 1);
    CHECK(kept + 1 >= high && kept <= high + 1);
}

// Голоса для телеметрии: несущий громче -48 дБ, канал PSG с уровнем.
void test_vgm_heard() {
    std::printf("test_vgm_heard\n");
    auto heard_after = [](const File& f, uint32_t frames) {
        Info info;
        CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
        std::vector<uint8_t> mem(Player::memory_need(info));
        Player p;
        CHECK(p.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), 1));
        std::vector<int32_t> l(frames), r(frames);
        p.render_add(l.data(), r.data(), frames);
        return p.heard();
    };
    // OPL3: модулятор молчит, несущий в полный уровень; после снятия
    // клавиши с быстрым затуханием - тишина.
    File a;
    a.put32(0x5c, 14318180);
    opl3_note(a, 0x30);
    a.wait(1000);
    CHECK(heard_after(a, 1000) == 1);
    a.cmd({0x5e, 0xb0, 0x11}).wait(4000).end();
    CHECK(heard_after(a, 5000) == 0);
    // YM2612, схема 7: все операторы несущие, громкий только четвёртый.
    // Два канала - два голоса; ЦАП подменяет канал 6 и считается голосом.
    File b;
    b.put32(0x2c, 7670453);
    for (int ch = 0; ch < 2; ch++) {
        b.cmd({0x52, 0xb0 + ch, 0x07}).cmd({0x52, 0xb4 + ch, 0xc0});
        for (int op = 0; op < 4; op++) {
            const int r = 4 * op + ch;
            b.cmd({0x52, 0x30 + r, 0x01}).cmd({0x52, 0x40 + r, op == 3 ? 0x00 : 0x7f}).cmd({0x52, 0x50 + r, 0x1f});
            b.cmd({0x52, 0x60 + r, 0x00}).cmd({0x52, 0x70 + r, 0x00}).cmd({0x52, 0x80 + r, 0x0f});
        }
        b.cmd({0x52, 0xa4 + ch, 0x22}).cmd({0x52, 0xa0 + ch, 0x69}).cmd({0x52, 0x28, 0xf0 + ch});
    }
    b.wait(1000);
    CHECK(heard_after(b, 1000) == 2);
    b.cmd({0x52, 0x2b, 0x80}).wait(100).end();
    CHECK(heard_after(b, 1100) == 3);
    // AY: два канала с уровнем, третий молчит.
    File c;
    c.put32(0x74, 1773400);
    c.cmd({0xa0, 0x07, 0x38}).cmd({0xa0, 0x08, 0x0f}).cmd({0xa0, 0x09, 0x04}).wait(500).end();
    CHECK(heard_after(c, 500) == 2);
}

// Метаданные трека: формат, строка на модель чипа (пара SN - одна строка со счётом 2,
// YM2612 с разрядом 31 - YM3438), теги GD3 по-английски в CP866 блоками по
// 16, бит 7 на последнем кадре. Трек движка - чип MUSICA.
void test_vgm_track_meta() {
    std::printf("test_vgm_track_meta\n");
    using namespace player::protocol;
    // Свёртка кадров с 0 по 79 вместе с ответом "кадр есть": разбивка на
    // строки и блоки сверяется целиком, байт в байт.
    auto digest = [](TrackMeta& meta) {
        uint32_t h = 2166136261u;
        auto mix   = [&h](uint32_t b) { h = (h ^ b) * 16777619u; };
        for (uint8_t k = 0; k < 80; ++k) {
            uint8_t out[HostProtocol::kTrackMetaBytes] = {};
            const bool ok                              = track_meta_frame(&meta, k, out);
            mix(ok ? 1u : 0u);
            for (const uint8_t b : out)
                mix(ok ? b : 0u);
        }
        std::printf("  track meta digest %08x\n", static_cast<unsigned>(h));
        return h;
    };
    File f;
    f.put32(0x0c, 3579545u | 0x40000000u);
    f.put32(0x2c, 7670453u | 0x80000000u);
    f.cmd({0x50, 0x9f}).cmd({0x30, 0x9f}).cmd({0x52, 0x28, 0x00}).wait(100).end();
    f.put32(0x14, static_cast<uint32_t>(f.b.size()) - 0x14);
    const size_t tag = f.b.size();
    f.cmd({'G', 'd', '3', ' ', 0, 1, 0, 0, 0, 0, 0, 0});
    auto str = [&](std::vector<int> units) {
        for (const int u : units)
            f.cmd({u & 0xff, u >> 8});
        f.cmd({0, 0});
    };
    std::vector<int> author;
    for (int i = 0; i < 32; i++)
        author.push_back(0x410 + i);
    for (int i = 0; i < 8; i++)
        author.push_back(0x430 + i);
    str({'T', 'i', 't', 'l', 'e'});
    str({0x3042});
    str({'G', 'a', 'm', 'e'});
    str({});
    str({});
    str({});
    str(author);
    str({0x3042});
    str({'1', '9', '9', '1'});
    str({});
    str({});
    f.put32(tag + 8, static_cast<uint32_t>(f.b.size() - tag - 12));

    Info vi;
    CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), vi));
    std::vector<uint8_t> mem(Player::memory_need(vi));
    Player p;
    CHECK(p.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), 1));
    TrackMeta t;
    std::memcpy(t.format, "VGM", 3);
    t.klass   = kClassChip;
    t.version = 0x171;
    t.vgm     = &p;
    CHECK_EQ(digest(t), 0x1cb84184u);

    std::vector<std::vector<uint8_t>> frames;
    for (uint8_t k = 0; k < 64; ++k) {
        uint8_t out[HostProtocol::kTrackMetaBytes];
        if (!track_meta_frame(&t, k, out)) break;
        frames.emplace_back(out, out + sizeof(out));
        if ((out[0] & HostProtocol::kTrackMetaLast) != 0) break;
    }
    // Формат, два чипа, название, автор тремя блоками, игра, дата.
    CHECK_EQ(frames.size(), size_t{9});
    if (frames.size() != 9) return;
    const uint8_t format[] = {kMetaFormat, 0xf6, 'V', 'G', 'M', 0, kClassChip, 0x71, 0x01};
    CHECK(std::memcmp(frames[0].data(), format, sizeof(format)) == 0);
    bool sn = false, ym = false;
    for (int i = 1; i <= 2; i++) {
        const uint8_t* c = frames[static_cast<size_t>(i)].data();
        CHECK_EQ(c[0], static_cast<uint8_t>(kMetaChip + i - 1));
        CHECK_EQ(c[1], 0xffu);
        const uint8_t sn_clock[] = {0x45, 0x95, 0x57, 0x03}, ym_clock[] = {0x53, 0x04, 0x67, 0x07};
        if (std::strcmp(reinterpret_cast<const char*>(c + 2), "SN76489") == 0) sn = std::memcmp(c + 13, sn_clock, 4) == 0 && c[17] == 2;
        if (std::strcmp(reinterpret_cast<const char*>(c + 2), "YM3438") == 0) ym = std::memcmp(c + 13, ym_clock, 4) == 0 && c[17] == 1;
    }
    CHECK(sn && ym);
    CHECK_EQ(frames[3][0], 0x10u);
    CHECK_EQ(frames[3][1], 0xf4u);
    CHECK(std::memcmp(frames[3].data() + 2, "Title", 6) == 0);
    CHECK_EQ(frames[4][0], 0x11u);
    CHECK_EQ(frames[4][1], 0x0fu);
    CHECK_EQ(frames[4][2], 0x80u);
    CHECK_EQ(frames[5][1], 0x1fu);
    CHECK_EQ(frames[5][17], 0x9fu);
    CHECK_EQ(frames[6][1], 0xf7u);
    CHECK_EQ(frames[6][2], 0xa0u);
    CHECK_EQ(frames[6][9], 0xa7u);
    CHECK_EQ(frames[6][10], 0u);
    CHECK_EQ(frames[7][0], 0x12u);
    CHECK_EQ(frames[8][0], 0x94u);
    CHECK(std::memcmp(frames[8].data() + 2, "1991", 5) == 0);

    // Трек движка: формат и чип MUSICA на 44100 Гц, он же последний.
    TrackMeta m;
    std::memcpy(m.format, "XM", 2);
    m.klass   = kClassTracker;
    m.version = 0x0104;
    CHECK_EQ(digest(m), 0x09ade2dau);
    uint8_t out[HostProtocol::kTrackMetaBytes];
    CHECK(track_meta_frame(&m, 0, out) && out[0] == kMetaFormat && out[7] == 0x04 && out[8] == 0x01);
    const uint8_t musica[] = {kMetaChip | HostProtocol::kTrackMetaLast, 0xff, 'M', 'U', 'S', 'I', 'C', 'A', 0, 0, 0, 0, 0, 0x00, 0x41, 0x04, 0x00, 1};
    CHECK(track_meta_frame(&m, 1, out) && std::memcmp(out, musica, sizeof(musica)) == 0);
    CHECK(!track_meta_frame(&m, 2, out));

    // С Song: числа, MUSICA, название без пробелов в конце, программа в два блока.
    soundsinth::model::Song s;
    std::memcpy(s.title, "Space Debris  ", 14);
    soundsinth::model::set_program(s, "Impulse Tracker", 15, 0x0214);
    CHECK(std::memcmp(s.program, "Impulse Tracker 2.14", 20) == 0);
    s.channel_count        = 8;
    s.default_speed        = 6;
    s.default_tempo        = 125;
    s.order_count          = 42;
    m.song                 = &s;
    const uint8_t params[] = {kMetaParams, 0xff, 0x08, 0, 0, 0, 0x06, 0, 0, 0, 0x25, 0x01, 0, 0, 0x42, 0, 0, 0};
    CHECK_EQ(digest(m), 0xc4b08bdbu);
    CHECK(track_meta_frame(&m, 1, out) && std::memcmp(out, params, sizeof(params)) == 0);
    CHECK(track_meta_frame(&m, 2, out) && out[0] == kMetaChip);
    CHECK(track_meta_frame(&m, 3, out) && out[0] == kMetaTagFirst && out[1] == 0xfb && std::memcmp(out + 2, "Space Debris", 12) == 0 && out[14] == 0);
    CHECK(track_meta_frame(&m, 4, out) && out[0] == kMetaProgram && out[1] == 0x0f && std::memcmp(out + 2, "Impulse Tracker ", 16) == 0);
    CHECK(track_meta_frame(&m, 5, out) && out[0] == (kMetaProgram | HostProtocol::kTrackMetaLast) && out[1] == 0xf3 && std::memcmp(out + 2, "2.14", 4) == 0);
    CHECK(!track_meta_frame(&m, 6, out));

    // .mid: три числа, без названия и программы.
    soundsinth::model::Song mid;
    mid.midi_header[0]         = 16;
    mid.midi_header[1]         = 480;
    mid.midi_header[2]         = 120;
    m.klass                    = kClassMidi;
    m.song                     = &mid;
    const uint8_t mid_params[] = {kMetaParams, 0xfb, 0x16, 0, 0, 0, 0x80, 0x04, 0, 0, 0x20, 0x01, 0, 0};
    CHECK_EQ(digest(m), 0xc7ccfc9au);
    CHECK(track_meta_frame(&m, 1, out) && std::memcmp(out, mid_params, sizeof(mid_params)) == 0);
    CHECK(track_meta_frame(&m, 2, out) && out[0] == (kMetaChip | HostProtocol::kTrackMetaLast));

    soundsinth::model::set_program(s, "Scream Tracker", 20, 0x0320);
    CHECK(std::memcmp(s.program, "Scream Tracker 3.20", 20) == 0);
    CHECK_EQ(cp866_from_cp437(0xc4), 0xc4u);
    CHECK_EQ(cp866_from_cp437(0x82), static_cast<uint8_t>('?'));

    CHECK_EQ(cp866(0x401), 0xf0u);
    CHECK_EQ(cp866(0x451), 0xf1u);
    CHECK_EQ(cp866(0x0a), static_cast<uint8_t>(' '));
    CHECK_EQ(cp866(0x3042), static_cast<uint8_t>('?'));
}

// Отпечаток выхода на случайных журналах: записи по всем регистрам каждого
// вида чипа, пары, банки отсчётов и образы ADPCM, потоки ЦАП, петля и
// перемотка. Числа сняты с плеера как есть: тест ловит любое изменение
// звука, голосов или нарезки FM.
struct Port {
    int cmd, lo, hi; // команда записи и диапазон регистров
};
struct GoldenCase {
    const char* name;
    std::vector<std::pair<uint32_t, uint32_t>> clocks; // поле частоты заголовка и значение
    std::vector<Port> ports;
    std::vector<int> blocks; // виды блоков данных
    bool dac;                // отсчёты 0x8n, 0xE0 и потоки 0x90-0x95 для YM2612
    uint64_t sound, skipped;
};

uint64_t fnv(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h = (h ^ ((v >> (8 * i)) & 0xffu)) * 0x100000001b3ull;
    }
    return h;
}

File golden_log(const GoldenCase& g) {
    uint32_t seed = 12345;
    auto rnd      = [&seed](uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<int>((seed >> 8) % n);
    };
    File f;
    for (const auto& c : g.clocks) {
        f.put32(c.first, c.second);
    }
    f.b[0x78] = 0x10; // AY: уровни YM2149
    f.b[0x7c] = 0x10; // усиление заголовка
    for (const int type : g.blocks) {
        const uint32_t n = 2000;
        if (type == 0x00) {
            f.cmd({0x67, 0x66, 0x00, static_cast<int>(n & 0xff), static_cast<int>(n >> 8), 0, 0});
        } else {
            const uint32_t len = n + 8;
            f.cmd({0x67, 0x66, type, static_cast<int>(len & 0xff), static_cast<int>(len >> 8), 0, 0, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0});
        }
        for (uint32_t i = 0; i < n; i++) {
            f.cmd({rnd(256)});
        }
    }
    for (int i = 0; i < 1600; i++) {
        if (i == 800) f.loop_here();
        const int what = rnd(100);
        if (what < 22) {
            f.wait(1u + static_cast<uint32_t>(rnd(120)));
        } else if (what < 25) {
            f.cmd({0x70 + rnd(16)});
        } else if (g.dac && what < 30) {
            f.cmd({0x80 + rnd(16)});
        } else if (g.dac && what < 31) {
            f.cmd({0xe0, rnd(256), rnd(8), 0, 0});
        } else if (g.dac && what < 33) {
            switch (rnd(6)) {
                case 0:
                    f.cmd({0x90, 0, 0x02, 0x00, 0x2a});
                    break;
                case 1:
                    f.cmd({0x91, 0, 0x00, 1 + rnd(2), rnd(4)});
                    break;
                case 2:
                    f.cmd({0x92, 0, rnd(256), rnd(256), rnd(2), 0});
                    break;
                case 3:
                    f.cmd({0x93, 0, rnd(256), rnd(8), 0, 0, (rnd(2) << 7) | (rnd(2) << 4) | rnd(4), rnd(256), rnd(4), 0, 0});
                    break;
                case 4:
                    f.cmd({0x94, rnd(4) == 0 ? 0xff : 0});
                    break;
                default:
                    f.cmd({0x95, 0, rnd(3), 0, rnd(32)});
                    break;
            }
        } else {
            const Port& p = g.ports[static_cast<size_t>(rnd(static_cast<uint32_t>(g.ports.size())))];
            const int reg = p.lo + rnd(static_cast<uint32_t>(p.hi - p.lo + 1));
            if (p.cmd == 0x50 || p.cmd == 0x30 || p.cmd == 0x4f || p.cmd == 0x3f) {
                f.cmd({p.cmd, rnd(256)});
            } else if (p.cmd == 0xd2) {
                f.cmd({p.cmd, reg >> 8, reg & 0xff, rnd(256)});
            } else {
                f.cmd({p.cmd, reg, rnd(256)});
            }
        }
    }
    return f.end();
}

void test_vgm_golden() {
    std::printf("test_vgm_golden\n");
    static uint8_t rhythm[player::vgm::kRhythmRomBytes];
    for (uint32_t i = 0; i < sizeof(rhythm); i++) {
        rhythm[i] = static_cast<uint8_t>(i * 37u + (i >> 5));
    }
    const uint32_t two = 0x40000000u, flag = 0x80000000u;
    // clang-format off
    const GoldenCase cases[] = {
        {"SN76489", {{0x0c, 3579545}}, {{0x50, 0, 0}, {0x4f, 0, 0}}, {}, false, 0xc5a793fed1b5d2d5ull, 0x584ec25c8df4ff54ull},
        {"SN76489 x2", {{0x0c, 3579545 | two}}, {{0x50, 0, 0}, {0x30, 0, 0}, {0x4f, 0, 0}, {0x3f, 0, 0}}, {}, false, 0x80b297c060e57151ull, 0xf27664a911f2a1f3ull},
        {"AY8910 x2", {{0x74, 1789773 | two}}, {{0xa0, 0x00, 0x0f}, {0xa0, 0x80, 0x8f}}, {}, false, 0x56586ee2de7568f4ull, 0x5a17504a0df406b7ull},
        {"SAA1099 x2", {{0xc8, 8000000 | two}}, {{0xbd, 0x00, 0x1f}, {0xbd, 0x80, 0x9f}}, {}, false, 0x7d0d64b191524e0aull, 0xc1301f33b53d136dull},
        {"SCC", {{0x9c, 1789773}}, {{0xd2, 0x000, 0x5ff}}, {}, false, 0x1ac9c61cfe868754ull, 0xc027a3dc88b8a043ull},
        {"YM2203 x2", {{0x44, 3993600 | two}}, {{0x55, 0x00, 0xb6}, {0x55, 0x28, 0x28}, {0x55, 0x2d, 0x2f}, {0xa5, 0x00, 0xb6}, {0xa5, 0x28, 0x28}}, {}, false, 0x8ed55c8eedf99f34ull, 0x8a797c887e63e356ull},
        {"YM2612", {{0x2c, 7670453}}, {{0x52, 0x21, 0xb6}, {0x52, 0x28, 0x28}, {0x52, 0x2a, 0x2c}, {0x53, 0x30, 0xb6}}, {0x00, 0x00, 0x00}, true, 0x432ef1a0d52a772eull, 0xb93ca75266e96cdbull},
        {"YM3438 x2", {{0x2c, 7670453 | two | flag}}, {{0x52, 0x21, 0xb6}, {0x52, 0x28, 0x28}, {0x53, 0x30, 0xb6}, {0xa2, 0x21, 0xb6}, {0xa2, 0x28, 0x28}, {0xa3, 0x30, 0xb6}}, {0x00}, true, 0x96515dd3bf22f001ull, 0x63e08ee99ce36a04ull},
        {"YM2413", {{0x10, 3579545}}, {{0x51, 0x00, 0x38}, {0x51, 0x20, 0x28}, {0x51, 0x0e, 0x0e}}, {}, false, 0xdbbdb548ecf29475ull, 0x8735055a19a93347ull},
        {"VRC7", {{0x10, 3579545 | flag}}, {{0x51, 0x00, 0x38}, {0x51, 0x20, 0x28}, {0x51, 0x0e, 0x0e}}, {}, false, 0xcfee34b5c57a4b3cull, 0x03002dc25ca42ed5ull},
        {"YM2151", {{0x30, 3579545}}, {{0x54, 0x00, 0xff}, {0x54, 0x08, 0x08}}, {}, false, 0xbd5acbab85447de7ull, 0x41ded4cb00cc7bafull},
        {"YM2610", {{0x4c, 8000000}}, {{0x58, 0x00, 0xb6}, {0x58, 0x28, 0x28}, {0x58, 0x10, 0x1c}, {0x59, 0x00, 0xb6}, {0x59, 0x00, 0x2f}}, {0x82, 0x83}, false, 0x352ea73e81867b33ull, 0x07f46c3c46a4ca08ull},
        {"YM2608", {{0x48, 7987200}}, {{0x56, 0x00, 0xb6}, {0x56, 0x28, 0x29}, {0x56, 0x10, 0x1f}, {0x57, 0x00, 0xb6}, {0x57, 0x00, 0x10}}, {0x81}, false, 0x1b40cf94b1dc0b87ull, 0xca1a47acae19d508ull},
        {"YM3812", {{0x50, 3579545}}, {{0x5a, 0x00, 0xf5}, {0x5a, 0xb0, 0xb8}, {0x5a, 0x01, 0x01}, {0x5a, 0xe0, 0xf5}}, {}, false, 0x6cccdc7af050d65eull, 0xd19694fbec93ecb5ull},
        {"YM3526", {{0x54, 3579545}}, {{0x5b, 0x00, 0xf5}, {0x5b, 0xb0, 0xb8}, {0x5b, 0x01, 0x01}, {0x5b, 0xe0, 0xf5}}, {}, false, 0x07cf32698938a8d2ull, 0xa1965671b9449d1dull},
        {"Y8950", {{0x58, 3579545}}, {{0x5c, 0x00, 0xf5}, {0x5c, 0xb0, 0xb8}, {0x5c, 0x07, 0x17}}, {0x88}, false, 0xc1aa0c9842c8482cull, 0x20c47bfe8f3e801aull},
        {"OPL3", {{0x5c, 14318180}}, {{0x5e, 0x00, 0xf5}, {0x5e, 0xb0, 0xb8}, {0x5f, 0x00, 0xf5}, {0x5f, 0xb0, 0xb8}, {0x5f, 0x04, 0x05}}, {}, false, 0xcebe216d13730ae3ull, 0x0b69fdbd089ae39aull},
        {"eight chips", {{0x0c, 3579545}, {0x2c, 7670453}, {0x30, 3579545}, {0x44, 3993600}, {0x74, 1789773}, {0xc8, 8000000}, {0x5c, 14318180}, {0x9c, 1789773}},
         {{0x50, 0, 0}, {0x52, 0x21, 0xb6}, {0x54, 0x00, 0xff}, {0x55, 0x00, 0xb6}, {0xa0, 0x00, 0x0f}, {0xbd, 0x00, 0x1f}, {0x5e, 0x00, 0xf5}, {0xd2, 0x000, 0x5ff}},
         {0x00}, true, 0xec46ea519612738full, 0xbeb18800b6231047ull},
    };
    // clang-format on
    player::vgm::set_rhythm_rom(rhythm);
    for (const GoldenCase& g : cases) {
        const File f = golden_log(g);
        Info info;
        CHECK(player::vgm::parse(f.b.data(), static_cast<uint32_t>(f.b.size()), info));
        std::vector<uint8_t> mem(Player::memory_need(info));
        // Петля дважды кусками разной длины; голоса - после каждого куска.
        auto run = [&](uint32_t skip) {
            Player p;
            CHECK(p.open(f.b.data(), static_cast<uint32_t>(f.b.size()), mem.data(), mem.size(), 2));
            uint64_t h = 0xcbf29ce484222325ull;
            std::vector<int32_t> l(1000), r(1000);
            for (uint32_t at = 0, k = 0; at < 120000; k++) {
                if (skip != 0 && k == 3) p.skip(skip);
                const uint32_t n = 1 + (k * 397u) % 1000u;
                std::fill(l.begin(), l.end(), 0);
                std::fill(r.begin(), r.end(), 0);
                h = fnv(h, p.render_add(l.data(), r.data(), n) ? 1u : 0u);
                for (uint32_t i = 0; i < n; i++) {
                    h = fnv(fnv(h, static_cast<uint32_t>(l[i])), static_cast<uint32_t>(r[i]));
                }
                h   = fnv(h, p.heard());
                at += n;
            }
            return fnv(h, p.fm_pieces());
        };
        const uint64_t sound = run(0), skipped = run(30011);
        if (sound != g.sound || skipped != g.skipped) {
            std::printf("  %s: 0x%016llxull, 0x%016llxull\n", g.name, static_cast<unsigned long long>(sound), static_cast<unsigned long long>(skipped));
        }
        CHECK(sound == g.sound);
        CHECK(skipped == g.skipped);
    }
    player::vgm::set_rhythm_rom(nullptr);
}

void run_vgm_tests() {
    test_vgm_golden();
    test_vgm_track_meta();
    test_vgm_heard();
    test_vgm_header();
    test_vgm_chunks();
    test_vgm_opl3_outputs();
    test_vgm_stereo_and_pairs();
    test_vgm_volumes();
    test_vgm_ym2612();
    test_vgm_old_version_fm();
    test_vgm_ym2151();
    test_vgm_ym2610();
    test_vgm_ym2608();
    test_vgm_unused_chips();
    test_vgm_loop_parameters();
    test_vgm_skip();
    test_vgm_gd3();
    test_vgm_vrc7();
    test_vgm_ym2203_divider();
    test_vgm_ym2413();
    test_vgm_scc();
    test_vgm_y8950();
    test_vgm_repeated_writes();
    test_vgm_loop_and_end();
    test_vgm_restart_in_one_sample();
    test_vgm_feedback_at_chip_rate();
    test_vgm_track();
}
