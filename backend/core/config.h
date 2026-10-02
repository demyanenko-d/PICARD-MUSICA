// SPDX-License-Identifier: MIT
#pragma once

// Настраиваемые компромиссы точности, памяти и процессора. Исключение -
// параметры ревербератора, кроме SOUNDSINTH_REVERB_WET_Q15: они константы
// ревербератора, подобраны по эталону вместе.
//
// Значения под #ifndef переопределяются через -D, как PICO_CONFIG в SDK.
// Размеры массивов и раскладка памяти (слоты, голоса, буферы вывода, пулы,
// зоны PSRAM, стек рендера) - без #ifndef: -D только на одну цель дал бы
// разный sizeof в разных единицах трансляции без предупреждения. Сейчас -D
// на них даёт "redefined", и побеждает config.h.

// --- Память ---

// Банк с SD-карты.
//
// Если в корне карты лежит SOUNDSINTH_BANK_SD_PATH, банк берётся с карты,
// иначе из флеша. Читается тем же путём, каким сэмплы попадают в PSRAM:
// блоки распаковываются частями, целиком банк нигде не лежит.
//
// Таблицам банка с карты нужен произвольный доступ: они лежат в PSRAM, и
// хранилище трека ужимается на них, только когда банк с карты взят.

// PSRAM под таблицы банка (пресеты, инструменты, огибающие, keymap,
// сэмплы - всё, кроме PCM). GeneralUser GS - 598.7 КБ, Timbres of Heaven
// - 599.7, SGM - 486.5. Не влезли - банк с карты не берётся, в журнал
// идёт строка, играет банк из флеша.
//
// 640, а не 768: при 768 зоне сэмплов не хватает 24 КБ на The HYBRID
// Collage, и трек не грузится.
#define SOUNDSINTH_BANK_SD_TABLE_BYTES (640u * 1024u)

// --- Голоса ---

// Слоты 0..channel_count-1 - живые каналы, за ними - фоновые голоса NNA.
// Звучит не больше SOUNDSINTH_MAX_VOICES: живые каналы первыми, фоновые
// добирают остаток. Лишние 32 слота - около 10 КБ статики.
#define SOUNDSINTH_MAX_SLOTS  96u // записей состояния: 64 канала и фоновые голоса NNA
#define SOUNDSINTH_MAX_VOICES 64u // физических голосов одновременно

// Слотов под фоновые голоса NNA (IT и .mid); звучат в пределах
// SOUNDSINTH_MAX_VOICES. SOUNDSINTH_MAX_SLOTS не меньше суммы с
// SOUNDSINTH_MAX_VOICES - проверяет движок.
#define SOUNDSINTH_MAX_NNA_VOICES 32u

// Антиклик: за сколько выходных отсчётов громкость голоса доходит до
// нового значения; без него ступенька на границе тика щёлкает на каждой
// строке. 44 - 1 мс при 44100 (.mid задаёт своё); 0 выключает.
#ifndef SOUNDSINTH_VOLUME_RAMP_SAMPLES
#define SOUNDSINTH_VOLUME_RAMP_SAMPLES 44u
#endif

// --- Сброс голосов при перегрузке ---

// Сброс лишних голосов при перегрузке процессора. Если рендер не успевает,
// DMA повторяет прошлый буфер и заикается вся музыка - на filt_ace-light.it
// до 30 с за трек. Уронить несколько тихих голосов заметно меньше.
// На PC по умолчанию выключен: выход тестов и pc_player воспроизводим.
#ifndef SOUNDSINTH_VOICE_CULL_ON_OVERLOAD
#define SOUNDSINTH_VOICE_CULL_ON_OVERLOAD 1
#endif

// Пороги с гистерезисом, в процентах загрузки рендера. 95: при 100% кольцо
// и два заряженных канала DMA дают 17-23 мс нагнать, при 90 голоса режутся
// зря. 70: при 85 бюджет всё время растёт к 64 и снова режет.
#ifndef SOUNDSINTH_VOICE_CULL_HIGH_PCT
#define SOUNDSINTH_VOICE_CULL_HIGH_PCT 95u
#endif
#ifndef SOUNDSINTH_VOICE_CULL_LOW_PCT
#define SOUNDSINTH_VOICE_CULL_LOW_PCT 70u
#endif
// Нижняя граница бюджета - защита от ошибочного замера, который загасил
// бы музыку целиком.
#ifndef SOUNDSINTH_VOICE_CULL_MIN_VOICES
#define SOUNDSINTH_VOICE_CULL_MIN_VOICES 8u
#endif

// Голос живого канала считается "тихим" (и потому расходным), если он
// тише этой доли самого громкого живого голоса. Мелодия и бас держатся у
// максимума, подкладки и эхо - заметно ниже.
#ifndef SOUNDSINTH_VOICE_CULL_QUIET_NUM
#define SOUNDSINTH_VOICE_CULL_QUIET_NUM 1u
#endif
#ifndef SOUNDSINTH_VOICE_CULL_QUIET_DEN
#define SOUNDSINTH_VOICE_CULL_QUIET_DEN 4u
#endif
// Спуск на голос за тик, подъём на голос за столько тиков (32 - 0.64 с).
// При быстром подъёме бюджет мечется 38-64-38 и каналы мерцают - на слух
// хуже ровно уменьшенной полифонии.
#ifndef SOUNDSINTH_VOICE_CULL_RISE_TICKS
#define SOUNDSINTH_VOICE_CULL_RISE_TICKS 32u
#endif

