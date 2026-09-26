#include "testing.h"

#include <vector>

#include "ay_rom_writer.h"
#include "core/live_midi/ay_midi.h"

using namespace soundsinth::midi_in;

namespace {

using Rom = pc_tests::AyRomWriter;

std::vector<MidiEvent> run(const Rom& rom, AyMidiInput& in) {
    std::vector<MidiEvent> out;
    MidiEvent e{};
    for (uint16_t w : rom.w) {
        if (in.feed(w, e)) out.push_back(e);
    }
    return out;
}

void test_note_on_from_rom_stream() {
    Rom rom;
    rom.bytes({0x90, 60, 100});
    AyMidiInput in;
    const auto ev = run(rom, in);
    CHECK_EQ(ev.size(), size_t{1});
    CHECK(ev[0].kind == MidiKind::NoteOn);
    CHECK_EQ(ev[0].channel, uint8_t{0});
    CHECK_EQ(ev[0].a, uint8_t{60});
    CHECK_EQ(ev[0].b, uint8_t{100});
    CHECK_EQ(in.bytes(), uint32_t{3});
    CHECK_EQ(in.framing_errors(), uint32_t{0});
}

void test_running_status_and_zero_velocity() {
    Rom rom;
    rom.bytes({0x93, 60, 100, 62, 90, 60, 0});
    AyMidiInput in;
    const auto ev = run(rom, in);
    CHECK_EQ(ev.size(), size_t{3});
    CHECK(ev[0].kind == MidiKind::NoteOn);
    CHECK_EQ(ev[1].a, uint8_t{62});
    CHECK(ev[2].kind == MidiKind::NoteOff);
    CHECK_EQ(ev[2].channel, uint8_t{3});
}

// Музыка на том же AY пишет другие регистры между байтами MIDI: поток
// линии это не трогает.
void test_other_registers_do_not_disturb() {
    Rom rom;
    rom.select(kAyRegPortA);
    rom.byte(0xC5);
    rom.select(7);
    rom.data(0x38);
    rom.select(0);
    rom.data(0x55);
    rom.select(kAyRegPortA);
    rom.byte(0x10);
    AyMidiInput in;
    const auto ev = run(rom, in);
    CHECK_EQ(ev.size(), size_t{1});
    CHECK(ev[0].kind == MidiKind::Program);
    CHECK_EQ(ev[0].channel, uint8_t{5});
    CHECK_EQ(ev[0].a, uint8_t{0x10});
}

void test_pitch_bend_and_control() {
    Rom rom;
    rom.bytes({0xE0, 0x00, 0x40, 0xB1, 7, 127});
    AyMidiInput in;
    const auto ev = run(rom, in);
    CHECK_EQ(ev.size(), size_t{2});
    CHECK(ev[0].kind == MidiKind::PitchBend);
    CHECK_EQ(ev[0].value, uint16_t{8192});
    CHECK(ev[1].kind == MidiKind::Control);
    CHECK_EQ(ev[1].a, uint8_t{7});
    CHECK_EQ(ev[1].b, uint8_t{127});
}

// SysEx пропускается целиком, реальное время внутри сообщения не мешает.
void test_sysex_and_realtime() {
    Rom rom;
    rom.bytes({0xF0, 0x41, 0x10, 0x42, 0xF7, 0x90, 64, 0xF8, 80});
    AyMidiInput in;
    const auto ev = run(rom, in);
    CHECK_EQ(ev.size(), size_t{1});
    CHECK(ev[0].kind == MidiKind::NoteOn);
    CHECK_EQ(ev[0].a, uint8_t{64});
    CHECK_EQ(ev[0].b, uint8_t{80});
}

// Тело SysEx собирается началом и отдаётся на F7 - ровно на той записи, где
// сообщение кончилось.
void test_sysex_body_collected() {
    Rom rom;
    rom.bytes({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x15, 0x02, 0x18, 0xF7});
    AyMidiInput in;
    MidiEvent e{};
    uint32_t ready = 0;
    for (uint16_t w : rom.w) {
        in.feed(w, e);
        if (!in.sysex_ready()) continue;
        ++ready;
        CHECK_EQ(in.sysex_len(), uint32_t{9});
        CHECK_EQ(in.sysex()[0], uint8_t{0x41});
        CHECK_EQ(in.sysex()[5], uint8_t{0x11}); // блок партии
        CHECK_EQ(in.sysex()[7], uint8_t{0x02}); // набор ударных
    }
    CHECK_EQ(ready, uint32_t{1});
}

// Сообщение длиннее сохраняемого начала: длина считается вся, лишнее не
// пишется. Оборванное новым статусом - не отдаётся вовсе.
void test_sysex_long_and_broken() {
    Rom rom;
    rom.select(kAyRegPortA);
    rom.byte(0xF0);
    for (uint8_t i = 0; i < 40; ++i) rom.byte(static_cast<uint8_t>(i));
    rom.byte(0xF7);
    rom.byte(0xF0);
    rom.byte(0x7e);
    rom.byte(0x90); // новый статус вместо F7 - сообщение оборвано
    rom.byte(60);
    rom.byte(100);
    AyMidiInput in;
    MidiEvent e{};
    uint32_t ready = 0, notes = 0;
    for (uint16_t w : rom.w) {
        if (in.feed(w, e)) ++notes;
        if (!in.sysex_ready()) continue;
        ++ready;
        CHECK_EQ(in.sysex_len(), uint32_t{40});
        CHECK_EQ(in.sysex()[7], uint8_t{7});
    }
    CHECK_EQ(ready, uint32_t{1});
    CHECK_EQ(notes, uint32_t{1});
}

// Кадр без стоп-бита считается ошибкой, поток ждёт покоя и дальше
// разбирается как ни в чём не бывало.
void test_framing_error_resyncs() {
    Rom rom;
    rom.select(kAyRegPortA);
    rom.bit(false);
    for (int i = 0; i < 8; ++i) rom.bit(true);
    rom.bit(false); // вместо стоп-бита
    rom.bit(true);  // покой
    rom.byte(0x90);
    rom.byte(60);
    rom.byte(1);
    AyMidiInput in;
    const auto ev = run(rom, in);
    CHECK_EQ(in.framing_errors(), uint32_t{1});
    CHECK_EQ(ev.size(), size_t{1});
    CHECK(ev[0].kind == MidiKind::NoteOn);
}

// Данные без статуса (подключились посреди потока) пропускаются.
void test_data_before_status_skipped() {
    Rom rom;
    rom.bytes({60, 100, 0x80, 60, 0});
    AyMidiInput in;
    const auto ev = run(rom, in);
    CHECK_EQ(in.skipped_bytes(), uint32_t{2});
    CHECK_EQ(ev.size(), size_t{1});
    CHECK(ev[0].kind == MidiKind::NoteOff);
}

} // namespace

void run_ay_midi_tests() {
    test_note_on_from_rom_stream();
    test_running_status_and_zero_velocity();
    test_other_registers_do_not_disturb();
    test_pitch_bend_and_control();
    test_sysex_and_realtime();
    test_sysex_body_collected();
    test_sysex_long_and_broken();
    test_framing_error_resyncs();
    test_data_before_status_skipped();
}
