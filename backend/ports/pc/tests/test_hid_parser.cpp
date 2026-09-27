// Разборщик дескрипторов HID из LUFA: проверка, что перенос в проект не
// сломал его. Дескриптор здесь канонический - загрузочная мышь из
// приложения B спецификации HID, - и разложиться он обязан ровно так, как
// написано в нём самом.
//
// Отбор полей пока живёт тут же: LUFA спрашивает приложение, какие
// предметы оставлять. Когда появится слой опознания устройств, отбор
// переедет к нему, а этот тест станет им пользоваться.

#include "testing.h"

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

// Сколько байт занимает поле в отчёте с этим номером.
constexpr uint16_t kUsagePageDesktop = 0x01;
constexpr uint16_t kUsagePageButton = 0x09;
constexpr uint16_t kUsageX = 0x30;
constexpr uint16_t kUsageY = 0x31;

// Расширение знака по ширине поля: разборщик кладёт границы сырыми
// разрядами, а поле бывает знаковым.
int32_t sign_extend(uint32_t value, uint8_t bits) {
    if (bits == 0 || bits >= 32) return static_cast<int32_t>(value);
    const uint32_t sign = 1u << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

const HID_ReportItem_t* find(uint16_t page, uint16_t usage) {
    for (uint8_t i = 0; i < g_info.TotalReportItems; ++i) {
        const HID_ReportItem_t& it = g_info.ReportItems[i];
        if (it.Attributes.Usage.Page == page && it.Attributes.Usage.Usage == usage) return &it;
    }
    return nullptr;
}

} // namespace

// Отбор полей живёт в devices/hid/report_map.cpp - один на всю
// программу. Значит тест проверяет и его: мимо фильтра проходят оси,
// шляпка, кнопки и клавиши, постоянные поля LUFA отбрасывает сам.

void run_hid_parser_tests() {
    std::printf("hid_parser: разбор дескриптора загрузочной мыши\n");

    const uint8_t rc = USB_ProcessHIDReport(kBootMouse, sizeof(kBootMouse), &g_info);
    CHECK_EQ(static_cast<int>(rc), static_cast<int>(HID_PARSE_Successful));

    // Три кнопки по биту и две оси по байту. Добивка в пять бит выброшена
    // как постоянное поле, но место в отчёте занимает.
    CHECK_EQ(static_cast<int>(g_info.TotalReportItems), 5);

    for (uint8_t b = 1; b <= 3; ++b) {
        const HID_ReportItem_t* it = find(kUsagePageButton, b);
        CHECK(it != nullptr);
        if (it == nullptr) continue;
        CHECK_EQ(static_cast<int>(it->Attributes.BitSize), 1);
        CHECK_EQ(static_cast<int>(it->BitOffset), b - 1);
        CHECK_EQ(static_cast<int>(it->Attributes.Logical.Maximum), 1);
        CHECK_EQ(static_cast<int>(it->ItemType), static_cast<int>(HID_REPORT_ITEM_In));
        CHECK_EQ(static_cast<int>(it->ReportID), 0); // номеров отчётов у этой мыши нет
    }

    const HID_ReportItem_t* x = find(kUsagePageDesktop, kUsageX);
    const HID_ReportItem_t* y = find(kUsagePageDesktop, kUsageY);
    CHECK(x != nullptr);
    CHECK(y != nullptr);
    if (x != nullptr && y != nullptr) {
        // Оси идут за тремя битами кнопок и пятью битами добивки.
        CHECK_EQ(static_cast<int>(x->BitOffset), 8);
        CHECK_EQ(static_cast<int>(y->BitOffset), 16);
        CHECK_EQ(static_cast<int>(x->Attributes.BitSize), 8);
        CHECK_EQ(static_cast<int>(y->Attributes.BitSize), 8);
        // ВАЖНО: знак у границ разборщик не расширяет, кладёт разряды как
        // пришли - `Logical.Minimum = ReportItemData`. В дескрипторе
        // минимум записан одним байтом 0x81, и здесь он и лежит как 129, а
        // не как -127.
        CHECK_EQ(static_cast<int>(x->Attributes.Logical.Minimum), 0x81);
        CHECK_EQ(static_cast<int>(x->Attributes.Logical.Maximum), 127);

        // Отсюда правило для слоя выше: если минимум больше максимума как
        // беззнаковые, то поле знаковое, и обе границы надо расширить по
        // ширине самого поля. На этом держится нормировка осей у геймпада.
        CHECK(x->Attributes.Logical.Minimum > x->Attributes.Logical.Maximum);
        const int32_t lo = sign_extend(x->Attributes.Logical.Minimum, x->Attributes.BitSize);
        const int32_t hi = sign_extend(x->Attributes.Logical.Maximum, x->Attributes.BitSize);
        CHECK_EQ(static_cast<int>(lo), -127);
        CHECK_EQ(static_cast<int>(hi), 127);
    }

    // Размер отчёта целиком: три бита кнопок, пять добивки, два байта осей.
    CHECK_EQ(static_cast<int>(USB_GetHIDReportSize(&g_info, 0, HID_REPORT_ITEM_In)), 3);

    // Извлечение значения из сырого отчёта: нажата вторая кнопка,
    // X = +5, Y = -3.
    const uint8_t report[3] = {0x02, 0x05, 0xFD};
    const HID_ReportItem_t* b2 = find(kUsagePageButton, 2);
    CHECK(b2 != nullptr);
    if (b2 != nullptr) {
        HID_ReportItem_t item = *b2;
        CHECK(USB_GetHIDReportItemInfo(report, &item));
        CHECK_EQ(static_cast<int>(item.Value), 1);
    }
    if (x != nullptr && y != nullptr) {
        HID_ReportItem_t ix = *x;
        HID_ReportItem_t iy = *y;
        CHECK(USB_GetHIDReportItemInfo(report, &ix));
        CHECK(USB_GetHIDReportItemInfo(report, &iy));
        CHECK_EQ(static_cast<int>(ix.Value), 5);
        // Значение приходит без знакового расширения: восемь бит как есть.
        CHECK_EQ(static_cast<int>(iy.Value), 0xFD);
        CHECK_EQ(static_cast<int>(HID_ALIGN_DATA((&iy), int8_t)), -3);
    }
}
