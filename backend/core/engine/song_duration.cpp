#include "core/engine/song_duration.h"

#include <cstring>
#include <new>
#include <type_traits>

namespace soundsinth::engine {

namespace {

// Отметки посещённых (order_pos, row) - битовый массив в буфере
// вызывающего, без кучи: std::set берёт около 28 байт на каждую новую пару
// без предела, а на RP2350 после статических арен куча почти пустая -
// длинный трек роняет pico-sdk в "*** PANIC *** Out of memory".
//
// Пределы 256 x 256; позиции за ними (у .mid бывает больше 256) не
// отмечаются, общий предохранитель - kTickGuard.
constexpr uint32_t kVisitedOrderBound = 256;
constexpr uint32_t kVisitedRowBound = 256;
constexpr uint32_t kVisitedBits = kVisitedOrderBound * kVisitedRowBound; // 65536 бит = 8192 байта

// true, если (order_pos, row) уже была отмечена (бит взводится в любом случае).
bool visited_test_and_set(uint8_t* bits, uint16_t order_pos, uint16_t row) {
    if (order_pos >= kVisitedOrderBound || row >= kVisitedRowBound) return false;
    const uint32_t idx = static_cast<uint32_t>(order_pos) * kVisitedRowBound + row;
    uint8_t& byte = bits[idx / 8];
    const uint8_t mask = static_cast<uint8_t>(1u << (idx % 8));
    const bool was_set = (byte & mask) != 0;
    byte |= mask;
    return was_set;
}

void visited_clear(uint8_t* bits, uint16_t order_pos, uint16_t row) {
    if (order_pos >= kVisitedOrderBound || row >= kVisitedRowBound) return;
    const uint32_t idx = static_cast<uint32_t>(order_pos) * kVisitedRowBound + row;
    bits[idx / 8] &= static_cast<uint8_t>(~(1u << (idx % 8)));
}

// Раскладка буфера: отметки, за ними PlayState. false - буфер мал или не
// выровнен.
bool split_scratch(uint8_t* scratch, uint32_t scratch_bytes, uint8_t*& visited_bits, PlayState*& ps) {
    if (scratch == nullptr || scratch_bytes < kDurationScratchBytes) return false;
    if (reinterpret_cast<uintptr_t>(scratch) % alignof(PlayState) != 0) return false;
    visited_bits = scratch;
    std::memset(visited_bits, 0, kVisitedBits / 8);
    ps = new (scratch + kVisitedScratchBytes) PlayState();
    return true;
}

static_assert(kVisitedBits / 8 == kVisitedScratchBytes, "размер отметок разошёлся с объявленным в .h");
static_assert(kVisitedScratchBytes % alignof(PlayState) == 0, "PlayState за отметками не выровнен");
static_assert(std::is_trivially_destructible_v<PlayState>, "PlayState в буфере не разрушается");

// Предел числа тиков - защита от вырожденных файлов, где цикл (order_pos,
// row) практически не замыкается: лучше остановиться с большой длиной, чем
// зависнуть. 20 млн тиков - больше 50 часов при tempo 255.
constexpr uint64_t kTickGuard = 20'000'000;

// Строк за один заход в позицию order: Pattern Loop, который снимает свои
// отметки и не кончается (E60/E61/E61 в одном канале), упирается в него, а
// не в kTickGuard.
constexpr uint32_t kRowsPerPositionLimit = 1024;

// Отметки ставятся на настоящей смене строки (on_new_row): повтор строки
// Pattern Delay её не читает. Сработавший Pattern Loop возвращает на
// пройденные строки - отметки тела петли [цель, источник] снимаются.
struct WalkCtx {
    uint8_t* visited_bits = nullptr;
    const PlayState* ps = nullptr;
    uint16_t prev_order_pos = 0xFFFF;
    uint16_t prev_row = 0;
    uint32_t rows_in_position = 0;
    DurationStats stats;
};

void walk_row(void* user, const soundsinth::model::PatternCell*, uint8_t) {
    WalkCtx& c = *static_cast<WalkCtx*>(user);
    const PlayState& ps = *c.ps;
    ++c.stats.rows;
    if (ps.order_pos != c.prev_order_pos) c.rows_in_position = 0;
    if (ps.last_advance_was_loop) {
        for (uint32_t row = ps.row; row <= c.prev_row; ++row) {
            visited_clear(c.visited_bits, ps.order_pos, static_cast<uint16_t>(row));
        }
    }
    if (visited_test_and_set(c.visited_bits, ps.order_pos, ps.row)) {
        c.stats.stop = DurationStop::Repeat;
    } else {
        if (c.prev_order_pos != 0xFFFF && ps.order_pos < c.prev_order_pos) c.stats.position_goes_back = true;
        if (++c.rows_in_position > kRowsPerPositionLimit) c.stats.stop = DurationStop::RowLimit;
    }
    c.stats.stop_order_pos = ps.order_pos;
    c.stats.stop_row = ps.row;
    c.prev_order_pos = ps.order_pos;
    c.prev_row = ps.row;
}

struct WalkResult {
    uint64_t frames = 0;
    uint16_t positions = 0; // сколько позиций order вошло, стартовая - первая
    DurationStats stats;
};

// Один проход секвенсора: до повтора пройденной (order_pos, row) - цикл
// замкнулся, дальше повтор, - до конца песни или до frame_limit отсчётов.
// Буфер не годится или воспроизводимых позиций нет - {0, 0}.
WalkResult walk(const soundsinth::model::Song& song, memory::PsramStore& psram, uint8_t* scratch, uint32_t scratch_bytes,
                uint64_t frame_limit) {
    WalkCtx c;
    PlayState* psp = nullptr;
    if (!split_scratch(scratch, scratch_bytes, c.visited_bits, psp)) return {};
    c.ps = psp;
    PlayState& ps = *psp;
    if (!sequencer_init(song, psram, ps, &walk_row, &c)) return {};
    c.stats.stop = DurationStop::None;

    WalkResult r;
    // Тик 0 строки 0 длительность получил в sequencer_init, движок его играет.
    r.frames = ps.last_tick_samples;
    r.positions = 1;
    uint16_t last_pos = ps.order_pos;
    uint64_t guard = 0;
    for (; guard < kTickGuard && r.frames < frame_limit; ++guard) {
        ++c.stats.ticks;
        if (!sequencer_tick(song, psram, ps, &walk_row, &c)) {
            c.stats.stop = DurationStop::SongEnd;
            break;
        }
        // Тик, на котором прочитана строка повтора, - уже второй проход (как
        // у OpenMPT GetLength: посещённая строка не прибавляется).
        if (c.stats.stop != DurationStop::None) break;
        r.frames += ps.last_tick_samples;
        if (ps.order_pos != last_pos) {
            last_pos = ps.order_pos;
            ++r.positions;
        }
    }
    if (c.stats.stop == DurationStop::None) {
        c.stats.stop = guard >= kTickGuard ? DurationStop::TickGuard : DurationStop::FrameLimit;
    }
    r.stats = c.stats;
    return r;
}

} // namespace

uint32_t compute_song_total_frames(const soundsinth::model::Song& song, memory::PsramStore& psram, uint8_t* scratch,
                                   uint32_t scratch_bytes, DurationStats* stats) {
    const WalkResult r = walk(song, psram, scratch, scratch_bytes, UINT64_MAX);
    if (stats != nullptr) *stats = r.stats;
    return r.frames > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(r.frames);
}

const char* duration_stop_name(DurationStop stop) {
    switch (stop) {
        case DurationStop::Repeat:
            return "повтор";
        case DurationStop::SongEnd:
            return "конец песни";
        case DurationStop::RowLimit:
            return "предел строк";
        case DurationStop::TickGuard:
            return "предел тиков";
        case DurationStop::FrameLimit:
            return "предел кадров";
        default:
            return "нет прохода";
    }
}

uint16_t compute_order_positions_in_frames(const soundsinth::model::Song& song, memory::PsramStore& psram,
                                           uint8_t* scratch, uint32_t scratch_bytes, uint32_t frames) {
    return walk(song, psram, scratch, scratch_bytes, frames).positions;
}

} // namespace soundsinth::engine
