// SPDX-License-Identifier: MIT
#include "player/live/dac_live.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <new>

#include "platform/log.h"

#include "core/chips/blep.h"
#include "core/engine/engine_defs.h"
#include "core/memory/arena.h"
#include "player/live/live_clock.h"
#include "player/shared_state.h"

namespace player::live {

TapRing<kDacRing> g_dac_ring;
volatile uint8_t g_dac_on = 0;

namespace {

using soundsinth::chips::blep_add;
using soundsinth::chips::kBlepPhases;
using soundsinth::chips::kBlepTaps;

constexpr uint32_t kBatch   = 256;
constexpr uint32_t kSources = 6; // по DacPort
// Байт ЦАП - отсчёт int16: 0x80 - ноль, шаг 64, размах 16384 - как у
// полного уровня канала AY.
constexpr int32_t kDacStep = 64;
// Бипер 48K, вывод 28 ULA Issue 2: (EAR, MIC) 00 - 0.39 В, 01 - 0.73,
// 10 - 3.66, 11 - 3.79 (World of Spectrum, 48K reference, порт #FE; у Issue
// 3 на 0.05-0.10 В ниже). От нуля, 3.40 В - 16384.
constexpr int32_t kBeeperLevel[4] = {0, 1638, 15759, 16384};
// ФВЧ 7 Гц: итог разностей утекает на 1/1024 за отсчёт. Уровень держится в
// Q8, иначе утечка встала бы на остатке до 1023.
constexpr uint32_t kLeakShift = 10;
// Итог меньше этого (в Q8 - 4 единицы int16) с пустыми строками - тишина.
constexpr int32_t kQuiet = 1024;

// Стороны, куда звучит источник: разряд 0 - слева, 1 - справа.
constexpr uint8_t kSides[kSources] = {3, 1, 1, 2, 2, 3};

// Строки разностей двух сторон - в начале арены трека, как у живого SAA.
// Хвост ступенек, kBlepTaps отсчётов, переезжает в начало строки.
struct Unit {
    int32_t rows[2][kBatch + kBlepTaps];
};
Unit* s_unit = nullptr;

TapReader s_tap; // пишет рендер
LiveClock s_time;
uint64_t s_pos            = 0; // начало следующего батча, 1/kBlepPhases отсчёта
int32_t s_level[kSources] = {};
int32_t s_acc[2]          = {}; // итог разностей стороны, Q8
uint32_t s_busy           = 0;  // батчей со ступеньками в строках
uint32_t s_writes         = 0;
uint32_t s_late           = 0;

soundsinth::mixbus::SoundSource s_source;

int32_t level_of(DacPort port, uint8_t data) {
    if (port == DacPort::Beeper) return kBeeperLevel[(data >> 3) & 3u];
    return (static_cast<int32_t>(data) - 0x80) * kDacStep;
}

// Записи с тиком внутри батча из n отсчётов - ступеньками в строки.
// stale - записи до простоя рендера: все на начало батча (LiveClock::sync).
void drain(uint64_t now, uint32_t n, bool stale = false) {
    const uint64_t end = s_pos + static_cast<uint64_t>(n) * kBlepPhases;
    uint32_t e         = 0;
    while (g_dac_ring.peek(s_tap, e)) {
        uint64_t tick = stale ? s_pos : s_time.tick_of(tap_stamp(e), now, static_cast<uint64_t>(soundsinth::engine::kSampleRateHz) * kBlepPhases, kLiveBusHz);
        if (tick >= end) return;
        if (tick < s_pos) {
            tick = s_pos;
            ++s_late;
        }
        const auto port = static_cast<DacPort>(tap_port(e));
        const auto src  = static_cast<uint32_t>(port);
        ++s_writes;
        if (src < kSources) {
            const int32_t v = level_of(port, tap_data(e));
            const int32_t d = v - s_level[src];
            s_level[src]    = v;
            if (d != 0) {
                const auto at = static_cast<uint32_t>(tick - s_pos);
                for (uint32_t side = 0; side < 2; ++side) {
                    if (kSides[src] & (1u << side)) blep_add(s_unit->rows[side], at / kBlepPhases, at % kBlepPhases, d);
                }
                s_busy = 2; // этот батч и хвост в следующем
            }
        }
        tap_next(s_tap);
    }
}

// Во флеше: зовётся раз на буфер.
void render_add(void*, int32_t* mix_l, int32_t* mix_r, uint32_t n) {
    if (g_dac_on == 0u) return;
    const uint64_t now = live_clock_now();
    uint32_t e         = 0;
    // Тишина: ни записей, ни ступенек, итог утёк. Отсчёт времени после
    // простоя - заново.
    if (s_busy == 0 && !g_dac_ring.peek(s_tap, e) && s_acc[0] < kQuiet && s_acc[0] > -kQuiet && s_acc[1] < kQuiet && s_acc[1] > -kQuiet) {
        s_time.anchored = false;
        return;
    }
    bool stale            = s_time.sync(now, s_pos);
    int32_t* const mix[2] = {mix_l, mix_r};
    for (uint32_t at = 0; at < n;) {
        const uint32_t k = n - at < kBatch ? n - at : kBatch;
        drain(now, k, stale);
        stale = false;
        for (uint32_t side = 0; side < 2; ++side) {
            int32_t* row = s_unit->rows[side];
            int32_t acc  = s_acc[side];
            int32_t* out = mix[side] + at;
            // Масштаб шины Q24.8: итог в Q8 - и есть вклад.
            for (uint32_t i = 0; i < k; ++i) {
                acc    += (row[i] << 8) - (acc >> kLeakShift);
                out[i] += acc;
            }
            s_acc[side] = acc;
            std::memmove(row, row + k, kBlepTaps * sizeof(int32_t));
            std::memset(row + kBlepTaps, 0, k * sizeof(int32_t));
        }
        s_pos += static_cast<uint64_t>(k) * kBlepPhases;
        if (s_busy != 0) --s_busy;
        at += k;
    }
}

} // namespace

void dac_live_configure(bool covox, bool soundrive, uint8_t beeper) {
    g_dac_on = 0;
    if (!covox && !soundrive && beeper == 0u) return;
    if (s_unit == nullptr) {
        void* const p = soundsinth::memory::arena_keep(shared::g_track_memory.resident, sizeof(Unit), alignof(Unit));
        if (p == nullptr) {
            debug_log("dac: no room at the start of the track arena, the machine DACs are off\n");
            return;
        }
        s_unit = new (p) Unit();
    }
    std::memset(s_unit, 0, sizeof(Unit));
    std::memset(s_level, 0, sizeof(s_level));
    s_acc[0] = s_acc[1] = 0;
    s_busy              = 0;
    s_time.anchored     = false;
    s_tap.tail          = g_dac_ring.head_now();
    const uint8_t beep  = beeper == 0u ? 0u : beeper == 1u ? kDacBeeperEar : kDacBeeperEar | kDacBeeperMic;
    std::atomic_thread_fence(std::memory_order_release); // флаг - после состояния, как у AY
    g_dac_on = (covox ? kDacCovox : 0u) | (soundrive ? kDacSoundrive : 0u) | beep;
    debug_logf("dac: covox %u, soundrive %u, beeper %u, delay %u ms\n", covox ? 1u : 0u, soundrive ? 1u : 0u, static_cast<unsigned>(beeper),
               static_cast<unsigned>(kLiveDelayHalves * 1000u / kLiveBusHz));
}

soundsinth::mixbus::SoundSource* dac_live_source() {
    s_source.self       = nullptr;
    s_source.render_add = &render_add;
    return &s_source;
}

void dac_live_log_stats() {
    if (g_dac_on == 0u) return;
    debug_logf("dac: writes %" PRIu32 ", lost %" PRIu32 ", late %" PRIu32 "\n", s_writes, s_tap.lost, s_late);
}

} // namespace player::live
