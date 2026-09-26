#pragma once

// Живой MIDI с порта A AY: разбор записей в порты AY, последовательного
// потока 8N1 и сообщений MIDI.
//
// Spectrum 128 выводит MIDI программно: бит 2 регистра 14 AY (порт A) -
// линия передачи. Регистр выбирается записью в #FFFD, значение пишется в
// #BFFD. ПЗУ (PLAY) пишет порт на каждый бит, $FA - линия в 0, $FE - в 1,
// 31388 бод (~113 тактов Z80 на бит). Кадр: старт-бит 0, восемь бит
// младшим вперёд, стоп-бит 1.
//
// Разбор идёт по последовательности записей, без времени: одна запись -
// один бит. Так пишут ПЗУ и программы, которые выводят каждый бит; линия в
// покое (единица) между кадрами записей не требует.

#include <cstdint>

namespace soundsinth::midi_in {

// --- Последовательный поток 8N1 по одному биту на запись ---

class SerialBits {
public:
    // Бит линии. true - получен байт (в out).
    bool feed(bool line, uint8_t& out) {
        if (count_ == 0) {
            // Покой: единицы пропускаются, ноль - старт-бит.
            if (!line) count_ = 1;
            return false;
        }
        if (count_ <= 8) {
            shift_ = static_cast<uint8_t>((shift_ >> 1) | (line ? 0x80u : 0u));
            ++count_;
            return false;
        }
        // Стоп-бит.
        count_ = 0;
        if (!line) {
            // Нет стоп-бита - кадр испорчен. Этот ноль может быть старт-битом
            // следующего, но надёжнее ждать покоя.
            ++framing_errors_;
            return false;
        }
        out = shift_;
        return true;
    }

    uint32_t framing_errors() const { return framing_errors_; }

private:
    uint8_t count_ = 0; // 0 - покой, 1..8 - бит данных, 9 - стоп-бит
    uint8_t shift_ = 0;
    uint32_t framing_errors_ = 0;
};

// --- Порты AY: выбор регистра и данные порта A ---

// Запись в порт AY, как её кладёт плата: kSelect - запись в #FFFD (выбор
// регистра), иначе в #BFFD (данные), младший байт - значение.
inline constexpr uint16_t kAyWriteSelect = 0x100u;
inline constexpr uint8_t kAyRegPortA = 14;
inline constexpr uint8_t kMidiOutBit = 1u << 2;

class AyPortA {
public:
    // Запись в порт AY. true - бит линии MIDI (в line).
    bool feed(uint16_t write, bool& line) {
        const auto value = static_cast<uint8_t>(write & 0xFFu);
        if (write & kAyWriteSelect) {
            reg_ = value;
            return false;
        }
        if (reg_ != kAyRegPortA) return false;
        line = (value & kMidiOutBit) != 0;
        return true;
    }

private:
    uint8_t reg_ = 0xFF;
};

// --- Сообщения MIDI ---

enum class MidiKind : uint8_t {
    NoteOff,
    NoteOn,       // скорость 0 превращается в NoteOff
    PolyPressure,
    Control,
    Program,
    ChannelPressure,
    PitchBend,    // value - 14 бит, 8192 - середина
};

struct MidiEvent {
    MidiKind kind;
    uint8_t channel; // 0..15
    uint8_t a;       // нота, контроллер, программа, давление
    uint8_t b;       // скорость, значение контроллера
    uint16_t value;  // PitchBend
};

// Сколько байт тела SysEx держим: столько читают разборы ударного канала и
// общей громкости, остальное сообщение проходит мимо. Сверка с ними - там,
// где видны обе стороны.
inline constexpr uint32_t kSysexKeep = 8;

// Разбор байтов в сообщения каналов. Running status, реального времени
// (0xF8..0xFF) внутри сообщения не мешает, системные общие пропускаются.
// Тело SysEx собирается началом: по нему узнают ударный канал и общую
// громкость.
class MidiParser {
public:
    // true - сообщение канала готово (в out).
    bool feed(uint8_t byte, MidiEvent& out);

    // Сообщение SysEx дошло до конца - его начало в sysex(), полная длина в
    // sysex_len(). Верно до следующего feed.
    bool sysex_ready() const { return sysex_ready_; }
    const uint8_t* sysex() const { return sysex_; }
    uint32_t sysex_len() const { return sysex_len_; }

    uint32_t skipped_bytes() const { return skipped_; }

private:
    uint8_t status_ = 0;  // running status, 0 - нет
    uint8_t data_[2] = {};
    uint8_t have_ = 0;
    bool in_sysex_ = false;
    bool sysex_ready_ = false;
    uint8_t sysex_[kSysexKeep] = {};
    uint32_t sysex_len_ = 0; // всего байт тела, а не сохранённых
    uint32_t skipped_ = 0;
};

// Длина данных сообщения канала по старшему полубайту статуса.
constexpr uint8_t midi_data_bytes(uint8_t status) {
    const uint8_t hi = status & 0xF0u;
    return (hi == 0xC0u || hi == 0xD0u) ? 1u : 2u;
}

// Сообщение обратно в байты канала: статус со старшим полубайтом и каналом,
// два байта данных. Питч-бенд - младшие семь бит, потом старшие.
inline void midi_event_bytes(const MidiEvent& e, uint8_t& status, uint8_t& d1, uint8_t& d2) {
    static constexpr uint8_t kHi[] = {0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0};
    status = static_cast<uint8_t>(kHi[static_cast<uint8_t>(e.kind)] | e.channel);
    if (e.kind == MidiKind::PitchBend) {
        d1 = static_cast<uint8_t>(e.value & 0x7fu);
        d2 = static_cast<uint8_t>((e.value >> 7) & 0x7fu);
    } else {
        d1 = e.a;
        d2 = e.b;
    }
}

// Весь путь: записи в порты AY -> сообщения MIDI.
class AyMidiInput {
public:
    // true - сообщение канала готово (в out).
    bool feed(uint16_t ay_write, MidiEvent& out) {
        sysex_ready_ = false; // готовность - про эту запись, а не про прошлую
        bool line = false;
        if (!port_.feed(ay_write, line)) return false;
        uint8_t byte = 0;
        if (!bits_.feed(line, byte)) return false;
        ++bytes_;
        const bool got = parser_.feed(byte, out);
        sysex_ready_ = parser_.sysex_ready();
        return got;
    }

    // Этой записью дошло до конца сообщение SysEx.
    bool sysex_ready() const { return sysex_ready_; }
    const uint8_t* sysex() const { return parser_.sysex(); }
    uint32_t sysex_len() const { return parser_.sysex_len(); }

    uint32_t bytes() const { return bytes_; }
    uint32_t framing_errors() const { return bits_.framing_errors(); }
    uint32_t skipped_bytes() const { return parser_.skipped_bytes(); }

private:
    AyPortA port_;
    SerialBits bits_;
    MidiParser parser_;
    bool sysex_ready_ = false;
    uint32_t bytes_ = 0;
};

} // namespace soundsinth::midi_in
