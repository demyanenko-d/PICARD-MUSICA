// SPDX-License-Identifier: MIT
#include "player/live/saa_live.h"

#include <atomic>
#include <cinttypes>
#include <new>

#include "platform/log.h"

#include "core/chips/saa_chip.h"
#include "core/engine/engine_defs.h"
#include "core/memory/arena.h"
#include "player/live/live_clock.h"
#include "player/live/live_tap.h"
#include "player/shared_state.h"

namespace player::live {

namespace {

using soundsinth::chips::SaaChip;

constexpr uint32_t kClockHz = 8000000; // такт SAA1099 на MultiSound
// Шесть каналов на полном уровне дают в сумме 6 x 16320; треть оставляет
// сумму на уровне трёх каналов AY в моно.
constexpr int32_t kGain = 85; // из 256

// Чип, строки разностей его выходов и отсчёты батча - в неприкосновенном
// начале арены трека.
struct Unit {
    SaaChip chip;
    SaaChip::Row rows[SaaChip::kOutputs];
    int16_t out[SaaChip::kOutputs][SaaChip::kMaxBatch];
};
Unit* s_unit = nullptr;

TapReader s_tap; // свой хвост в кольце отвода, пишет рендер
LiveClock s_time;
bool s_clock_on   = false; // такт чипа разрешён, пишет рендер
uint32_t s_writes = 0;     // записей чипа принято, пишет рендер
uint32_t s_late   = 0;     // пишет рендер
TapReset s_reset;          // сброс машины

soundsinth::mixbus::SoundSource s_source;
uint32_t s_heard = 0; // голосов слышно, пишет рендер

bool is_write(TapPort port) {
    return port == TapPort::SaaAddr || port == TapPort::SaaData;
}
// Управление MultiSound: запись 1111xxxx в #FFFD.
bool is_control(TapPort port, uint8_t data) {
    return port == TapPort::AySelect && (data & 0xf0u) == 0xf0u;
}
bool clock_enabled(uint8_t control) {
    return (control & 0x08u) == 0;
}

// Такта нет: есть ли в отводе запись чипа или включение такта. Чужие записи
// и управление без включения по пути забираются.
bool wakes() {
    uint32_t e = 0;
    while (tap_peek(s_tap, e)) {
        const TapPort port = tap_port(e);
        if (is_write(port)) return true;
        if (is_control(port, tap_data(e)) && clock_enabled(tap_data(e))) return true;
        tap_next(s_tap);
    }
    return false;
}

// Записи с тактом внутри батча, по порядку.
// stale - записи до простоя рендера: все на такт привязки (LiveClock::sync).
void drain(uint64_t now, bool stale = false) {
    SaaChip& chip = s_unit->chip;
    uint32_t e    = 0;
    while (tap_peek(s_tap, e)) {
        const TapPort port = tap_port(e);
        const uint8_t data = tap_data(e);
        const bool control = is_control(port, data);
        if (control || is_write(port)) {
            // Такт ядра - вход чипа / 2.
            uint64_t tick = stale ? s_time.origin_tick : s_time.tick_of(tap_stamp(e), now, kClockHz, 2u * kLiveBusHz);
            if (tick > chip.last_units()) return;
            if (control) {
                s_clock_on = clock_enabled(data);
            } else {
                ++s_writes;
                if (tick < chip.now()) {
                    tick = chip.now();
                    ++s_late;
                }
                chip.run_to(tick);
                if (port == TapPort::SaaAddr) {
                    chip.write_address(data);
                } else {
                    chip.write_data(data);
                }
            }
        }
        tap_next(s_tap);
    }
}

// Сброс машины: такт снят, регистры в ноль, записи до сброса выброшены.
void machine_reset() {
    if (!s_reset.take(s_tap)) return;
    s_clock_on = false;
    s_unit->chip.reset();
}

// Во флеше: зовётся раз на буфер.
void render_add(void*, int32_t* mix_l, int32_t* mix_r, uint32_t n) {
    if (!g_saa_live_on) return;
    machine_reset();
    // Без такта чип стоит: считать нечего, пока нет записи или включения.
    // Отсчёт времени после простоя - заново.
    if (!s_clock_on && !wakes()) {
        s_time.anchored = false;
        s_heard         = 0;
        return;
    }
    SaaChip& chip      = s_unit->chip;
    const uint64_t now = live_clock_now();
    bool stale         = s_time.sync(now, chip.now());
    int16_t* out[SaaChip::kOutputs];
    for (uint32_t c = 0; c < SaaChip::kOutputs; ++c) {
        out[c] = s_unit->out[c];
    }
    for (uint32_t at = 0; at < n;) {
        const uint32_t k = n - at < SaaChip::kMaxBatch ? n - at : SaaChip::kMaxBatch;
        chip.begin(k, s_unit->rows);
        drain(now, stale);
        stale = false;
        // Без такта выход чипа не звучит: батч нужен только регистрам.
        if (chip.end(out) && s_clock_on) {
            // Масштаб шины Q24.8: отсчёт канала * 256 при доле 256.
            for (uint32_t i = 0; i < k; ++i) {
                int32_t l = 0, r = 0;
                for (uint32_t c = 0; c < SaaChip::kChannels; ++c) {
                    l += out[2u * c][i];
                    r += out[2u * c + 1u][i];
                }
                mix_l[at + i] += l * kGain;
                mix_r[at + i] += r * kGain;
            }
        }
        at += k;
    }
    s_heard = s_clock_on ? chip.heard() : 0u;
}

} // namespace

volatile bool g_saa_live_on = false;

void saa_live_configure(bool on) {
    g_saa_live_on = false;
    if (!on) return;
    if (s_unit == nullptr) {
        void* const p = soundsinth::memory::arena_keep(shared::g_track_memory.resident, sizeof(Unit), alignof(Unit));
        if (p == nullptr) {
            debug_log("saa: no room at the start of the track arena, the live chip is off\n");
            return;
        }
        s_unit = new (p) Unit();
    }
    s_unit->chip.init(kClockHz, soundsinth::engine::kSampleRateHz);
    s_clock_on      = false;
    s_time.anchored = false;
    s_tap.tail      = tap_head();
    // Флаг - последним и после барьера: обработчик шины на Core1 и рендер
    // видят его только с готовым состоянием (configure зовётся и под
    // работающим рендером, по концу VGM).
    std::atomic_thread_fence(std::memory_order_release);
    g_saa_live_on = true;
    debug_logf("saa: live SAA1099 at %" PRIu32 " Hz on ports #FF/#1FF, delay %u ms\n", kClockHz, static_cast<unsigned>(kLiveDelayHalves * 1000u / kLiveBusHz));
}

void saa_live_machine_reset() {
    s_reset.request();
}

soundsinth::mixbus::SoundSource* saa_live_source() {
    s_source.self       = nullptr;
    s_source.render_add = &render_add;
    return &s_source;
}

uint32_t saa_live_heard() {
    return g_saa_live_on ? s_heard : 0u;
}

void saa_live_log_stats() {
    if (!g_saa_live_on) return;
    debug_logf("saa: live writes %" PRIu32 ", lost %" PRIu32 ", late %" PRIu32 ", clock %s, resets %" PRIu32 "\n", s_writes, s_tap.lost, s_late,
               s_clock_on ? "on" : "off", s_reset.count);
}

} // namespace player::live
