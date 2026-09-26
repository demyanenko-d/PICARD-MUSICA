// Отвод записей AY (ay_tap.h).

#include "player/live/ay_tap.h"

#include <atomic>
#include <cinttypes>

#include "platform/hot_path.h"
#include "platform/log.h"
#include "core/live_midi/ay_midi.h"

namespace player::live {

namespace {

// Один писатель (обработчик записи на Core1), один читатель (Core0).
// Ёмкость - kAyTapRing из заголовка: её показывает лог живого режима.
constexpr uint32_t kRing = kAyTapRing;
uint16_t s_ring[kRing];
std::atomic<uint32_t> s_head{0}; // пишет Core1
std::atomic<uint32_t> s_tail{0}; // пишет Core0
uint32_t s_lost = 0;             // пишет Core1

} // namespace

void SOUNDSINTH_HOT_PATH(ay_tap_push)(uint16_t ay_write) {
    const uint32_t h = s_head.load(std::memory_order_relaxed);
    if (h - s_tail.load(std::memory_order_acquire) >= kRing) {
        ++s_lost;
        return;
    }
    s_ring[h & (kRing - 1u)] = ay_write;
    s_head.store(h + 1u, std::memory_order_release);
}

bool ay_tap_pop(uint16_t& ay_write) {
    const uint32_t t = s_tail.load(std::memory_order_relaxed);
    if (t == s_head.load(std::memory_order_acquire)) return false;
    ay_write = s_ring[t & (kRing - 1u)];
    s_tail.store(t + 1u, std::memory_order_release);
    return true;
}

uint32_t ay_tap_lost() {
    return s_lost;
}

namespace {

soundsinth::midi_in::AyMidiInput s_input;
uint32_t s_events = 0;
uint32_t s_unprinted = 0;
uint32_t s_printed = 0; // строк событий за период строки счётчиков

void log_event(const soundsinth::midi_in::MidiEvent& e) {
    using soundsinth::midi_in::MidiKind;
    switch (e.kind) {
        case MidiKind::NoteOn:
            debug_logf("midi: к%u нота %u вкл, скорость %u\n", e.channel + 1u, e.a, e.b);
            break;
        case MidiKind::NoteOff:
            debug_logf("midi: к%u нота %u выкл\n", e.channel + 1u, e.a);
            break;
        case MidiKind::Control:
            debug_logf("midi: к%u контроллер %u = %u\n", e.channel + 1u, e.a, e.b);
            break;
        case MidiKind::Program:
            debug_logf("midi: к%u программа %u\n", e.channel + 1u, e.a);
            break;
        case MidiKind::PitchBend:
            debug_logf("midi: к%u бенд %u\n", e.channel + 1u, e.value);
            break;
        default:
            debug_logf("midi: к%u давление %u %u\n", e.channel + 1u, e.a, e.b);
            break;
    }
}

} // namespace

void ay_tap_drain_and_log() {
    // Строк событий за период строки счётчиков не больше этого: плотный
    // поток переполняет кольцо лога, остальное - в счётчик.
    constexpr uint32_t kLinesPerPeriod = 16;
    uint16_t w = 0;
    soundsinth::midi_in::MidiEvent e{};
    while (ay_tap_pop(w)) {
        if (!s_input.feed(w, e)) continue;
        ++s_events;
        if (s_printed < kLinesPerPeriod) {
            log_event(e);
            ++s_printed;
        } else {
            ++s_unprinted;
        }
    }
}

void ay_tap_log_stats() {
    static uint32_t s_last_bytes = 0;
    s_printed = 0;
    if (s_input.bytes() == s_last_bytes && s_lost == 0) return;
    s_last_bytes = s_input.bytes();
    debug_logf("midi: байт %" PRIu32 ", событий %" PRIu32 " (не напечатано %" PRIu32 "), ошибок кадра %" PRIu32
               ", без статуса %" PRIu32 ", потеряно в кольце %" PRIu32 "\n",
               s_input.bytes(), s_events, s_unprinted, s_input.framing_errors(), s_input.skipped_bytes(), s_lost);
}

} // namespace player::live
