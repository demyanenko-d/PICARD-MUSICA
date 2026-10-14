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

// Мышь с осями по 12 разрядов: минимум -2047 записан двумя байтами, то
// есть ширина данных предмета (16) не равна ширине поля (12).
// clang-format off
const uint8_t kMouse12[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x16, 0x01, 0xF8, //     Logical Minimum (-2047)
    0x26, 0xFF, 0x07, //     Logical Maximum (2047)
    0x75, 0x0C,       //     Report Size (12)
    0x95, 0x02,       //     Report Count (2)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0xC0,             //   End Collection
    0xC0              // End Collection
};

// Ось 16 разрядов с границами одним байтом: -1..1.
const uint8_t kAxis16[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x04,       // Usage (Joystick)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x30,       //   Usage (X)
    0x15, 0xFF,       //   Logical Minimum (-1)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x10,       //   Report Size (16)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0xC0              // End Collection
};

// 0..255 одним байтом: максимум 0xFF при неотрицательном минимуме -
// беззнаковое 255, а не -1. Так пишут многие геймпады.
const uint8_t kAxisU8[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x04,       // Usage (Joystick)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x30,       //   Usage (X)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0xFF,       //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0xC0              // End Collection
};
// clang-format on

HID_ReportInfo_t g_info;

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

    // Минимум записан одним байтом 0x81: со знаком по размеру данных
    // предмета это -127. На границах держится нормировка осей геймпада.
    CHECK_EQ(static_cast<int>(x.logical_min), -127);
    CHECK_EQ(static_cast<int>(x.logical_max), 127);

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

    std::printf("hid_parser: logical bounds signed by the item data size\n");
    ReportMap m12;
    CHECK(report_map_build(kMouse12, sizeof(kMouse12), m12));
    const Field& x12 = m12.axis[static_cast<uint8_t>(Axis::X)];
    CHECK(x12.present());
    CHECK_EQ(static_cast<int>(x12.logical_min), -2047);
    CHECK_EQ(static_cast<int>(x12.logical_max), 2047);
    const uint8_t r12[3] = {0xFF, 0xFF, 0xFF}; // X = -1, Y = -1
    int32_t v12          = 0;
    CHECK(report_field_read(x12, r12, sizeof(r12), v12));
    CHECK_EQ(static_cast<int>(v12), -1);

    ReportMap m16;
    CHECK(report_map_build(kAxis16, sizeof(kAxis16), m16));
    const Field& x16 = m16.axis[static_cast<uint8_t>(Axis::X)];
    CHECK(x16.present());
    CHECK_EQ(static_cast<int>(x16.logical_min), -1);
    CHECK_EQ(static_cast<int>(x16.logical_max), 1);

    ReportMap mu8;
    CHECK(report_map_build(kAxisU8, sizeof(kAxisU8), mu8));
    const Field& xu8 = mu8.axis[static_cast<uint8_t>(Axis::X)];
    CHECK(xu8.present());
    CHECK_EQ(static_cast<int>(xu8.logical_min), 0);
    CHECK_EQ(static_cast<int>(xu8.logical_max), 255);
}
