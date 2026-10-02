// SPDX-License-Identifier: MIT
#pragma once

// SysEx живого потока: ударность каналов и общая громкость.
//
// Разбор - тот же, что у загрузчика .mid. В очередь едут служебные события,
// а не готовое решение: живую песню трогает одна сторона, читатель, и
// порядок с нотами сохраняется - канал успевает стать ударным до своей ноты.

#include <cstdint>

#include "core/formats/midi_convert.h"
#include "core/formats/midi_sysex.h"
#include "core/live_midi/ay_midi.h"
#include "core/live_midi/live_stream.h"

namespace soundsinth::midi_in {

static_assert(kSysexKeep >= formats::midi::kSysexPrefix, "SysEx parsing needs more body bytes than the stream holds");

class SysexTracker {
public:
    // Умолчание GM: ударный только десятый канал - с него же начинает живая
    // песня, поэтому в очередь пойдёт лишь то, что объявит поток.
    void begin() {
        for (uint32_t c = 0; c < 16; ++c) {
            drums_[c] = (c == 9);
        }
        drum_changes_   = 0;
        volume_changes_ = 0;
    }

    // Тело сообщения (без F0 и F7); len - полная длина, даже если сохранено
    // меньше.
    void apply(const uint8_t* body, uint32_t len, uint32_t at_ms, LiveStream& stream) {
        bool next[16];
        for (uint32_t c = 0; c < 16; ++c) {
            next[c] = drums_[c];
        }
        formats::midi::sysex_drum_channels(body, len, next);
        for (uint8_t c = 0; c < 16; ++c) {
            if (next[c] == drums_[c]) continue;
            drums_[c] = next[c];
            stream.push(at_ms, formats::midi::kDrumChannelStatus, c, next[c] ? 1u : 0u);
            ++drum_changes_;
        }
        uint8_t vol = 0;
        if (formats::midi::sysex_master_volume(body, len, vol)) {
            stream.push(at_ms, formats::midi::kMasterVolumeStatus, vol, 0);
            ++volume_changes_;
        }
    }

    bool drum_channel(uint8_t channel) const { return channel < 16 && drums_[channel]; }
    uint32_t drum_changes() const { return drum_changes_; }
    uint32_t volume_changes() const { return volume_changes_; }

private:
    bool drums_[16]          = {};
    uint32_t drum_changes_   = 0;
    uint32_t volume_changes_ = 0;
};

} // namespace soundsinth::midi_in
