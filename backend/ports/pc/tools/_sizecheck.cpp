// SPDX-License-Identifier: MIT
// Размеры структур, от которых зависят решения по памяти на плате.
//
// Нужен потому, что прикидки "байт двести" на этом проекте уже подводили:
// массивы движка умножаются на число голосов, и ошибка в полтора раза
// съедает всю свободную SRAM.
#include <cstdio>

#include "core/bank/bank_format.h"
#include "core/engine/tracker_engine.h"
#include "core/engine/voice.h"
#include "core/model/instrument.h"
#include "core/formats/midi_convert.h"

int main() {
    using namespace soundsinth;
    std::printf("%-28s %zu\n", "BankInstrument", sizeof(bank::BankInstrument));
    std::printf("%-28s %zu\n", "Instrument", sizeof(soundsinth::model::Instrument));
    std::printf("%-28s %zu\n", "ChannelState", sizeof(engine::ChannelState));
    std::printf("%-28s %zu\n", "Voice", sizeof(engine::Voice));
    std::printf("%-28s %zu\n", "TrackerEngine", sizeof(engine::TrackerEngine));
    std::printf("%-28s %zu\n", "midi::Converter", sizeof(formats::midi::Converter));
    return 0;
}
