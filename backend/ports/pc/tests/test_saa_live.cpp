// SPDX-License-Identifier: MIT
// Живой SAA1099 целиком: записи со штампом шины через кольцо отвода в
// источник микшера. Такт чипа по управлению MultiSound, высота тона на
// выходе, сброс машины.

#include "testing.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/memory/track_memory.h"
#include "player/live/live_clock.h"
#include "player/live/live_tap.h"
#include "player/live/saa_live.h"
#include "player/shared_state.h"

extern char g_test_log[8192];

namespace {

using player::live::TapPort;

uint64_t g_bus_clock = 0;
uint64_t test_clock() {
    return g_bus_clock;
}

constexpr uint64_t kBusHz = 2000000;
constexpr uint32_t kRate  = 44100;

struct Ev {
    uint64_t bus;
    TapPort port;
    uint8_t data;
};

// Запись регистра: номер в #1FF, значение в #FF.
void reg(std::vector<Ev>& v, uint64_t bus, uint8_t r, uint8_t value) {
    v.push_back({bus, TapPort::SaaAddr, r});
    v.push_back({bus + 24u, TapPort::SaaData, value});
}

// Тон канала 0: октава 5, частота 255 - полупериод (511 - 255) << (7 - 5) =
// 1024 такта ядра по 4 МГц, 1953.125 Гц; громкость 15 слева и справа. Без
// записи частоты (freq = false) после сброса она 0: 978.5 Гц.
void tone(std::vector<Ev>& v, uint64_t bus, bool freq = true) {
    reg(v, bus, 0x00, 0xff);
    if (freq) reg(v, bus + 100, 0x08, 255);
    reg(v, bus + 200, 0x10, 0x05);
    reg(v, bus + 300, 0x14, 0x01);
    reg(v, bus + 400, 0x1c, 0x01);
}

// Рендер по 256 отсчётов с часов start; записи кладутся в кольцо, когда их
// время настало. Итог - левая сторона, right - правая.
std::vector<int32_t> run(const std::vector<Ev>& ev, uint64_t start, uint32_t frames, std::vector<int32_t>* right = nullptr) {
    soundsinth::mixbus::SoundSource* src = player::live::saa_live_source();
    std::vector<int32_t> l(frames, 0), r(frames, 0);
    size_t e = 0;
    for (uint32_t done = 0; done < frames; done += 256u) {
        const uint64_t now = start + static_cast<uint64_t>(done) * kBusHz / kRate;
        g_bus_clock        = now;
        while (e < ev.size() && ev[e].bus <= now) {
            player::live::tap_push(static_cast<uint32_t>(ev[e].bus), ev[e].port, ev[e].data);
            ++e;
        }
        const uint32_t n = frames - done < 256u ? frames - done : 256u;
        src->render_add(src->self, &l[done], &r[done], n);
    }
    if (right != nullptr) *right = r;
    return l;
}

uint32_t rising(const std::vector<int32_t>& v, uint32_t from, uint32_t to) {
    int32_t lo = v[from], hi = v[from];
    for (uint32_t i = from; i < to; ++i) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    const int32_t mid = (lo + hi) / 2;
    uint32_t up       = 0;
    for (uint32_t i = from + 1; i < to; ++i)
        if (v[i - 1] < mid && v[i] >= mid) ++up;
    return up;
}

bool silent(const std::vector<int32_t>& v) {
    for (int32_t s : v)
        if (s != 0) return false;
    return true;
}

bool stats_say(const char* want) {
    g_test_log[0] = 0;
    player::live::saa_live_log_stats();
    return std::string(g_test_log).find(want) != std::string::npos;
}

// Регистры записаны, такта нет - тишина; #F7 в #FFFD даёт такт, и тон
// звучит своей высотой с обеих сторон; #FF снимает такт.
void saa_waits_for_the_clock() {
    player::live::saa_live_configure(true);
    constexpr uint64_t kBase = 1000000;
    std::vector<Ev> ev;
    tone(ev, kBase + 1000);
    CHECK(silent(run(ev, kBase, kRate / 5u)));
    CHECK(stats_say("clock off"));
    CHECK(stats_say("writes 10,"));

    constexpr uint64_t kOn = kBase + kBusHz;
    std::vector<int32_t> r;
    const std::vector<int32_t> l = run({{kOn + 1000, TapPort::AySelect, 0xf7}}, kOn, 2u * kRate, &r);
    const uint32_t up            = rising(l, kRate / 2, kRate * 3 / 2);
    std::printf("  live SAA tone: %u rising crossings in 1 s (want 1952-1954)\n", up);
    CHECK(up >= 1952 && up <= 1954);
    CHECK(l == r);
    CHECK(stats_say("clock on"));
    CHECK(stats_say("late 0,"));

    constexpr uint64_t kOff      = kOn + 4u * kBusHz;
    const std::vector<int32_t> q = run({{kOff + 1000, TapPort::AySelect, 0xff}}, kOff, kRate);
    CHECK(silent(std::vector<int32_t>(q.begin() + kRate / 2, q.end())));
}

// Сброс машины снимает такт и обнуляет регистры: чип молчит и после #F7,
// пока программа не запишет регистры заново.
void saa_machine_reset_stops_the_clock() {
    player::live::saa_live_configure(true);
    constexpr uint64_t kBase = 20000000;
    std::vector<Ev> ev;
    tone(ev, kBase + 1000);
    ev.push_back({kBase + 2000, TapPort::AySelect, 0xf6});
    CHECK(!silent(run(ev, kBase, kRate / 2u)));

    player::live::saa_live_machine_reset();
    constexpr uint64_t kAfter = kBase + 2u * kBusHz;
    CHECK(silent(run({}, kAfter, kRate / 2u)));
    CHECK(stats_say("resets 1"));

    constexpr uint64_t kAgain = kAfter + 2u * kBusHz;
    CHECK(silent(run({{kAgain + 1000, TapPort::AySelect, 0xf7}}, kAgain, kRate)));

    constexpr uint64_t kTone = kAgain + 2u * kBusHz;
    std::vector<Ev> again;
    tone(again, kTone + 1000);
    const std::vector<int32_t> l = run(again, kTone, 2u * kRate);
    const uint32_t up            = rising(l, kRate / 2, kRate * 3 / 2);
    CHECK(up >= 1952 && up <= 1954);
}

// Запись, ждущая в кольце своей задержки, сбросом выброшена: частота 255
// из неё не доходит, тон после сброса - с частотой 0. Два запроса сброса
// до рендера - один сброс.
void saa_machine_reset_drops_waiting_writes() {
    player::live::saa_live_configure(true);
    constexpr uint64_t kBase = 30000000;
    std::vector<Ev> ev;
    tone(ev, kBase + 1000);
    ev.push_back({kBase + 2000, TapPort::AySelect, 0xf6});
    CHECK(!silent(run(ev, kBase, kRate / 2u)));
    g_test_log[0] = 0;
    player::live::saa_live_log_stats();
    const std::string before = g_test_log;
    const size_t at          = before.find("resets ");
    CHECK(at != std::string::npos);
    const unsigned resets = at != std::string::npos ? static_cast<unsigned>(std::stoul(before.substr(at + 7))) : 0u;

    // Частота 255 (регистр 0x08) - в кольце, её такт ещё не настал.
    constexpr uint64_t kNow = kBase + kBusHz / 2u;
    player::live::tap_push(static_cast<uint32_t>(kNow), TapPort::SaaAddr, 0x08);
    player::live::tap_push(static_cast<uint32_t>(kNow + 24u), TapPort::SaaData, 255);
    player::live::saa_live_machine_reset();
    player::live::saa_live_machine_reset();
    constexpr uint64_t kAgain = kBase + 2u * kBusHz;
    std::vector<Ev> again;
    tone(again, kAgain + 1000, false);
    again.push_back({kAgain + 2000, TapPort::AySelect, 0xf7});
    const std::vector<int32_t> l = run(again, kAgain, 2u * kRate);
    const uint32_t up            = rising(l, kRate / 2, kRate * 3 / 2);
    std::printf("  live SAA tone after reset: %u rising crossings in 1 s (want 978-979)\n", up);
    CHECK(up >= 977 && up <= 980);
    CHECK(stats_say(("resets " + std::to_string(resets + 1u)).c_str()));
}

// SAA1099=0: источник молчит и записей не разбирает.
void saa_off_stays_silent() {
    player::live::saa_live_configure(false);
    constexpr uint64_t kBase = 40000000;
    std::vector<Ev> ev;
    tone(ev, kBase + 1000);
    ev.push_back({kBase + 2000, TapPort::AySelect, 0xf7});
    CHECK(silent(run(ev, kBase, kRate / 2u)));
}

} // namespace

void run_saa_live_tests() {
    std::printf("player: live SAA1099 from the bus\n");
    // Чип берёт память в начале арены трека: арена обязана быть, и без
    // метаданных трека.
    if (shared::g_track_memory.resident.base == nullptr) soundsinth::memory::track_memory_create(shared::g_track_memory);
    soundsinth::memory::track_memory_reset_for_new_track(shared::g_track_memory);
    player::live::live_clock_set(&test_clock);
    saa_waits_for_the_clock();
    saa_machine_reset_stops_the_clock();
    saa_machine_reset_drops_waiting_writes();
    saa_off_stays_silent();
    player::live::live_clock_set(nullptr);
}
