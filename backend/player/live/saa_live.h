// SPDX-License-Identifier: MIT
#pragma once

// Живой SAA1099 по схеме MultiSound: записи Z80 в #1FF (номер регистра) и
// #FF (данные) играет ядро чипа платы. Записи - из общего кольца отвода
// (live_tap.h) со штампом шины, как у живого AY.
//
// Такт чипа разрешает запись 1111xxxx в #FFFD: разряд 3 в нуле - такт есть
// (#F7), в единице - нет (#FF). После сброса машины такта нет, и чип молчит,
// пока программа его не включит. Сброс машины обнуляет и регистры чипа,
// как у живого AY.

#include <cstdint>

#include "core/audio/sound_source.h"

namespace player::live {

// Настройка SAA1099: чип и его буферы берутся в начале арены трека.
void saa_live_configure(bool on);
// Флаг читает обработчик шины: без вызова, он во флеш не ходит.
extern volatile bool g_saa_live_on;
inline bool saa_live_enabled() {
    return g_saa_live_on;
}

// Машину сбросили: такт чипа снят, регистры в ноль. Зовёт основной поток Core1, исполняет
// ближайший рендер.
void saa_live_machine_reset();

// Источник микшера; пока эмуляция выключена - молчит.
soundsinth::mixbus::SoundSource* saa_live_source();

// Строка журнала: принято, потеряно на полном кольце, опоздавших.
void saa_live_log_stats();

// Голосов слышно на конец последнего рендера; выключено - 0.
uint32_t saa_live_heard();

} // namespace player::live
