// SPDX-License-Identifier: MIT
#pragma once

// Ключи сеанса и звуковой цепочки: буферы вывода, задачи, подкачка .mid.
//
// Отдельно от core/config.h: ядро не читает ни одного из них. Значения
// под #ifndef переопределяются через -D.

#include <cstdint> // static_assert ниже считает в числах ядра

#include "core/config.h" // SOUNDSINTH_AUDIO_BUFFER_FRAMES для проверки пула

// --- Задачи ---

// Стек фоновой render-задачи. Замер платы на играющем треке: тронуто
// 1064 Б. Глубже всего цепочка сведения: VoiceMixer::mix 656 плюс кадры
// вокруг. Остаток печатается на сносе трека, по нему и подгонять.
#ifndef SOUNDSINTH_RENDER_TASK_STACK_WORDS
#define SOUNDSINTH_RENDER_TASK_STACK_WORDS 1024u
#endif
// Стек задачи тика. Тик не сводит звук и не держит буферов: разбор строки,
// эффекты, арбитр и команды голосам - глубина вызовов невелика. Замер
// платы: тронуто 1176 Б; живой MIDI кладёт сверху около 0.3 КБ.
#ifndef SOUNDSINTH_SEQUENCER_TASK_STACK_WORDS
#define SOUNDSINTH_SEQUENCER_TASK_STACK_WORDS 768u
#endif

// Приоритет задачи рендера (player::audio::RenderTask) относительно
// IDLE (platform::os_task_create).
#ifndef SOUNDSINTH_RENDER_TASK_PRIORITY
#define SOUNDSINTH_RENDER_TASK_PRIORITY 2u
#endif

// --- Пул буферов вывода ---

// Буферов в пуле ровно столько, сколько их бывает вне очереди свободных:
// два у вывода (играет и заряжен), очередь готовых и тот, что пишет
// рендер, - формула в static_assert ниже. Лишний в обороте не участвует
// никогда. Задержка звука - весь пул без играющего (5 - около 23 мс).
// Длина буфера - у ядра: по ней считает и микшер.
//
// Появится потребитель, держащий больше одного буфера (второй синк живого
// MIDI), - поднять и его: иначе рендер встанет в begin_write.
#ifndef SOUNDSINTH_AUDIO_BUFFER_COUNT
#define SOUNDSINTH_AUDIO_BUFFER_COUNT 5u
#endif
// Сколько отрендеренных буферов можно накопить впрок. Это и есть запас на
// всплеск: end_write ждёт места в очереди готовых, поэтому рендер опережает
// вывод ровно на её глубину, сколько бы буферов ни было в пуле.
// Живой MIDI: отрисовка буфера доходит до 7.8 мс при длине буфера 5.8 -
// всплеск тика не укладывается, и при глубине 1 это провал вывода.
#ifndef SOUNDSINTH_RENDERED_QUEUE_DEPTH
#define SOUNDSINTH_RENDERED_QUEUE_DEPTH 2u
#endif
static_assert(SOUNDSINTH_AUDIO_BUFFER_COUNT >= SOUNDSINTH_RENDERED_QUEUE_DEPTH + 3u, "pool: two at the output, the ready queue and the render buffer");

// --- .mid ---

// Сколько секунд звука у .mid загружать до старта. Звук стартует раньше,
// чем приехали все сэмплы, остальное грузится фоном; нота, чей сэмпл ещё не
// загружен, не звучит. Граница "первые два паттерна" у .mid бывает короче
// секунды - длину паттерна задают сетка и темп, - поэтому мера во времени.
// 0 - два паттерна.
//
// Цена - задержка перед первой нотой, заметная с банком на карте.
#ifndef SOUNDSINTH_MIDI_PREFETCH_SECONDS
#define SOUNDSINTH_MIDI_PREFETCH_SECONDS 10
#endif

// Тишина, после которой живой режим выключается: либо пауза между пьесами,
// либо со Спектрума запустили другое приложение.
#ifndef SOUNDSINTH_LIVE_MIDI_TIMEOUT_MS
#define SOUNDSINTH_LIVE_MIDI_TIMEOUT_MS 5000u
#endif
