// SPDX-License-Identifier: MIT
#pragma once

// Расписание MIDI-файла по кадрам для залитого плеером проигрывателя.
//
// В этом режиме карта General Sound работает разборщиком: плеер отдаёт ей
// файл целиком, а потом раз в кадр просит очередную порцию MIDI-байт и сам
// выталкивает их в линию с порта A звукогенератора. Эти байты приходят на
// отвод AY и играют живым MIDI.
//
// Всё, что нужно знать о файле, плеер подставляет в тело блоба перед
// заливкой: разрешение, смещение первой дорожки, их число и длину. Заголовок
// MThd поэтому не разбирается.
//
// Память: файл лежит в области GS, рабочие таблицы - там же, в её хвосте.
// В SRAM не уходит ничего.

#include <cstdint>

#include "devices/gs/gs_device.h"

namespace player::gs {

// Сколько дорожек разбираем. Плеер обрезает их число до 64, и список имён
// у него ровно на 64 записи.
inline constexpr uint8_t kMidiTracksMax = 64;
inline constexpr uint8_t kMidiNameChars = 24;
// Очередь байт у карты - 256 байт; больше за кадр она не отдаёт.
inline constexpr uint16_t kMidiFrameMax = 256;
// Но за кадр отдаём не больше 51 байта данных. У плеера на кадр заведён
// счётчик в 52 байта (#BFCC); досчитав до нуля, он ставит (#DFFF)=5, и
// после этого блок состояния больше не запрашивается - телеметрия замирает,
// а с ней и признак конца песни. Байт #FF счётчика не расходует, поэтому
// порция из 51 байта плюс #FF оставляет плеера в полном порядке. Не
// влезшее уходит в следующий кадр.
inline constexpr uint16_t kMidiFrameData = 51;

// Блок состояния #01: 135 байт, и плеер раскладывает их по своим адресам
// (разбор #EC6A).
//
//   0..14   телеметрия MIDI-режима, #BFCD..#BFDB
//   15      (#BD2A)
//   16      (#BD29)
//   17,18   слово (#EF1A), старшим байтом вперёд
//   19,20   слово (#EF24), старшим байтом вперёд
//   21      (#E129)  <- операнд "продолжать играть"
//   22      (#EE14)
//   23      (#EE15)
//   24..98  #FFB4..#FFFE, складываются по "или"
//   99..134 #BFDC..#BFFF
//
// Байт 21 - операнд команды `ld a,nn / and a / jp nz,<цикл кадра>`: ноль
// там значит "кончили", и плеер уходит на следующий трек.
inline constexpr uint16_t kMidiStatePlaying = 21;
// Положение в тактах, которое плеер печатает шестью знаками (#EE38): 24
// бита из двух слов карты, в блоке старшим байтом вперёд.
//
//   17 - старший байт первого слова, у блоба он всегда ноль
//   18 - младший байт первого слова: биты 23..16 положения
//   19 - старший байт второго слова: биты 15..8
//   20 - младший байт второго слова: биты 7..0
inline constexpr uint16_t kMidiStatePos = 17;
// Байты 22 и 23 у блоба - постоянные #78 и #00, их же плеер кладёт себе
// при старте; блок отдаёт те же значения.
inline constexpr uint16_t kMidiStateConst = 22;

// Картинка нот: три ряда по 25 байт, байты 24..98 блока.
//
//   ряд 0 - подсветка самих клавиш, по три пикселя на звучащую;
//   ряд 1 - полоски белых клавиш, ряд 2 - чёрных. Плеер рисует их
//           исключающим ИЛИ и каждый кадр сдвигает вниз на строку
//           пикселей - отсюда и бегущие полоски.
//
// Клавиша - это нота минус 24 (так считает и карта: #5496 режет ноты ниже
// 24 и выше 109). Октава занимает ровно 28 пикселей, полутона внутри неё
// стоят по kKeyPixel; каждая клавиша светит тремя пикселями. Геометрия
// выведена из рисунка плеера.
// Сколько кадров горит строка дорожки после взятой ноты.
inline constexpr uint8_t kMidiBlinkFrames = 3;
// Столбики нот над клавиатурой: 36 штук по три пикселя, байты 99..134
// блока. Разбор #53E8 в теле блоба: столбик выбирается нотой ПО МОДУЛЮ 36,
// а значение - это скорость, делённая пополам. Плеер (#8C11) делит ещё раз
// и получает высоту. Карта чистит их при чтении, то есть столбик живёт
// один кадр.
inline constexpr uint16_t kMidiStateBars   = 99;
inline constexpr uint8_t kMidiBars         = 36;
inline constexpr uint16_t kMidiStateKeys   = 24;
inline constexpr uint16_t kMidiKeyRow      = 25; // байт в ряду
inline constexpr uint8_t kMidiKeysMax      = 85; // столько клавиш у рисунка
inline constexpr uint8_t kMidiKeyLowest    = 24; // нота самой левой клавиши
inline constexpr uint8_t kMidiOctavePx     = 28;
inline constexpr uint8_t kMidiKeyPixel[12] = {0, 3, 5, 7, 10, 13, 15, 17, 19, 21, 23, 25};
// Чёрные клавиши октавы: их полоска идёт вторым рядом.
inline constexpr bool kMidiKeyBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
// Кадр Спектрума: расписание блоба идёт по прерыванию 50 Гц.
inline constexpr uint32_t kMidiFrameUs = 20000;

// Раскладка рабочей области. Лежит в хвосте области GS: файл .mid у плеера
// не длиннее 64 КБ (поле длины в блобе шестнадцатиразрядное), и с ним она
// не пересекается.
inline constexpr uint32_t kMidiNamesAt = 0;
inline constexpr uint32_t kMidiInfoAt  = kMidiNamesAt + devices::gs::kMidiNamesBytes;
// Блока состояния здесь нет: он живёт в SRAM у автомата, потому что первые
// его байты собираются в прерывании по аргументу команды.
inline constexpr uint32_t kMidiFrameAt = kMidiInfoAt + devices::gs::kMidiInfoBytes;
// Порций две, и они чередуются: пока плеер дочитывает одну, следующая
// собирается в другую. Одной мало: плеер читает порцию побайтно в своём
// темпе, и конец чтения не известен.
inline constexpr uint8_t kMidiFrameSlots = 2;
inline constexpr uint32_t kMidiWorkSize  = kMidiFrameAt + kMidiFrameMax * kMidiFrameSlots;

class MidiSched {
public:
    // Разобрать файл. file - начало .mid в памяти, work - рабочая область
    // не меньше kMidiWorkSize. false - разбирать нечего.
    // state - буфер блока состояния на 135 байт, он у автомата в SRAM.
    bool begin(const uint8_t* file, uint32_t len, const devices::gs::MidiFileFacts& facts, uint8_t* work, uint8_t* state);

