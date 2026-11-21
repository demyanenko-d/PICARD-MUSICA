// SPDX-License-Identifier: MIT
#pragma once

// Живой AY/YM: записи Z80 в #FFFD/#BFFD играет ядро чипа платы и сводит
// в микшер по раскладке ABC. Записи - из общего кольца отвода (live_tap.h)
// со штампом шины: рендер разбирает их с постоянной задержкой, и каждая
// запись ложится на свой такт чипа. На чтение #FFFD отвечает ay_port.h.

#include <cstdint>

#include "core/audio/sound_source.h"

namespace player::live {

// Режим из настройки AY_EMULATION: 0 - выключено, 1 - AY-3-8910, 2 - YM2149,
// 3 - TurboSound (два YM2149, выбор чипа #FE/#FF), 4 - TurboSound FM (два
// YM2203); stereo - AyStereo: 0 ACB, 1 ABC, 2 моно. Включённый режим
// забирает чтение порта #FD, выключенный - отдаёт машине.
void ay_live_configure(uint8_t mode, uint32_t clock_hz, uint8_t stereo);
// Флаг читает обработчик шины: без вызова, он во флеш не ходит.
extern volatile bool g_ay_live_on;
inline bool ay_live_enabled() {
    return g_ay_live_on;
}

// Машину сбросили: вход сброса AY стоит на линии RESET, регистры чипов в
// ноль, и звук смолкает. Записи до сброса из кольца выбрасываются. Зовёт
// основной поток Core1, исполняет ближайший рендер.
void ay_live_machine_reset();

// Источник микшера; пока эмуляция выключена - молчит.
soundsinth::mixbus::SoundSource* ay_live_source();

// Строка журнала: принято, потеряно на полном кольце, опоздавших.
void ay_live_log_stats();

// Голосов слышно на конец последнего рендера: каналы SSG с уровнем не ноль
// и каналы FM с несущим громче -48 дБ; выключено - 0.
uint32_t ay_live_heard();

} // namespace player::live
