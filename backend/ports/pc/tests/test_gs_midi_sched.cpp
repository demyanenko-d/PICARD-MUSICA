// SPDX-License-Identifier: MIT
// Расписание MIDI по кадрам: то, что карта General Sound отдаёт залитому
// плееру, а он выталкивает в линию с порта A звукогенератора.
//
// Проверяется байт в байт: порция кадра - это поток MIDI, и всё, что в
// неё попало лишнего или не в том виде, Спектрум выдаст в линию как есть.

#include "testing.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "player/gs/midi_sched.h"

using player::gs::kMidiFrameData;
using player::gs::kMidiWorkSize;
using player::gs::MidiSched;

namespace {

// Файл .mid собирается байт за байтом, чтобы видеть, что попадает в линию.
// Заголовок MThd планировщик не читает (сведения о файле подставляет
// плеер), он здесь для полноты файла.
struct MidiBuilder {
    std::vector<uint8_t> bytes;
    uint32_t track_at = 0;

    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v >> 8));
        u8(static_cast<uint8_t>(v));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v >> 16));
        u16(static_cast<uint16_t>(v));
    }
    void tag(const char* s) {
        for (int i = 0; i < 4; ++i)
            u8(static_cast<uint8_t>(s[i]));
    }
    // Переменная длина MIDI, старшими группами вперёд.
    void varlen(uint32_t v) {
        uint8_t buf[5];
        int n    = 0;
        buf[n++] = static_cast<uint8_t>(v & 0x7f);
        while ((v >>= 7) != 0)
            buf[n++] = static_cast<uint8_t>((v & 0x7f) | 0x80);
        while (n-- > 0)
            u8(buf[n]);
    }

    void header(uint16_t division) {
        tag("MThd");
        u32(6);
        u16(0);
        u16(1);
        u16(division);
    }
    void track_begin() {
        tag("MTrk");
        u32(0); // длину впишем в track_end
        track_at = static_cast<uint32_t>(bytes.size());
    }
    void track_end() {
        varlen(0);
        u8(0xff);
        u8(0x2f);
        u8(0x00);
        const uint32_t len = static_cast<uint32_t>(bytes.size()) - track_at;
        for (int i = 0; i < 4; ++i)
            bytes[track_at - 4 + i] = static_cast<uint8_t>(len >> (24 - 8 * i));
    }

    void note_on(uint32_t delta, uint8_t ch, uint8_t note, uint8_t vel) {
        varlen(delta);
        u8(static_cast<uint8_t>(0x90 | ch));
        u8(note);
        u8(vel);
    }
    void sysex(uint32_t delta, const uint8_t* body, uint32_t len) {
        varlen(delta);
        u8(0xf0);
        varlen(len);
        for (uint32_t i = 0; i < len; ++i)
            u8(body[i]);
    }
};

devices::gs::MidiFileFacts facts_for(const MidiBuilder& m, uint16_t division) {
    devices::gs::MidiFileFacts f;
    f.division    = division;
    f.first_track = 14; // MThd это 14 байт
    f.track_count = 1;
    f.file_length = static_cast<uint16_t>(m.bytes.size());
    return f;
}

// Собрать первый непустой кадр: пока плеер ничего не получил, расписание
// отдаёт только замыкающий #FF.
uint16_t first_data_frame(MidiSched& s, int limit = 8) {
    for (int i = 0; i < limit; ++i) {
        const uint16_t n = s.next_frame();
        if (n > 1) return n;
    }
    return 0;
}

std::vector<uint8_t> work_buf() {
    return std::vector<uint8_t>(kMidiWorkSize, 0);
}

// Дамп GM Reset - самый ходовой SysEx в коллекции.
const uint8_t kGmReset[] = {0x7e, 0x7f, 0x09, 0x01, 0xf7};

void sysex_short_goes_through() {
    MidiBuilder m;
    m.header(96);
    m.track_begin();
    m.sysex(0, kGmReset, sizeof(kGmReset));
    m.note_on(0, 0, 60, 100);
    m.track_end();

    auto work                                   = work_buf();
    uint8_t state[devices::gs::kMidiStateBytes] = {};
    MidiSched s;
    CHECK(s.begin(m.bytes.data(), (uint32_t)m.bytes.size(), facts_for(m, 96), work.data(), state));

    const uint16_t n = first_data_frame(s);
    CHECK(n > 1);
    const uint8_t* f = s.frame();
    // Статус #F0, тело как есть, затем нота. Плеер пропускает #F0..#F7 в
    // линию целиком.
    CHECK_EQ(f[0], 0xf0);
    CHECK_EQ(std::memcmp(f + 1, kGmReset, sizeof(kGmReset)), 0);
    CHECK_EQ(f[1 + sizeof(kGmReset)], 0x90);
    CHECK_EQ(f[2 + sizeof(kGmReset)], 60);
}