    const uint8_t* names() const { return work_ + kMidiNamesAt; }
    const uint8_t* info() const { return work_ + kMidiInfoAt; }
    const uint8_t* state() const { return state_; }
    // Признаки "дорожка только что играла": автомат собирает из них строки
    // видимого окна списка.
    const uint8_t* track_flags() const { return blink_; }

    // Собрать порцию очередного кадра в рабочую область. Возвращает её
    // длину вместе с замыкающим #FF.
    uint16_t next_frame();
    // Последняя собранная порция. Зовётся после next_frame().
    const uint8_t* frame() const { return work_ + kMidiFrameAt + static_cast<uint32_t>(frame_slot_) * kMidiFrameMax; }

    bool ready() const { return ready_; }
    bool finished() const { return finished_; }

    // Диагностика.
    uint8_t track_count() const { return track_count_; }
    uint32_t events() const { return events_; }
    uint32_t bytes_out() const { return bytes_out_; }
    // Сколько байт пришлось пропустить в поисках заголовков дорожек и где
    // разбор остановился: по ним видно, что файл не такой, как ждали.
    uint32_t skipped() const { return skipped_; }
    uint32_t stopped_at() const { return stopped_at_; }
    uint32_t tempo_us() const { return tempo_us_; }
    // Положение в тактах - то, что плеер показывает строкой Ticks.
    uint32_t position_ticks() const { return static_cast<uint32_t>(now_q16_ >> 16); }
    // Длина всей песни в тактах - её плеер печатает строкой Total и по ней
    // же рисует ход.
    uint32_t total_ticks() const { return total_ticks_; }

private:
    struct Track {
        uint32_t pos    = 0; // где читать следующее событие
        uint32_t end    = 0;
        uint32_t tick   = 0; // абсолютный такт следующего события
        uint8_t running = 0;
        bool done       = true;
    };

