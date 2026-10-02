// SPDX-License-Identifier: MIT
// Разборщик дескрипторов HID из LUFA: проверка, что перенос в проект не
// сломал его. Дескриптор здесь канонический - загрузочная мышь из
// приложения B спецификации HID, - и разложиться он обязан ровно так, как
// написано в нём самом.
//
// Смотреть в таблицу предметов разборщика тест не может и не должен:
// отбор (CALLBACK_HIDParser_FilterHIDReportItem в devices/hid) разбирает
// поле на месте и всегда отвечает "не класть", так что таблица пуста
// всегда. Проверяется то, что из дескриптора вышло - ReportMap, - и то,
// что разборщик считает мимо таблицы: размер отчёта.

#include "testing.h"

#include "devices/hid/report_map.h"

extern "C" {
#include "HIDParser.h"
}

namespace {

// clang-format off
const uint8_t kBootMouse[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Button)
    0x19, 0x01,       //     Usage Minimum (Button 1)
    0x29, 0x03,       //     Usage Maximum (Button 3)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x95, 0x03,       //     Report Count (3)
    0x75, 0x01,       //     Report Size (1)
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
    0x95, 0x01,       //     Report Count (1)
    0x75, 0x05,       //     Report Size (5)
    0x81, 0x01,       //     Input (Constant)  - добивка, отбрасывается
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x02,       //     Report Count (2)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0xC0,             //   End Collection
    0xC0              // End Collection
};
// clang-format on

HID_ReportInfo_t g_info;

// Расширение знака по ширине поля: разборщик кладёт границы сырыми
// разрядами, а поле бывает знаковым.
int32_t sign_extend(uint32_t value, uint8_t bits) {
    if (bits == 0 || bits >= 32) return static_cast<int32_t>(value);
    const uint32_t sign = 1u << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

} // namespace

void run_hid_parser_tests() {
    using namespace devices::hid;
    std::printf("hid_parser: parsing a boot mouse descriptor\n");

    ReportMap map;
    CHECK(report_map_build(kBootMouse, sizeof(kBootMouse), map));

    // Три кнопки по биту, подряд с нулевого. Номеров отчётов у этой мыши
    // нет.
    CHECK_EQ(static_cast<int>(map.button_count), 3);
    CHECK(!map.uses_report_ids);
    for (uint8_t b = 0; b < 3 && b < map.button_count; ++b) {
        CHECK_EQ(static_cast<int>(map.button[b].bit_offset), b);
        CHECK_EQ(static_cast<int>(map.button[b].report_id), 0);
    }

    // Оси идут за тремя битами кнопок и пятью битами добивки. Добивку
    // разборщик как поле выбрасывает, но место в отчёте она занимает, и
    // смещения это обязаны показывать.
    const Field& x = map.axis[static_cast<uint8_t>(Axis::X)];
    const Field& y = map.axis[static_cast<uint8_t>(Axis::Y)];
    CHECK(x.present());
    CHECK(y.present());
    CHECK_EQ(static_cast<int>(x.bit_offset), 8);
    CHECK_EQ(static_cast<int>(y.bit_offset), 16);
    CHECK_EQ(static_cast<int>(x.bit_size), 8);
    CHECK_EQ(static_cast<int>(y.bit_size), 8);

    // ВАЖНО: знак у границ разборщик не расширяет, кладёт разряды как
    // пришли - в дескрипторе минимум записан одним байтом 0x81, то есть
    // 129 как беззнаковое. Правило "минимум больше максимума - поле
    // знаковое, расширить обе границы по ширине поля" живёт в
    // report_map.cpp, и вот его итог. На нём держится нормировка осей у
    // геймпада.
    CHECK_EQ(static_cast<int>(x.logical_min), -127);
    CHECK_EQ(static_cast<int>(x.logical_max), 127);
    CHECK_EQ(static_cast<int>(sign_extend(0x81, 8)), -127);

    // Прямой вызов разборщика: таблица предметов обязана остаться пустой -
    // отбор всё разобрал на месте и ничего не принял, - а размер отчёта он
    // считает мимо неё и потому верен: три бита кнопок, пять добивки, два
    // байта осей.
    const uint8_t rc = USB_ProcessHIDReport(kBootMouse, sizeof(kBootMouse), &g_info);
    CHECK_EQ(static_cast<int>(rc), static_cast<int>(HID_PARSE_NoUnfilteredReportItems));
    CHECK_EQ(static_cast<int>(g_info.TotalReportItems), 0);
    CHECK_EQ(static_cast<int>(USB_GetHIDReportSize(&g_info, 0, HID_REPORT_ITEM_In)), 3);

    // Извлечение значения из сырого отчёта: нажата вторая кнопка,
    // X = +5, Y = -3.
    const uint8_t report[3] = {0x02, 0x05, 0xFD};
    CHECK(report_button_read(map.button[1], report, sizeof(report)));
    CHECK(!report_button_read(map.button[0], report, sizeof(report)));
    int32_t vx = 0, vy = 0;
    CHECK(report_field_read(x, report, sizeof(report), vx));
    CHECK(report_field_read(y, report, sizeof(report), vy));
    CHECK_EQ(static_cast<int>(vx), 5);
    CHECK_EQ(static_cast<int>(vy), -3);
}
