// SPDX-License-Identifier: MIT
// Цифровой звук машины целиком: записи со штампом шины через своё кольцо в
// источник микшера. Место ступеньки во времени, стороны Covox и Soundrive,
// уровни бипера, утечка постоянной и тишина, потери кольца.

#include "testing.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/memory/track_memory.h"
#include "player/live/dac_live.h"
#include "player/live/live_clock.h"
#include "player/shared_state.h"

extern char g_test_log[8192];

namespace {

using player::live::DacPort;

uint64_t g_bus_clock = 0;
uint64_t test_clock() {
    return g_bus_clock;
}

constexpr uint64_t kBusHz = 2000000;
constexpr uint32_t kRate  = 44100;
// Задержка звука от записи - 50 мс, kLiveDelayHalves.
constexpr uint64_t kDelay = kBusHz / 20u;

struct Ev {
    uint64_t bus;
    DacPort port;
    uint8_t data;
};

// Рендер по 256 отсчётов с часов start; записи кладутся в кольцо, когда их
// время настало.
std::vector<int32_t> run(const std::vector<Ev>& ev, uint64_t start, uint32_t frames, std::vector<int32_t>* right = nullptr) {
    soundsinth::mixbus::SoundSource* src = player::live::dac_live_source();
    std::vector<int32_t> l(frames, 0), r(frames, 0);
    size_t e = 0;
    for (uint32_t done = 0; done < frames; done += 256u) {
        const uint64_t now = start + static_cast<uint64_t>(done) * kBusHz / kRate;
        g_bus_clock        = now;
        while (e < ev.size() && ev[e].bus <= now) {
            player::live::g_dac_ring.push(static_cast<uint32_t>(ev[e].bus), static_cast<uint8_t>(ev[e].port), ev[e].data);
            ++e;
        }
        const uint32_t n = frames - done < 256u ? frames - done : 256u;
        src->render_add(src->self, &l[done], &r[done], n);
    }
    if (right != nullptr) *right = r;
    return l;
}

// Первый отсчёт, где |v| дошёл до половины want.
uint32_t half_at(const std::vector<int32_t>& v, int32_t want) {
    for (uint32_t i = 0; i < v.size(); ++i)
        if (std::abs(v[i]) * 2 >= std::abs(want)) return i;
    return static_cast<uint32_t>(v.size());
}

bool silent(const std::vector<int32_t>& v) {
    for (int32_t s : v)
        if (s != 0) return false;
    return true;
}

bool stats_say(const char* want) {
    g_test_log[0] = 0;
    player::live::dac_live_log_stats();
    return std::string(g_test_log).find(want) != std::string::npos;
}

// Отсчёт записи через x полумикросекунд после начала рендера: рендер
// привязывает часы при первом вызове, звук идёт на kDelay позже.
uint32_t sample_of(uint64_t x) {
    return static_cast<uint32_t>((x + kDelay) * kRate / kBusHz);
}

// Covox: байт 0xC0 - ступенька +64 * 64 в обе стороны, середина ступеньки
// BLEP на 8 отсчётов позже записи.
void covox_step_lands_in_time_on_both_sides() {
    player::live::dac_live_configure(true, false, 0);
    constexpr uint64_t kBase = 1000000, kAt = 20000;
    std::vector<int32_t> r;
    const std::vector<int32_t> l = run({{kBase + kAt, DacPort::Covox, 0xc0}}, kBase, kRate / 5u, &r);
    const int32_t step           = 0x40 * 64 * 256;
    const uint32_t at            = half_at(l, step);
    std::printf("  covox step at sample %u (want %u)\n", at, sample_of(kAt) + 8u);
    CHECK(at >= sample_of(kAt) + 7u && at <= sample_of(kAt) + 9u);
    CHECK(l == r);
    CHECK(stats_say("writes 1,"));
    CHECK(stats_say("late 0"));
}

// Запись, лежавшая в кольце, пока рендер стоял 0.3 с (меню NMI): старше
// полуоборота штампа она выглядит записью из будущего. После простоя она
// ложится сразу, а не через 0.2 с.
void record_through_render_gap() {
    player::live::dac_live_configure(true, false, 0);
    constexpr uint64_t kBase = 30000000;
    run({{kBase + 5000, DacPort::Covox, 0xc0}}, kBase, 512);
    const uint64_t resume        = kBase + 11610 + kBusHz * 3u / 10u;
    const std::vector<int32_t> l = run({}, resume, kRate / 20u);
    const uint32_t at            = half_at(l, 0x40 * 64 * 256);
    std::printf("  record through a 0.3 s render gap: step at sample %u of %u\n", at, kRate / 20u);
    CHECK(at < 64u);
}

// Soundrive: #0F и #1F только слева, #4F и #5F только справа; два ЦАП
// стороны складываются.
void soundrive_sides() {
    player::live::dac_live_configure(false, true, 0);
    constexpr uint64_t kBase = 10000000;
    std::vector<int32_t> r;
    std::vector<int32_t> l = run({{kBase + 1000, DacPort::SdA, 0xa0}, {kBase + 1000, DacPort::SdB, 0xa0}}, kBase, kRate / 5u, &r);
    const uint32_t i       = sample_of(1000) + 14u;
    CHECK(std::abs(l[i] - 2 * 0x20 * 64 * 256) < 2 * 0x20 * 64 * 256 / 30);
    CHECK(silent(r));

    // Левый уровень между прогонами не утёк: рендера не было. Заново.
    player::live::dac_live_configure(false, true, 0);
    constexpr uint64_t kRight = kBase + 4u * kBusHz;
    l                         = run({{kRight + 1000, DacPort::SdC, 0x60}, {kRight + 1000, DacPort::SdD, 0x80}}, kRight, kRate / 5u, &r);
    CHECK(silent(l));
    CHECK(std::abs(r[i] + 0x20 * 64 * 256) < 0x20 * 64 * 256 / 30);
}

// Бипер 48K: MIC один - 1638, EAR - 15759, оба - 16384 (ULA Issue 3).
void beeper_levels() {
    player::live::dac_live_configure(false, false, 2);
    constexpr uint64_t kBase = 20000000;
    const uint32_t i         = sample_of(1000) + 14u;
    const uint8_t data[3]    = {0x08, 0x10, 0x18};
    const int32_t want[3]    = {1638, 15759, 16384};
    for (uint32_t k = 0; k < 3; ++k) {
        player::live::dac_live_configure(false, false, 2);
        const uint64_t at            = kBase + k * 4u * kBusHz;
        const std::vector<int32_t> l = run({{at + 1000, DacPort::Beeper, data[k]}}, at, kRate / 5u);
        std::printf("  beeper %02x: %d (want %d)\n", data[k], l[i] / 256, want[k]);
        CHECK(std::abs(l[i] / 256 - want[k]) <= want[k] / 30 + 1);
    }
}

// Постоянный уровень утекает (ФВЧ 7 Гц), и источник замолкает совсем: не
// трогает буфер и не считает.
void dc_leaks_to_silence() {
    player::live::dac_live_configure(true, false, 0);
    constexpr uint64_t kBase     = 40000000;
    const std::vector<int32_t> l = run({{kBase + 1000, DacPort::Covox, 0xff}}, kBase, 2u * kRate);
    CHECK(l[sample_of(1000) + 20u] > 0);
    CHECK(silent(std::vector<int32_t>(l.end() - kRate / 2u, l.end())));
}

// Без чтения кольцо переполняется: старое потеряно и посчитано.
void ring_overflow_counts_lost() {
    player::live::dac_live_configure(true, false, 0);
    constexpr uint64_t kBase = 50000000;
    for (uint32_t i = 0; i < player::live::kDacRing + 100u; ++i) {
        player::live::g_dac_ring.push(static_cast<uint32_t>(kBase + i), static_cast<uint8_t>(DacPort::Covox), static_cast<uint8_t>(i));
    }
    run({}, kBase + player::live::kDacRing + 200u, kRate / 2u);
    CHECK(stats_say("lost 100,"));
}

// Всё выключено: источник молчит и записей не разбирает.
void off_stays_silent() {
    player::live::dac_live_configure(false, false, 0);
    CHECK(!player::live::dac_live_enabled());
    constexpr uint64_t kBase = 60000000;
    CHECK(silent(run({{kBase + 1000, DacPort::Covox, 0xff}}, kBase, kRate / 5u)));
}

// Делитель синка при 300 МГц - ближайший в 1/256 (i2s_sink.cpp): 106 +
// 75/256, вывод 44099.81 Гц, ниже номинала - поправка отрицательная. И
// соседний 106 + 74/256, 44101.43 Гц - выше. За 30 минут шины такт записи
// обязан уйти ровно на столько, сколько рендер отыграл сверх номинала или
// недоиграл, - иначе съеден запас задержки.
void out_rate_skew_follows_output() {
    using player::live::LiveClock;
    constexpr uint32_t kSys = 300000000u, kDiv = (kSys * 4u + kRate / 2u) / kRate;
    static_assert(kDiv == 106u * 256u + 75u, "sink divider at 300 MHz");
    LiveClock c;
    c.sync(kDelay, 0);
    constexpr uint64_t kSpan = 1800u * kBusHz;
    const uint64_t now       = kSpan; // привязка: шина 0 - такт 0
    for (const uint32_t div : {kDiv, kDiv - 1u}) {
        const uint64_t actual_mhz = uint64_t{kSys} * 4000u / div;
        player::live::live_clock_set_out_rate(actual_mhz, kRate);
        CHECK(div == kDiv ? player::live::g_live_out_skew_q32 < 0 : player::live::g_live_out_skew_q32 > 0);
        const uint64_t tick = c.tick_of(static_cast<uint32_t>(now), now, kRate, kBusHz);
        const double want   = 1800.0 * static_cast<double>(actual_mhz) / 1000.0;
        std::printf("  out %.3f Hz: tick after 30 min %llu (want %.0f, nominal %u)\n", actual_mhz / 1000.0, static_cast<unsigned long long>(tick), want,
                    1800u * kRate);
        CHECK(std::abs(static_cast<double>(tick) - want) < 2.0);
    }
    player::live::live_clock_set_out_rate(kRate * 1000ull, kRate);
    CHECK_EQ(player::live::g_live_out_skew_q32, 0);
    CHECK_EQ(c.tick_of(static_cast<uint32_t>(now), now, kRate, kBusHz), uint64_t{1800u} * kRate);
}

} // namespace

void run_dac_live_tests() {
    out_rate_skew_follows_output();
    std::printf("player: live Covox, Soundrive and beeper from the bus\n");
    if (shared::g_track_memory.resident.base == nullptr) soundsinth::memory::track_memory_create(shared::g_track_memory);
    soundsinth::memory::track_memory_reset_for_new_track(shared::g_track_memory);
    player::live::live_clock_set(&test_clock);
    covox_step_lands_in_time_on_both_sides();
    record_through_render_gap();
    soundrive_sides();
    beeper_levels();
    dc_leaks_to_silence();
    ring_overflow_counts_lost();
    off_stays_silent();
    player::live::live_clock_set(nullptr);
}