    // Переменная длина MIDI. false - вышли за дорожку.
    bool read_varlen(uint32_t& pos, uint32_t end, uint32_t& out) const;
    // Поставить дорожку на следующее событие, прочитав дельту.
    void arm(Track& t);
    // Выдать событие дорожки в порцию. Возвращает false, если не поместилось.
    bool emit(Track& t, uint16_t& n);
    void scan_name(const Track& t, uint8_t* dst) const;
    // Нота взята или снята: держим счёт по клавишам, как это делает карта.
    void key_change(uint8_t note, bool on);
    // Собрать картинку нот в блок состояния.
    void draw_keys();
    // Длина песни: проход по всем дорожкам со счётом задержек. Один раз на
    // файл, своими указателями - рабочие курсоры трогать нельзя.
    uint32_t scan_total_ticks();

    const uint8_t* file_ = nullptr;
    uint32_t len_        = 0;
    uint8_t* work_       = nullptr;
    uint8_t* state_      = nullptr;
    // Сколько кадров строка дорожки ещё горит. Карта так же гасит свои
    // счётчики по одному за кадр.
    uint8_t blink_[kMidiTracksMax] = {};

    Track tracks_[kMidiTracksMax] = {};
    uint8_t track_count_          = 0;
    uint16_t division_            = 0;
    uint32_t tempo_us_            = 500000; // умолчание MIDI: 120 долей в минуту
    // Положение в тактах, Q16. Считается не накоплением приращения, а от
    // опоры: такт кадра - дробь, и накопленное усечение уводило бы события
    // на кадр назад. Опора переставляется на смене темпа, между сменами
    // счёт идёт от неё в микросекундах.
    uint64_t tick_base_q16_ = 0;
    uint64_t us_since_base_ = 0;
    uint64_t now_q16_       = 0;
    uint8_t frame_slot_     = 0;
    // Дамп, не влезший в кадр целиком, продолжается в следующем. Плеер
    // только выталкивает байты в линию, поэтому разрез получателю не
    // виден. Пара на всё расписание, а не поле дорожки: кадр обрывается на
    // первом же непоместившемся событии, значит недоданным может быть
    // только один дамп. Отдаётся он прежде выбора дорожки по такту - иначе
    // чужие байты встанут в середину.
    uint8_t sysex_track_ = kMidiTracksMax;
    uint32_t sysex_left_ = 0;
    bool ready_          = false;
    bool finished_       = true;
    uint32_t events_     = 0;
    uint32_t bytes_out_  = 0;
    // Сколько голосов держит каждую клавишу: нота бывает взята на
    // нескольких каналах разом, и снятие одной гасило бы чужую.
    uint8_t keys_[kMidiKeysMax] = {};
    // Столбики за кадр: собираются по взятым нотам и гаснут отдачей.
    uint8_t bars_[kMidiBars] = {};
    uint32_t skipped_        = 0;
    uint32_t stopped_at_     = 0;
    uint32_t total_ticks_    = 0;
};

} // namespace player::gs