// Спуск: голос на столько процентов загрузки сверх верхнего порога. Голос с
// фильтром стоит около 413 нс при отсчёте 22.7 мкс - 1.8% загрузки, отсюда 2.
// Потолок шага - чтобы всплеск замера не срезал полифонию целиком.
#ifndef SOUNDSINTH_VOICE_CULL_STEP_PCT
#define SOUNDSINTH_VOICE_CULL_STEP_PCT 2u
#endif
#ifndef SOUNDSINTH_VOICE_CULL_MAX_STEP
#define SOUNDSINTH_VOICE_CULL_MAX_STEP 8u
#endif

// Цена голоса в наносекундах, замерено на RP2350 при 300 МГц. BASE - всё,
// кроме распаковки; DECODE - один вызов декодера Dpcm8 (84 такта); FILTER -
// резонансный фильтр.
//
// Жертва при перегрузке выбирается по цене, а не только по громкости: Raw8
// без фильтра стоит 372 нс, Dpcm8 на шаге 3 с фильтром - 1090, почти втрое
// больше, а самый тихий голос обычно самый дешёвый.
//
// У Dpcm8 распаковка последовательная: step вызовов на выходной отсчёт,
// цена растёт с высотой. Raw8 и Raw16 прыгают за постоянное время.
#ifndef SOUNDSINTH_VOICE_COST_BASE_NS
#define SOUNDSINTH_VOICE_COST_BASE_NS 91u
#endif
// За один вызов, у Dpcm8 умножается на step.
#ifndef SOUNDSINTH_VOICE_COST_DECODE_NS
#define SOUNDSINTH_VOICE_COST_DECODE_NS 281u
#endif
// Raw8/Raw16: один прыжок, от step не зависит.
#ifndef SOUNDSINTH_VOICE_COST_JUMP_NS
#define SOUNDSINTH_VOICE_COST_JUMP_NS 281u
#endif
#ifndef SOUNDSINTH_VOICE_COST_FILTER_NS
#define SOUNDSINTH_VOICE_COST_FILTER_NS 156u
#endif

// --- Вывод звука: пул буферов ---
#define SOUNDSINTH_AUDIO_BUFFER_FRAMES 256u // размер одного буфера, стерео-фреймов
// --- Только .mid ---

// На сколько выше основного тона держать срез фильтра у .mid, в единицах
// шкалы IT (24 на октаву). 0 - ровно основной тон, мало: остаётся синус без
// второй гармоники.
#ifndef SOUNDSINTH_MIDI_FILTER_NOTE_MARGIN
#define SOUNDSINTH_MIDI_FILTER_NOTE_MARGIN 24
#endif

// Лимитер и компрессор. Включает формат флагом Song::limiter_enabled;
// сейчас ни один: .mid играет как эталон, перегруз мягко насыщается,
// трекерный уровень задан файлом.

// Потолок лимитера в отсчётах. Ровно шкала: лимитер срабатывает только
// там, где иначе была бы полка. При выключенном компрессоре неклипующие
// рендеры остаются побайтово прежними - это проверяется тестом.
#ifndef SOUNDSINTH_MIDI_LIMITER_CEILING
#define SOUNDSINTH_MIDI_LIMITER_CEILING 32767
#endif

// Восстановление усиления: за кадр добирается 1/2^N недостающего. 11 при
// 44.1 кГц - постоянная времени около 46 мс. Быстрее - искажение на басу,
// медленнее - проседание после удара.
#ifndef SOUNDSINTH_MIDI_LIMITER_RELEASE_SHIFT
#define SOUNDSINTH_MIDI_LIMITER_RELEASE_SHIFT 11
#endif

// Компрессор - ступень лимитера перед потолком, без лимитера не работает.
// Порог 90% и наклон 16:1: крутой наклон при высоком пороге трогает только
// макушки. Опущенный потолок лимитера полку не убирает, а переносит ниже.
#ifndef SOUNDSINTH_MIDI_COMPRESSOR_THRESHOLD
#define SOUNDSINTH_MIDI_COMPRESSOR_THRESHOLD 29491
#endif
// Доля ужатия в Q15. Наклон: выход/порог = a + (1-a)*вход/порог, то есть
// степень сжатия равна 1/(1-a). 30720/32768 = 0.9375 - это 16:1.
#ifndef SOUNDSINTH_MIDI_COMPRESSOR_AMOUNT
#define SOUNDSINTH_MIDI_COMPRESSOR_AMOUNT 30720
#endif

// --- Ревербератор ---

// Громкость возврата общего ревербератора, Q15. Посыл задаёт канал через
// CC91, это множитель поверх него.
//
// Подобрано по эталонному рендеру: в паузе хора Bohemian Rhapsody возврат
// у эталона -35.9 дБ, у нас -35.4. Связан с делителем входа (kInputShift) и
// сдвигом выхода (kOutputShift) ревербератора: при смене любого уровень
// подбирается заново.
#ifndef SOUNDSINTH_REVERB_WET_Q15
#define SOUNDSINTH_REVERB_WET_Q15 15200
#endif

// --- Живой MIDI ---

// Фора события: столько миллисекунд оно ждёт в очереди, прежде чем
// прозвучать. За это время строится инструмент и подгружается PCM его
// сэмплов из флеша (типичный сэмпл банка - 2 мс, самый большой - 13).
// Задержка живого режима = фора плюс тик.
#ifndef SOUNDSINTH_LIVE_MIDI_LOOKAHEAD_MS
#define SOUNDSINTH_LIVE_MIDI_LOOKAHEAD_MS 100u
#endif

// Темп живой песни: строка = тик, 250 даёт ровно 10 мс на тик.
#ifndef SOUNDSINTH_LIVE_MIDI_TEMPO
#define SOUNDSINTH_LIVE_MIDI_TEMPO 250u
#endif