// Главная проверка: SysEx, не влезший в кадр, обязан уехать в следующий
// ЦЕЛИКОМ и без посторонних байт. Отказ здесь слышен - тело дампа уходит
// в линию как ноты.
void sysex_over_frame_moves_whole() {
    // Набиваем кадр нотами почти до потолка, потом кладём длинный дамп.
    MidiBuilder m;
    m.header(96);
    m.track_begin();
    const int notes = kMidiFrameData / 3; // по три байта на ноту
    for (int i = 0; i < notes; ++i)
        m.note_on(0, 0, static_cast<uint8_t>(60 + i), 100);
    uint8_t body[40];
    for (uint32_t i = 0; i < sizeof(body) - 1; ++i)
        body[i] = static_cast<uint8_t>(i + 1);
    body[sizeof(body) - 1] = 0xf7;
    m.sysex(0, body, sizeof(body));
    m.note_on(0, 0, 36, 100);
    m.track_end();

    auto work                                   = work_buf();
    uint8_t state[devices::gs::kMidiStateBytes] = {};
    MidiSched s;
    CHECK(s.begin(m.bytes.data(), (uint32_t)m.bytes.size(), facts_for(m, 96), work.data(), state));

    // Первый кадр: только ноты, дамп не влез.
    const uint16_t n1 = first_data_frame(s);
    CHECK_EQ(n1, static_cast<uint16_t>(notes * 3 + 1));
    const uint8_t* f1 = s.frame();
    CHECK_EQ(f1[0], 0x90);
    CHECK_EQ(f1[n1 - 1], 0xff);

    // Второй кадр: дамп целиком, со своим статусом и ни байтом раньше.
    const uint16_t n2 = s.next_frame();
    const uint8_t* f2 = s.frame();
    CHECK(n2 > 1);
    CHECK_EQ(f2[0], 0xf0);
    CHECK_EQ(std::memcmp(f2 + 1, body, sizeof(body)), 0);
    // И сразу за ним нота, которая стояла в файле после дампа.
    CHECK_EQ(f2[1 + sizeof(body)], 0x90);
    CHECK_EQ(f2[2 + sizeof(body)], 36);
}

// SysEx длиннее порции кадра. Таких в коллекции немного, но они есть, и
// расписание обязано их пережить: отдать, что может, и дойти до конца
// песни, а не встать молча.
void sysex_longer_than_frame_survives() {
    uint8_t body[80];
    for (uint32_t i = 0; i < sizeof(body) - 1; ++i)
        body[i] = static_cast<uint8_t>((i & 0x7f));
    body[sizeof(body) - 1] = 0xf7;

    MidiBuilder m;
    m.header(96);
    m.track_begin();
    m.sysex(0, body, sizeof(body));
    m.note_on(0, 0, 60, 100);
    m.track_end();

    auto work                                   = work_buf();
    uint8_t state[devices::gs::kMidiStateBytes] = {};
    MidiSched s;
    CHECK(s.begin(m.bytes.data(), (uint32_t)m.bytes.size(), facts_for(m, 96), work.data(), state));

    // Нота за дампом обязана дойти до линии, и песня - до конца.
    bool saw_note = false;
    for (int i = 0; i < 64 && !saw_note; ++i) {
        const uint16_t n = s.next_frame();
        const uint8_t* f = s.frame();
        for (uint16_t k = 0; k + 2 < n; ++k) {
            if (f[k] == 0x90 && f[k + 1] == 60) saw_note = true;
        }
    }
    CHECK(saw_note);
    CHECK(s.events() > 0);
}

// Дамп, разрезанный между кадрами, обязан дойти подряд. Вторая дорожка
// стоит на том же такте и готова писать - её байты не должны встать в
// середину.
void sysex_split_is_not_interleaved() {
    uint8_t body[90];
    for (uint32_t i = 0; i < sizeof(body) - 1; ++i)
        body[i] = static_cast<uint8_t>(1 + (i % 100));
    body[sizeof(body) - 1] = 0xf7;

    MidiBuilder m;
    m.header(96);
    m.track_begin();
    m.sysex(0, body, sizeof(body));
    m.track_end();
    m.track_begin();
    for (int i = 0; i < 8; ++i)
        m.note_on(0, 1, static_cast<uint8_t>(40 + i), 100);
    m.track_end();

    auto work                                   = work_buf();
    uint8_t state[devices::gs::kMidiStateBytes] = {};
    devices::gs::MidiFileFacts f                = facts_for(m, 96);
    f.track_count                               = 2;
    MidiSched s;
    CHECK(s.begin(m.bytes.data(), (uint32_t)m.bytes.size(), f, work.data(), state));

    // Склеиваем линию из кадров и ищем в ней дамп целиком.
    std::vector<uint8_t> line;
    for (int i = 0; i < 32; ++i) {
        const uint16_t n  = s.next_frame();
        const uint8_t* fr = s.frame();
        for (uint16_t k = 0; k + 1 < n; ++k)
            line.push_back(fr[k]); // без замыкающего #FF
    }
    bool found = false;
    for (size_t i = 0; i + sizeof(body) < line.size(); ++i) {
        if (line[i] == 0xf0 && std::memcmp(&line[i + 1], body, sizeof(body)) == 0) found = true;
    }
    CHECK(found);
}

} // namespace

void run_gs_midi_sched_tests() {
    std::printf("gs: midi schedule by frames\n");
    sysex_short_goes_through();
    sysex_over_frame_moves_whole();
    sysex_longer_than_frame_survives();
    sysex_split_is_not_interleaved();
}
