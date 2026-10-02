// SPDX-License-Identifier: MIT
#include "core/live_midi/ay_midi.h"

namespace soundsinth::midi_in {

bool MidiParser::feed(uint8_t byte, MidiEvent& out) {
    if (byte >= 0xF8u) return false; // реальное время - в любом месте потока
    sysex_ready_ = false;            // готовность живёт ровно до следующего байта

    if (byte & 0x80u) {
        if (byte == 0xF0u) {
            in_sysex_  = true;
            sysex_len_ = 0;
            status_    = 0;
            return false;
        }
        if (byte >= 0xF0u) {
            // Конец SysEx и системные общие: running status сбрасывается,
            // их данные пропускаются как байты без статуса. Оборванное
            // сообщение (новый статус вместо F7) не отдаём.
            sysex_ready_ = in_sysex_ && byte == 0xF7u;
            in_sysex_    = false;
            status_      = 0;
            return false;
        }
        in_sysex_ = false;
        status_   = byte;
        have_     = 0;
        return false;
    }

    if (in_sysex_) {
        if (sysex_len_ < kSysexKeep) sysex_[sysex_len_] = byte;
        ++sysex_len_;
        return false;
    }
    if (status_ == 0) {
        ++skipped_;
        return false;
    }
    data_[have_++] = byte;
    if (have_ < midi_data_bytes(status_)) return false;
    have_ = 0; // running status: следующие данные - то же сообщение

    out.channel = status_ & 0x0Fu;
    out.a       = data_[0];
    out.b       = data_[1];
    out.value   = 0;
    switch (status_ & 0xF0u) {
        case 0x80u:
            out.kind = MidiKind::NoteOff;
            break;
        case 0x90u:
            out.kind = data_[1] ? MidiKind::NoteOn : MidiKind::NoteOff;
            break;
        case 0xA0u:
            out.kind = MidiKind::PolyPressure;
            break;
        case 0xB0u:
            out.kind = MidiKind::Control;
            break;
        case 0xC0u:
            out.kind = MidiKind::Program;
            out.b    = 0;
            break;
        case 0xD0u:
            out.kind = MidiKind::ChannelPressure;
            out.b    = 0;
            break;
        default:
            out.kind  = MidiKind::PitchBend;
            out.value = static_cast<uint16_t>(data_[0] | (data_[1] << 7));
            break;
    }
    return true;
}

} // namespace soundsinth::midi_in
