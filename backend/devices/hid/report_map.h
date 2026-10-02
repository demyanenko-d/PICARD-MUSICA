// SPDX-License-Identifier: MIT
#pragma once

// Что нашлось в дескрипторе отчётов устройства: где лежат оси, шляпка и
// кнопки.
//
// Разбор идёт один раз, при подключении, разборщиком LUFA. Его таблица
// предметов занимает пару килобайт и нужна только на это время; отсюда
// уносится выжимка - десятки байт, по которым отчёт разбирается дальше
// без всякого разборщика.
//
// Поле ищется по назначению (usage), а не по смещению в байтах, поэтому
// работает и незнакомый геймпад.

#include <cstdint>

namespace devices::hid {

inline constexpr uint8_t kMaxButtons = 16;

// Оси в порядке назначений Generic Desktop 0x30..0x38.
enum class Axis : uint8_t { X, Y, Z, Rx, Ry, Rz, Slider, Dial, Wheel, Count };

inline constexpr uint8_t kAxisCount = static_cast<uint8_t>(Axis::Count);

// Поле отчёта. Пустое, когда bit_size == 0.
struct Field {
    uint8_t report_id   = 0; // 0 - отчёты этого устройства без номеров
    uint8_t bit_size    = 0;
    uint16_t bit_offset = 0;
    int32_t logical_min = 0; // знак уже расширен
    int32_t logical_max = 0;

    bool present() const { return bit_size != 0; }
};

// Кнопка: у неё всегда один разряд и границы 0..1, хранить остальное незачем.
struct ButtonBit {
    uint8_t report_id   = 0;
    uint16_t bit_offset = 0;
};

inline constexpr uint8_t kMaxModifiers = 8;

// Клавиатура устроена иначе, чем кнопки: модификаторы - разряды с
// назначениями 0xE0..0xE7, а коды нажатых клавиш лежат массивом. Слот
// массива несёт не признак, а сам код клавиши, поэтому назначения его
// полей смысла не имеют - важны только размер, смещение и число слотов.
struct KeyboardMap {
    ButtonBit modifier[kMaxModifiers];
    uint8_t modifier_usage[kMaxModifiers] = {}; // 0xe0..0xe7
    uint8_t modifier_count                = 0;

    uint8_t report_id        = 0;
    uint16_t slot_bit_offset = 0;
    uint8_t slot_bit_size    = 0;
    uint8_t slot_count       = 0;

    bool present() const { return slot_count != 0; }
};

struct ReportMap {
    KeyboardMap keyboard;

    Field axis[kAxisCount];
    Field hat;
    ButtonBit button[kMaxButtons];
    uint8_t button_count = 0;
    bool uses_report_ids = false;

    bool has_axes() const;
};

// Разобрать дескриптор. false - пригодных полей не нашлось или разборщик
// отказал. Не из прерывания: проход по дескриптору это микросекунды, но
// таблица предметов большая.
bool report_map_build(const uint8_t* desc, uint16_t len, ReportMap& out);

// Код отказа разборщика от последнего report_map_build (HID_PARSE_* из
// lib/hidparser). Ноль - разобрано; отказ по потолку предметов или номеров
// отчёта лечится иначе, чем незнакомая страница назначений.
uint8_t report_map_last_error();

// Карта загрузочной мыши: кнопки разрядами 0..2, затем X и Y по байту со
// знаком. Нужна, когда дескриптор не разобрался, - раскладка задана
// спецификацией и одинакова у всех.
void report_map_boot_mouse(ReportMap& out);

// Достать поле из сырого отчёта. false - отчёт не того номера, короче
// нужного или поля нет. Знак расширяется по ширине поля.
bool report_field_read(const Field& f, const uint8_t* report, uint16_t len, int32_t& out);

// Нажата ли кнопка. Вне отчёта или не тот номер - не нажата.
bool report_button_read(const ButtonBit& b, const uint8_t* report, uint16_t len);

// Код клавиши из слота массива. false - слота нет, отчёт не тот или
// короче нужного. Ноль в out значит "слот пуст".
bool report_key_read(const KeyboardMap& k, uint8_t slot, const uint8_t* report, uint16_t len, uint8_t& out);

} // namespace devices::hid
