// SPDX-License-Identifier: MIT
// Живой AY целиком: записи со штампом шины через кольцо в источник
// микшера. Высота тона на выходе, ровность моментов записей при рендере,
// забегающем вперёд, запись моложе часов рендера, TurboSound.

#include "testing.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/live_midi/ay_midi.h"
#include "core/memory/track_memory.h"
#include "player/live/ay_live.h"
#include "player/live/ay_port.h"
#include "player/live/live_clock.h"
#include "player/live/live_tap.h"
#include "player/shared_state.h"

extern char g_test_log[8192];
extern uint32_t g_port_page_sets[256];
extern uint32_t g_port_clears[256];

namespace {

uint64_t g_pc_bus_clock = 0;

// Запись в отвод, как кладёт обработчик шины.
void push(uint32_t stamp, uint16_t write) {
    const bool sel = (write & soundsinth::midi_in::kAyWriteSelect) != 0;
    player::live::tap_push(stamp, sel ? player::live::TapPort::AySelect : player::live::TapPort::AyData, static_cast<uint8_t>(write));
}

uint64_t test_clock() {
    return g_pc_bus_clock;
}

constexpr uint64_t kBusHz = 2000000;
constexpr uint32_t kRate  = 44100;
constexpr uint64_t kBase  = 1000000; // полмикросекунды шины на старте

struct Ev {
    uint64_t bus;
    uint16_t write;
};

void reg(std::vector<Ev>& v, uint64_t bus, uint8_t r, uint8_t value) {
    v.push_back({bus, static_cast<uint16_t>(soundsinth::midi_in::kAyWriteSelect | r)});
    v.push_back({bus + 24u, value});
}

// Рендер по 256 отсчётов; часы шины идут вровень с выводом, но каждый
// седьмой буфер рендер забегает на ahead буферов вперёд. Записи кладутся в
// кольцо, когда их время настало.
std::vector<int32_t> run(const std::vector<Ev>& ev, uint32_t frames, uint32_t ahead) {
    soundsinth::mixbus::SoundSource* src = player::live::ay_live_source();
    std::vector<int32_t> l(frames, 0), r(frames, 0);
    size_t e = 0;
    for (uint32_t done = 0; done < frames; done += 256u) {
        uint64_t now = kBase + static_cast<uint64_t>(done) * kBusHz / kRate;
        if (ahead != 0 && (done / 256u) % 7u == 0) now -= static_cast<uint64_t>(ahead) * 256u * kBusHz / kRate;
        g_pc_bus_clock = now;
        while (e < ev.size() && ev[e].bus <= now) {
            push(static_cast<uint32_t>(ev[e].bus), ev[e].write);
            ++e;
        }
        const uint32_t n = frames - done < 256u ? frames - done : 256u;
        src->render_add(src->self, &l[done], &r[done], n);
    }
    return l;
}

bool stats_say(const char* want) {
    g_test_log[0] = 0;
    player::live::ay_live_log_stats();
    return std::string(g_test_log).find(want) != std::string::npos;
}

// Тон TP = 100 на 1.75 МГц - 1093.75 Гц на выходе микшера.
void live_tone_pitch() {
    player::live::ay_live_configure(1, 1750000, 0);
    std::vector<Ev> ev;
    reg(ev, kBase + 1000, 0, 100);
    reg(ev, kBase + 1100, 7, 0x3e);
    reg(ev, kBase + 1200, 8, 15);
    const std::vector<int32_t> l = run(ev, 2u * kRate, 3);
    int32_t lo = l[kRate / 2], hi = l[kRate / 2];
    for (uint32_t i = kRate / 2; i < kRate * 3 / 2; ++i) {
        if (l[i] < lo) lo = l[i];
        if (l[i] > hi) hi = l[i];
    }
    const int32_t mid = (lo + hi) / 2;
    uint32_t up       = 0;
    for (uint32_t i = kRate / 2 + 1; i <= kRate * 3 / 2; ++i)
        if (l[i - 1] < mid && l[i] >= mid) ++up;
    std::printf("  live tone TP=100: %u rising crossings in 1 s (want 1093-1094)\n", up);
    CHECK(up >= 1093 && up <= 1094);
    CHECK(stats_say("late 0,"));
}

// Громкость A меняется каждые 20 мс (тон и шум выключены): на выходе
// перепады идут ровно через 882 отсчёта, хотя рендер забегает вперёд, и
// ни одна запись не опоздала.
void live_writes_keep_their_moments() {
    player::live::ay_live_configure(1, 1750000, 0);
    std::vector<Ev> ev;
    reg(ev, kBase + 500, 7, 0x3f);
    constexpr uint64_t kFrame = kBusHz / 50u;
    for (uint32_t f = 0; f < 100; ++f) {
        reg(ev, kBase + 4000 + f * kFrame, 8, (f & 1u) ? 0 : 15);
    }
    const std::vector<int32_t> l = run(ev, 3u * kRate, 4);
    std::vector<uint32_t> edges;
    for (uint32_t i = 1; i < l.size(); ++i) {
        const int32_t half = 8192 * 256;
        if ((l[i - 1] < half) != (l[i] < half)) edges.push_back(i);
    }
    uint32_t off = 0;
    for (size_t i = 1; i < edges.size(); ++i) {
        const uint32_t d = edges[i] - edges[i - 1];
        if (d < 881u || d > 883u) ++off;
    }
    std::printf("  live frame writes: %zu edges, %u spaced off 882+-1\n", edges.size(), off);
    CHECK(edges.size() >= 99u);
    CHECK_EQ(off, 0u);
    CHECK(stats_say("late 0,"));
}

// Запись положена после того, как рендер прочитал часы: штамп моложе
// часов. Она не опоздавшая, а будущая.
void live_stamp_younger_than_the_clock() {
    player::live::ay_live_configure(1, 1750000, 0);
    soundsinth::mixbus::SoundSource* src = player::live::ay_live_source();
    std::vector<int32_t> l(256), r(256);
    g_pc_bus_clock = kBase;
    src->render_add(src->self, l.data(), r.data(), 256);
    push(static_cast<uint32_t>(kBase + 300), soundsinth::midi_in::kAyWriteSelect | 8u);
    push(static_cast<uint32_t>(kBase + 320), 15);
    g_pc_bus_clock = kBase + 200; // часы рендера раньше штампов
    for (int i = 0; i < 20; ++i) {
        src->render_add(src->self, l.data(), r.data(), 256);
        g_pc_bus_clock += 256u * kBusHz / kRate;
    }
    CHECK(stats_say("late 0,"));
}

// Одиночный чип: #FE в #FFFD - выбор несуществующего регистра, чип не
// меняется, канал A первого звучит в полную долю.
void live_single_chip_ignores_ts_select() {
    player::live::ay_live_configure(1, 1750000, 0);
    std::vector<Ev> ev;
    ev.push_back({kBase + 100, static_cast<uint16_t>(soundsinth::midi_in::kAyWriteSelect | 0xfeu)});
    reg(ev, kBase + 200, 7, 0x3f);
    reg(ev, kBase + 300, 8, 15);
    const std::vector<int32_t> l = run(ev, kRate / 4, 0);
    CHECK_EQ(l.back(), 16384 * 256);
    CHECK(stats_say("turbosound selects 0"));
    player::live::ay_live_configure(0, 1750000, 0);
}

// TurboSound: записи второму чипу (#FE) не попадают в первый.
void live_turbosound_keeps_chips_apart() {
    player::live::ay_live_configure(3, 1750000, 0);
    std::vector<Ev> ev;
    ev.push_back({kBase + 100, static_cast<uint16_t>(soundsinth::midi_in::kAyWriteSelect | 0xfeu)});
    reg(ev, kBase + 200, 7, 0x3f);
    reg(ev, kBase + 300, 9, 15); // второй чип, канал B - справа при ACB
    ev.push_back({kBase + 400, static_cast<uint16_t>(soundsinth::midi_in::kAyWriteSelect | 0xffu)});
    reg(ev, kBase + 500, 7, 0x3f);
    reg(ev, kBase + 600, 8, 15); // первый чип, канал A - слева
    soundsinth::mixbus::SoundSource* src = player::live::ay_live_source();
    std::vector<int32_t> l(kRate / 4, 0), r(kRate / 4, 0);
    size_t e = 0;
    for (uint32_t done = 0; done < l.size(); done += 256u) {
        g_pc_bus_clock = kBase + static_cast<uint64_t>(done) * kBusHz / kRate;
        while (e < ev.size() && ev[e].bus <= g_pc_bus_clock) {
            push(static_cast<uint32_t>(ev[e].bus), ev[e].write);
            ++e;
        }
        const uint32_t n = static_cast<uint32_t>(l.size()) - done < 256u ? static_cast<uint32_t>(l.size()) - done : 256u;
        src->render_add(src->self, &l[done], &r[done], n);
    }
    // Оба чипа на две трети: A первого - слева, B второго - справа.
    const int32_t want = 16384 * 170;
    CHECK_EQ(l.back(), want);
    CHECK_EQ(r.back(), want);
    CHECK(stats_say("turbosound selects 2"));
    player::live::ay_live_configure(0, 1750000, 0);
}

// Сброс машины: звучащий канал смолкает, запись, ждущая в кольце своей
// задержки, выброшена, запись после сброса играет.
void live_machine_reset_silences() {
    player::live::ay_live_configure(1, 1750000, 0);
    std::vector<Ev> ev;
    reg(ev, kBase + 100, 7, 0x3f);
    reg(ev, kBase + 200, 8, 15);
    std::vector<int32_t> l = run(ev, kRate / 4, 0);
    CHECK(l.back() != 0);
    soundsinth::mixbus::SoundSource* src = player::live::ay_live_source();
    std::vector<int32_t> a(256), b(256);
    uint64_t now = kBase + static_cast<uint64_t>(kRate / 4) * kBusHz / kRate;
    // До сброса: громкость B - в кольце, её такт ещё не настал.
    push(static_cast<uint32_t>(now), soundsinth::midi_in::kAyWriteSelect | 9u);
    push(static_cast<uint32_t>(now + 24), 15);
    player::live::ay_live_machine_reset();
    player::live::ay_live_machine_reset(); // два запроса до рендера - один сброс
    for (int i = 0; i < 20; ++i) {
        std::fill(a.begin(), a.end(), 0);
        std::fill(b.begin(), b.end(), 0);
        g_pc_bus_clock = now;
        src->render_add(src->self, a.data(), b.data(), 256);
        now += 256u * kBusHz / kRate;
    }
    CHECK_EQ(a.back(), 0);
    CHECK_EQ(b.back(), 0);
    CHECK(stats_say("resets 1"));
    // После сброса чип принимает записи как обычно.
    push(static_cast<uint32_t>(now), soundsinth::midi_in::kAyWriteSelect | 8u);
    push(static_cast<uint32_t>(now + 24), 15);
    for (int i = 0; i < 20; ++i) {
        std::fill(a.begin(), a.end(), 0);
        g_pc_bus_clock = now;
        src->render_add(src->self, a.data(), b.data(), 256);
        now += 256u * kBusHz / kRate;
    }
    CHECK(a.back() != 0);
}

// Управление MultiSound: запись 1111xxxx в #FFFD.
void control(std::vector<Ev>& v, uint64_t bus, uint8_t value) {
    v.push_back({bus, static_cast<uint16_t>(soundsinth::midi_in::kAyWriteSelect | value)});
}

// Канал FM 0 первого чипа: алгоритм 7, звучит один оператор без модуляции;
// блок 4, F-Number 1024 - 1024 * 2^3 * (3.5 МГц / 72) / 2^20 = 379.8 Гц.
void fm_tone(std::vector<Ev>& v, uint64_t bus) {
    static constexpr uint8_t kPatch[][2] = {
        {0x30, 0x01}, {0x40, 0x00}, {0x50, 0x1f}, {0x60, 0x00}, {0x70, 0x00}, {0x80, 0x0f}, {0x44, 0x7f},
        {0x48, 0x7f}, {0x4c, 0x7f}, {0xb0, 0x07}, {0xa4, 0x24}, {0xa0, 0x00}, {0x28, 0xf0},
    };
    for (const auto& w : kPatch) {
        reg(v, bus, w[0], w[1]);
        bus += 200;
    }
}

// TurboSound FM: FM звучит своей высотой, пока разряд 2 управления в нуле
// (#FB), и молчит при единице (#FF); записи регистров FM принимаются и без
// звука.
void live_tsfm_plays_fm() {
    player::live::ay_live_configure(4, 1750000, 0);
    std::vector<Ev> ev;
    control(ev, kBase + 500, 0xff);
    fm_tone(ev, kBase + 1000);
    CHECK_EQ(run(ev, kRate / 2u, 0).back(), 0);

    ev.clear();
    control(ev, kBase + 500, 0xfb);
    const std::vector<int32_t> l = run(ev, 2u * kRate, 3);
    int32_t lo = l[kRate / 2], hi = l[kRate / 2];
    for (uint32_t i = kRate / 2; i < kRate * 3 / 2; ++i) {
        if (l[i] < lo) lo = l[i];
        if (l[i] > hi) hi = l[i];
    }
    const int32_t mid = (lo + hi) / 2;
    uint32_t up       = 0;
    for (uint32_t i = kRate / 2 + 1; i <= kRate * 3 / 2; ++i)
        if (l[i - 1] < mid && l[i] >= mid) ++up;
    std::printf("  live TSFM tone: %u rising crossings in 1 s (want 379-380), swing %d\n", up, hi - lo);
    CHECK(up >= 379 && up <= 380);
    CHECK(hi - lo > 1000000);

    ev.clear();
    control(ev, kBase + 500, 0xff);
    const std::vector<int32_t> q = run(ev, kRate / 2u, 0);
    CHECK_EQ(q.back(), 0);
}

// Сброс машины: FM-часть сброшена и выключена - после #FB без новой записи
// регистров тишина.
void live_tsfm_machine_reset() {
    player::live::ay_live_configure(4, 1750000, 0);
    std::vector<Ev> ev;
    control(ev, kBase + 500, 0xfb);
    fm_tone(ev, kBase + 1000);
    CHECK(run(ev, kRate / 2u, 0).back() != 0);
    player::live::ay_live_machine_reset();
    ev.clear();
    control(ev, kBase + 500, 0xfb);
    CHECK_EQ(run(ev, kRate / 2u, 0).back(), 0);
}

// Чип, которому в FM не писали, не считается; первая же нота после секунды
// простоя играет своей высотой, а после снятия клавиши выход - снова ноль.
void live_tsfm_plays_after_idle() {
    player::live::ay_live_configure(4, 1750000, 0);
    std::vector<Ev> ev;
    control(ev, kBase + 500, 0xfb);
    fm_tone(ev, kBase + kBusHz);
    reg(ev, kBase + kBusHz * 5u / 2u, 0x28, 0x00);
    const std::vector<int32_t> l = run(ev, 4u * kRate, 3);
    CHECK_EQ(l[kRate / 2], 0);
    const uint32_t from = kRate * 5 / 4, to = kRate * 9 / 4;
    int32_t lo = l[from], hi = l[from];
    for (uint32_t i = from; i < to; ++i) {
        if (l[i] < lo) lo = l[i];
        if (l[i] > hi) hi = l[i];
    }
    const int32_t mid = (lo + hi) / 2;
    uint32_t up       = 0;
    for (uint32_t i = from + 1; i <= to; ++i)
        if (l[i - 1] < mid && l[i] >= mid) ++up;
    std::printf("  live TSFM tone after idle: %u rising crossings in 1 s (want 379-380)\n", up);
    CHECK(up >= 379 && up <= 380);
    CHECK_EQ(l.back(), 0);
}

// Остановка под журнал VGM и возврат по настройкам: остановленный чип
// молчит, запись за время остановки пропадает, после возврата чип чист.
void live_stop_and_restore() {
    player::live::ay_live_configure(1, 1750000, 0);
    std::vector<Ev> ev;
    reg(ev, kBase + 100, 7, 0x3f);
    reg(ev, kBase + 200, 8, 15);
    CHECK(run(ev, kRate / 4, 0).back() != 0);
    player::live::ay_live_configure(0, 1750000, 0);
    ev.clear();
    reg(ev, kBase + 100, 9, 15);
    CHECK_EQ(run(ev, kRate / 4, 0).back(), 0);
    player::live::ay_live_configure(1, 1750000, 0);
    ev.clear();
    CHECK_EQ(run(ev, kRate / 4, 0).back(), 0);
}

// Что машина прочтёт из порта #FD при старшем байте адреса hi.
uint32_t port_read(uint8_t hi = 0xff) {
    return reinterpret_cast<const uint8_t*>(player::live::g_ay_port_page)[hi];
}

void port_reg(uint8_t r, uint8_t v) {
    player::live::ay_port_write(true, r);
    player::live::ay_port_write(false, v);
}

// Чтение #FFFD по режимам: одиночный AY-3-8910 и YM2149, TurboSound,
// TurboSound FM, сброс машины, выключенный чип.
void live_port_answers() {
    using player::live::ay_port_write;
    // AY-3-8910: недоступные разряды нулями; ответ только при A15 = A14 = 1.
    const uint32_t sets = g_port_page_sets[0xfd];
    player::live::ay_live_configure(1, 1750000, 0);
    CHECK_EQ(g_port_page_sets[0xfd], sets + 1u);
    CHECK_EQ(port_read(), 0xffu); // регистр не выбран
    port_reg(1, 0xff);
    CHECK_EQ(port_read(), 0x0fu);
    CHECK_EQ(port_read(0xc0), 0x0fu);
    CHECK_EQ(port_read(0xbf), 0xffu);
    CHECK_EQ(port_read(0x7f), 0xffu);
    ay_port_write(true, 0xfe); // одиночному чипу - чужой адрес
    CHECK_EQ(port_read(), 0xffu);
    ay_port_write(false, 0x55); // данные после него - мимо
    ay_port_write(true, 1);
    CHECK_EQ(port_read(), 0x0fu);
    // YM2149: как записано; порт A на вход читает единицы, на выход - защёлку.
    player::live::ay_live_configure(2, 1750000, 0);
    port_reg(1, 0xf5);
    CHECK_EQ(port_read(), 0xf5u);
    port_reg(14, 0x55);
    CHECK_EQ(port_read(), 0xffu);
    port_reg(7, 0x40);
    ay_port_write(true, 14);
    CHECK_EQ(port_read(), 0x55u);
    // TurboSound: регистры у каждого чипа свои.
    player::live::ay_live_configure(3, 1750000, 0);
    ay_port_write(true, 0xff);
    port_reg(0, 0x11);
    ay_port_write(true, 0xfe);
    CHECK_EQ(port_read(), 0xffu); // чип выбран, регистр - нет
    port_reg(0, 0x22);
    CHECK_EQ(port_read(), 0x22u);
    ay_port_write(true, 0xff);
    ay_port_write(true, 0);
    CHECK_EQ(port_read(), 0x11u);
    // Сброс машины: регистры в ноль, регистр не выбран.
    player::live::ay_live_machine_reset();
    CHECK_EQ(port_read(), 0xffu);
    ay_port_write(true, 0);
    CHECK_EQ(port_read(), 0u);
    // TurboSound FM: вне SSG ноль; разряд 1 управления в нуле - состояние.
    player::live::ay_live_configure(4, 1750000, 0);
    ay_port_write(true, 0xff);
    port_reg(0, 0x33);
    CHECK_EQ(port_read(), 0x33u);
    ay_port_write(true, 0x30);
    CHECK_EQ(port_read(), 0u);
    ay_port_write(true, 0xfd);
    ay_port_write(true, 0);
    CHECK_EQ(port_read(), 0u); // состояние, а не регистр 0
    ay_port_write(true, 0xff);
    ay_port_write(true, 0);
    CHECK_EQ(port_read(), 0x33u);
    // Выключено: порт отдан машине, записи тень не трогают.
    const uint32_t clears = g_port_clears[0xfd];
    player::live::ay_live_configure(0, 1750000, 0);
    CHECK(g_port_clears[0xfd] > clears);
    port_reg(0, 0x44);
    CHECK_EQ(port_read(), 0x33u);
    player::live::ay_live_configure(1, 1750000, 0);
}

} // namespace

void run_ay_live_tests() {
    std::printf("player: live AY from the bus\n");
    // Чипы берут память в начале арены трека: арена обязана быть, и без
    // метаданных трека.
    if (shared::g_track_memory.resident.base == nullptr) soundsinth::memory::track_memory_create(shared::g_track_memory);
    soundsinth::memory::track_memory_reset_for_new_track(shared::g_track_memory);
    player::live::live_clock_set(&test_clock);
    live_tone_pitch();
    live_writes_keep_their_moments();
    live_stamp_younger_than_the_clock();
    live_single_chip_ignores_ts_select();
    live_turbosound_keeps_chips_apart();
    live_machine_reset_silences();
    live_tsfm_plays_fm();
    live_tsfm_machine_reset();
    live_tsfm_plays_after_idle();
    live_stop_and_restore();
    live_port_answers();
    player::live::live_clock_set(nullptr);
}
