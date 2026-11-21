// SPDX-License-Identifier: MIT
#include "player/live/ay_live.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <new>

#include "ymfm_opn.h"

#include "platform/hot_path.h"
#include "platform/log.h"

#include "core/chips/ay_chip.h"
#include "core/engine/engine_defs.h"
#include "core/memory/arena.h"
#include "devices/hal/z80_ports.h"
#include "player/fm_tables.h"
#include "player/live/ay_port.h"
#include "player/live/live_clock.h"
#include "player/live/live_tap.h"
#include "player/shared_state.h"

namespace player::live {

namespace {

using soundsinth::chips::AyChip;
using soundsinth::engine::kSampleRateHz;

TapReader s_tap;           // свой хвост в кольце отвода, пишет рендер
uint32_t s_writes     = 0; // записей AY принято, пишет рендер
uint32_t s_late       = 0; // пишет рендер
uint32_t s_ts_selects = 0; // выборов чипа TurboSound (#FE/#FF), пишет рендер
TapReset s_reset;          // сброс машины

// TurboSound: два чипа на одних портах, запись 1111xxxx в #FFFD выбирает
// чип младшим разрядом: единица (#FF) - первый, ноль (#FE) - второй;
// разряды 1-3 - управление MultiSound, выбору не мешают. Без TurboSound это
// выбор несуществующего регистра, как у одиночного чипа.
constexpr uint32_t kChips = 2;
// Чип и строки разностей его выходов. Записи обоих чипов идут одним
// потоком, оба считаются разом: строки свои у каждого. Память - в
// неприкосновенном начале арены трека, берётся при включении: TurboSound -
// два чипа, иначе один.
struct Unit {
    AyChip ay;
    AyChip::Row rows[AyChip::kChannels];
};
Unit* s_unit     = nullptr;
uint32_t s_units = 0; // сколько взято в арене
uint32_t s_count = 0; // сколько в работе
uint32_t s_chip  = 0; // куда идут записи
bool s_ts        = false;
uint32_t s_clock = 1750000;
LiveClock s_time;
uint8_t s_reg[kChips] = {};
bool s_reg_ok[kChips] = {};

// TurboSound FM: у каждого чипа, кроме SSG (он же ядро AY), FM-часть YM2203 -
// три канала на движке OPNA. Номер регистра у YM2203 - байт целиком: до #10
// - SSG, дальше - FM. FM звучит, пока разряд 2 управления 1111xxxx в нуле;
// после сброса машины - молчит. Вход чипа - удвоенный такт SSG, отсчёт FM -
// вход / 72.
using FmEngine                 = ymfm::fm_engine_base<ymfm::opna_registers>;
constexpr uint32_t kFmChannels = 3;
constexpr uint32_t kFmMask     = (1u << kFmChannels) - 1u;
// Доля канала FM в обе стороны, из 256: уровень FM против SSG у железа не
// замерен; несущая на полном уровне - половина полного канала SSG.
constexpr int32_t kFmGain = 256;
struct FmIntf : ymfm::ymfm_interface {};
struct FmUnit {
    FmIntf intf;
    FmEngine engine{intf};
    int16_t out[kFmChannels][AyChip::kMaxBatch];
    uint32_t done = 0;     // отсчётов текущего батча посчитано
    bool sound    = false; // в текущем батче движок считался
};
FmUnit* s_fm_store  = nullptr; // пара, в той же памяти арены
FmUnit* s_fm        = nullptr; // она же, пока режим - TurboSound FM
uint32_t s_fm_ratio = 0;       // отсчётов FM на отсчёт выхода, Q24
bool s_fm_sound     = false;   // FM включён управлением
// Раскладка: доля канала слева и справа, 256 - целиком. Два чипа разом -
// каждый на две трети, чтобы сумма не вышла за шкалу.
int32_t s_gain_l[AyChip::kChannels] = {};
int32_t s_gain_r[AyChip::kChannels] = {};
const char* s_layout_name           = "";
uint32_t s_heard                    = 0; // голосов слышно, пишет рендер

soundsinth::mixbus::SoundSource s_source;

// FM чипа - до отсчёта to текущего батча. Чип, у которого все операторы
// затухли и клавиши отпущены, не считается: до записи его выход - ноль.
// Нули пишутся только в батче, где движок считался: иначе строки не читают.
void fm_run(FmUnit& fm, uint32_t to) {
    if (to <= fm.done) return;
    const auto zero = [&fm](uint32_t from, uint32_t till) {
        for (uint32_t c = 0; c < kFmChannels; ++c) {
            std::fill(fm.out[c] + from, fm.out[c] + till, int16_t{0});
        }
    };
    if (fm.engine.idle()) {
        if (fm.sound) zero(fm.done, to);
    } else {
        if (!fm.sound) zero(0, fm.done);
        int16_t* out[FmEngine::CHANNELS] = {};
        for (uint32_t c = 0; c < kFmChannels; ++c) {
            out[c] = fm.out[c] + fm.done;
        }
        fm.engine.generate_channels(kFmMask, out, to - fm.done, s_fm_ratio, 0, 32767);
        fm.sound = true;
    }
    fm.done = to;
}

void apply(TapPort port, uint8_t data, uint64_t tick) {
    ++s_writes;
    if (port == TapPort::AySelect) {
        if (s_ts && (data & 0xf0u) == 0xf0u) {
            s_chip = (data & 1u) != 0 ? 0u : 1u;
            ++s_ts_selects;
            s_fm_sound = (data & 0x04u) == 0;
        }
        if (s_fm != nullptr) {
            s_reg_ok[s_chip] = true;
            s_reg[s_chip]    = data;
            return;
        }
        // Выбор с ненулевой старшей тетрадой снимает выбор регистра: данные
        // после него чип не принимает.
        s_reg_ok[s_chip] = (data & 0xf0u) == 0;
        s_reg[s_chip]    = data & 0x0fu;
        return;
    }
    if (!s_reg_ok[s_chip]) return;
    AyChip& ay = s_unit[s_chip].ay;
    if (tick < ay.now()) {
        tick = ay.now();
        ++s_late;
    }
    const uint8_t reg = s_reg[s_chip];
    if (reg >= AyChip::kRegs) {
        // Регистр FM: запись ложится в свой отсчёт батча.
        FmUnit& fm = s_fm[s_chip];
        fm_run(fm, ay.sample_at(tick));
        uint8_t value = data;
        if (ymfm::ym2203_on_opna(reg, value)) fm.engine.write(reg, value);
        return;
    }
    ay.run_to(tick);
    ay.write(reg, data);
}

// Записи с тактом внутри батча, по порядку. Батч у чипов общий.
// stale - записи до простоя рендера: все на такт привязки (LiveClock::sync).
void drain(uint64_t now, bool stale = false) {
    uint32_t e = 0;
    while (tap_peek(s_tap, e)) {
        const TapPort port = tap_port(e);
        if (port == TapPort::AySelect || port == TapPort::AyData) {
            // Такт ядра - вход чипа / 8.
            const uint64_t tick = stale ? s_time.origin_tick : s_time.tick_of(tap_stamp(e), now, s_clock, 8u * kLiveBusHz);
            if (tick > s_unit[0].ay.last_units()) return;
            apply(port, tap_data(e), tick);
        }
        tap_next(s_tap);
    }
}

// Сброс в начале батча: записи до него выброшены, регистры в ноль записями,
// чтобы перепад уровня лёг ступенькой.
void machine_reset() {
    if (!s_reset.take(s_tap)) return;
    for (uint32_t c = 0; c < s_count; ++c) {
        for (uint8_t r = 0; r < AyChip::kRegs; ++r) {
            s_unit[c].ay.write(r, 0);
        }
        s_reg_ok[c] = false;
        if (s_fm != nullptr) s_fm[c].engine.reset();
    }
    s_chip     = 0;
    s_fm_sound = false;
}

// Во флеше: зовётся раз на буфер; в SRAM - только приём из обработчика шины.
void render_add(void*, int32_t* mix_l, int32_t* mix_r, uint32_t n) {
    if (!g_ay_live_on) return;
    const uint64_t now = live_clock_now();
    bool stale         = s_time.sync(now, s_unit[0].ay.now());
    for (uint32_t at = 0; at < n;) {
        const uint32_t k = n - at < AyChip::kMaxBatch ? n - at : AyChip::kMaxBatch;
        for (uint32_t c = 0; c < s_count; ++c) {
            s_unit[c].ay.begin(k, s_unit[c].rows);
            if (s_fm != nullptr) {
                s_fm[c].done  = 0;
                s_fm[c].sound = false;
            }
        }
        machine_reset();
        drain(now, stale);
        stale = false;
        // С первого выбора чипа TurboSound оба чипа - на две трети, и доля не
        // меняется от батча к батчу: смена была бы ступенькой громкости.
        // Буфер один на чип по очереди - кадр стека рендера невелик.
        const int32_t both = s_ts_selects != 0 ? 170 : 256;
        int16_t buf[AyChip::kChannels][AyChip::kMaxBatch];
        int16_t* const out[AyChip::kChannels] = {buf[0], buf[1], buf[2]};
        for (uint32_t c = 0; c < s_count; ++c) {
            if (s_fm != nullptr) {
                // FM считается и при звуке, выключенном управлением:
                // состояние чипа идёт дальше.
                FmUnit& fm = s_fm[c];
                fm_run(fm, k);
                if (s_fm_sound && fm.sound) {
                    for (uint32_t i = 0; i < k; ++i) {
                        const int32_t v  = ((fm.out[0][i] + fm.out[1][i] + fm.out[2][i]) * kFmGain >> 8) * both;
                        mix_l[at + i]   += v;
                        mix_r[at + i]   += v;
                    }
                }
            }
            if (!s_unit[c].ay.end(out)) continue; // весь батч тишина
            // Масштаб шины Q24.8: отсчёт канала * 256 при доле 256.
            for (uint32_t i = 0; i < k; ++i) {
                mix_l[at + i] += ((buf[0][i] * s_gain_l[0] + buf[1][i] * s_gain_l[1] + buf[2][i] * s_gain_l[2]) >> 8) * both;
                mix_r[at + i] += ((buf[0][i] * s_gain_r[0] + buf[1][i] * s_gain_r[1] + buf[2][i] * s_gain_r[2]) >> 8) * both;
            }
        }
        at += k;
    }
    uint32_t heard = 0;
    for (uint32_t c = 0; c < s_count; ++c) {
        heard += s_unit[c].ay.heard();
        if (s_fm != nullptr && s_fm_sound) heard += s_fm[c].engine.heard_channels(kFmMask, 0x200);
    }
    s_heard = heard;
}

// Ответ на чтение #FFFD - с чистого листа. AY-3-8910 недоступные разряды
// регистров читает нулями, YM2149 и YM2203 - как записаны.
void port_start(uint8_t mode) {
    static constexpr uint8_t kAyMask[16] = {0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff, 0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff};
    for (uint32_t r = 0; r < 16; ++r) {
        g_ay_port.mask[r] = mode == 1 ? kAyMask[r] : 0xff;
    }
    g_ay_port.reset();
    std::fill(std::begin(g_ay_port_page), std::end(g_ay_port_page), 0xffffffffu);
    g_ay_port.mode = mode;
    ay_port_publish();
    devices::hal::z80_port_set_read_page(kAyPort, reinterpret_cast<const uint8_t*>(g_ay_port_page));
}

} // namespace

volatile bool g_ay_live_on = false;
AyPort g_ay_port;
alignas(256) SOUNDSINTH_BSS_SECTION("ay_port_page") uint32_t g_ay_port_page[64];

SOUNDSINTH_NOINLINE void SOUNDSINTH_HOT_PATH(ay_port_write)(bool select, uint8_t data) {
    AyPort& p = g_ay_port;
    if (p.mode == 0) return;
    if (select) {
        p.select(data);
    } else {
        p.write(data);
    }
    ay_port_publish();
}

void ay_live_configure(uint8_t mode, uint32_t clock_hz, uint8_t stereo) {
    g_ay_live_on = false;
    // Чтение #FFFD - машине, пока чипы не заведены.
    devices::hal::z80_port_clear_read(kAyPort);
    g_ay_port.mode = 0;
    s_clock        = clock_hz;
    if (mode == 0) return;
    // ACB: A слева, B справа, C пополам; ABC: B пополам. Моно - каждый на
    // две трети, чтобы сумма трёх полных не вышла за шкалу.
    static constexpr int32_t kLayout[3][2][3] = {
        {{256, 0, 128}, {0, 256, 128}},
        {{256, 128, 0}, {0, 128, 256}},
        {{170, 170, 170}, {170, 170, 170}},
    };
    static constexpr const char* kLayoutName[3] = {"ACB", "ABC", "mono"};
    const uint32_t lay                          = stereo < 3u ? stereo : 0u;
    for (uint32_t c = 0; c < AyChip::kChannels; ++c) {
        s_gain_l[c] = kLayout[lay][0][c];
        s_gain_r[c] = kLayout[lay][1][c];
    }
    s_layout_name        = kLayoutName[lay];
    const bool fm        = mode == 4;
    const uint32_t count = mode >= 3 ? kChips : 1u;
    if (s_units < count) {
        void* const p = soundsinth::memory::arena_keep(shared::g_track_memory.resident, count * sizeof(Unit), alignof(Unit));
        if (p == nullptr) {
            debug_log("ay: no room at the start of the track arena, live chips are off\n");
            return;
        }
        s_unit  = static_cast<Unit*>(p);
        s_units = count;
    }
    // Чипы - с чистого листа: и регистры, и уровни выходов.
    for (uint32_t c = 0; c < count; ++c) {
        new (&s_unit[c]) Unit();
    }
    if (fm && s_fm_store == nullptr) {
        void* const p = soundsinth::memory::arena_keep(shared::g_track_memory.resident, kChips * sizeof(FmUnit), alignof(FmUnit));
        if (p == nullptr) {
            debug_log("ay: no room at the start of the track arena for the FM part, live chips are off\n");
            return;
        }
        s_fm_store = static_cast<FmUnit*>(p);
        for (uint32_t c = 0; c < kChips; ++c) {
            new (&s_fm_store[c]) FmUnit();
            s_fm_store[c].engine.set_tables(fm_tables());
        }
    }
    s_fm       = fm ? s_fm_store : nullptr;
    s_fm_sound = false;
    if (fm) {
        for (uint32_t c = 0; c < kChips; ++c) {
            s_fm[c].engine.reset();
        }
        s_fm_ratio = static_cast<uint32_t>(((static_cast<uint64_t>(clock_hz) * 2u) << 24) / (72u * kSampleRateHz));
    }
    s_count = count;
    for (uint32_t c = 0; c < count; ++c) {
        s_unit[c].ay.init(clock_hz, mode >= 2, kSampleRateHz);
    }
    s_ts            = mode >= 3;
    s_chip          = 0;
    s_time.anchored = false;
    s_reg_ok[0]     = false;
    s_reg_ok[1]     = false;
    s_tap.tail      = tap_head();
    port_start(mode);
    // Флаг - последним и после барьера: обработчик шины на Core1 и рендер
    // видят его только с готовым состоянием (configure зовётся и под
    // работающим рендером, по концу VGM).
    std::atomic_thread_fence(std::memory_order_release);
    g_ay_live_on                              = true;
    static constexpr const char* kChipName[5] = {"", "AY-3-8910", "YM2149", "TurboSound 2x YM2149", "TurboSound FM 2x YM2203"};
    debug_logf("ay: live %s at %" PRIu32 " Hz, %s, delay %u ms\n", kChipName[mode < 5u ? mode : 0u], clock_hz, s_layout_name,
               static_cast<unsigned>(kLiveDelayHalves * 1000u / kLiveBusHz));
}

void ay_live_machine_reset() {
    g_ay_port.reset();
    ay_port_publish();
    s_reset.request();
}

soundsinth::mixbus::SoundSource* ay_live_source() {
    s_source.self       = nullptr;
    s_source.render_add = &render_add;
    return &s_source;
}

uint32_t ay_live_heard() {
    return g_ay_live_on ? s_heard : 0u;
}

void ay_live_log_stats() {
    if (!g_ay_live_on) return;
    debug_logf("ay: live writes %" PRIu32 ", lost %" PRIu32 ", late %" PRIu32 ", turbosound selects %" PRIu32 ", resets %" PRIu32 "\n", s_writes, s_tap.lost,
               s_late, s_ts_selects, s_reset.count);
}

} // namespace player::live
