// SPDX-License-Identifier: MIT
#include "devices/hid/report_map.h"

extern "C" {
#include "HIDParser.h"
}

namespace devices::hid {
namespace {

// Страницы назначений, которые нас касаются.
constexpr uint16_t kPageDesktop  = 0x01;
constexpr uint16_t kPageKeyboard = 0x07;
constexpr uint16_t kPageButton   = 0x09;

// Назначения Generic Desktop.
constexpr uint16_t kUsageX     = 0x30;
constexpr uint16_t kUsageWheel = 0x38; // колесо мыши - последняя ось
constexpr uint16_t kUsageHat   = 0x39;

// Индекс оси - назначение минус kUsageX: перечисление Axis повторяет
// порядок назначений 0x30..0x38.
static_assert(static_cast<uint16_t>(Axis::Wheel) == kUsageWheel - kUsageX, "Axis follows Generic Desktop usages 0x30..0x38");
static_assert(static_cast<uint16_t>(Axis::Count) == kUsageWheel - kUsageX + 1u, "Axis ends at the wheel");
constexpr Axis axis_of_usage(uint16_t usage) {
    return static_cast<Axis>(usage - kUsageX);
}

// Таблица предметов разборщика. Одна на всех: устройства подключаются по
// очереди, из цикла Core1.
//
// Места в ней на один предмет: отбор разбирает предмет на лету и в таблицу
// не кладёт, потолок таблицы не достигается при любом числе кнопок.
HID_ReportInfo_t s_parsed;

// Карта, которую наполняет отбор. Живёт только на время одного
// report_map_build; разбор идёт из цикла Core1, вторым его не начать.
ReportMap* s_out = nullptr;

// Код отказа разборщика LUFA от последнего разбора: по одному false потолок
// номеров отчёта (8) не отличить от незнакомой страницы назначений.
uint8_t s_last_error = 0;

// Знак значения поля - по его ширине: в отчёте поле лежит своими разрядами.
int32_t sign_extend(uint32_t value, uint8_t bits) {
    if (bits == 0 || bits >= 32) return static_cast<int32_t>(value);
    const uint32_t sign = 1u << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

// Знак у границ разборщик расширяет сам, по размеру данных предмета: у
// поля в 12 разрядов минимум -2047 приходит двумя байтами 01 F8, и
// расширение по ширине поля дало бы 59393.
void fill(Field& f, const HID_ReportItem_t& it) {
    f.report_id   = it.ReportID;
    f.bit_size    = it.Attributes.BitSize;
    f.bit_offset  = it.BitOffset;
    f.logical_min = static_cast<int32_t>(it.Attributes.Logical.Minimum);
    f.logical_max = static_cast<int32_t>(it.Attributes.Logical.Maximum);
}

// Разряды поля из отчёта, младшим вперёд, без расширения знака.
//
// Байтами, а не по разряду: поле до 32 разрядов с произвольного смещения
// лежит не более чем в пяти байтах. Побитовый сбор стоил около восьми
// команд на разряд, то есть 128 команд на ось в 16 разрядов, и это на
// каждый отчёт мыши - у игровой до тысячи в секунду.
uint32_t raw_bits(const uint8_t* report, uint16_t len, uint16_t bit_offset, uint8_t bit_size) {
    if (bit_size == 0 || bit_size > 32) return 0;
    const uint16_t first = static_cast<uint16_t>(bit_offset >> 3);
    const uint8_t shift  = static_cast<uint8_t>(bit_offset & 7u);
    // Байт за концом отчёта считается нулевым - как у прежнего сбора,
    // который на нём обрывался.
    uint64_t word       = 0;
    const uint16_t need = static_cast<uint16_t>((shift + bit_size + 7u) / 8u); // не больше пяти
    for (uint16_t k = 0; k < need; ++k) {
        const uint16_t byte = static_cast<uint16_t>(first + k);
        if (byte >= len) break;
        word |= static_cast<uint64_t>(report[byte]) << (k * 8u);
    }
    word >>= shift;
    if (bit_size == 32) return static_cast<uint32_t>(word);
    return static_cast<uint32_t>(word) & ((1u << bit_size) - 1u);
}

// Смещение начала данных: у устройства с номерами отчётов первый байт -
// номер, и поля считаются от второго.
bool locate(uint8_t report_id, const uint8_t*& report, uint16_t& len) {
    if (report_id == 0) return true;
    if (len == 0 || report[0] != report_id) return false;
    ++report;
    --len;
    return true;
}

} // namespace

bool ReportMap::has_axes() const {
    for (const Field& f : axis) {
        if (f.present()) return true;
    }
    return hat.present();
}

// Один предмет дескриптора в карту. Зовётся из отбора разборщика, по
// предмету за раз - порядок тот же, что у прежнего прохода по таблице.
namespace {
void consider_item(const HID_ReportItem_t& it) {
    if (s_out == nullptr) return;
    ReportMap& out = *s_out;
    // Только то, что устройство присылает само.
    if (it.ItemType != HID_REPORT_ITEM_In) return;
    if (it.ReportID != 0) out.uses_report_ids = true;

    const uint16_t page  = it.Attributes.Usage.Page;
    const uint16_t usage = it.Attributes.Usage.Usage;

    if (page == kPageButton) {
        if (out.button_count >= kMaxButtons) return;
        ButtonBit& b = out.button[out.button_count++];
        b.report_id  = it.ReportID;
        b.bit_offset = it.BitOffset;
        return;
    }
    if (page == kPageKeyboard) {
        KeyboardMap& k = out.keyboard;
        if ((it.ItemFlags & HID_IOF_VARIABLE) != 0) {
            // Разряд: это модификатор, его назначение 0xE0..0xE7.
            if (k.modifier_count >= kMaxModifiers) return;
            k.modifier[k.modifier_count].report_id  = it.ReportID;
            k.modifier[k.modifier_count].bit_offset = it.BitOffset;
            k.modifier_usage[k.modifier_count]      = static_cast<uint8_t>(usage);
            ++k.modifier_count;
            return;
        }
        // Массив: слоты идут подряд, разборщик отдаёт их по одному.
        // Назначения слотов не значат ничего - код клавиши приходит
        // значением, а не номером поля.
        if (k.slot_count == 0) {
            k.report_id       = it.ReportID;
            k.slot_bit_offset = it.BitOffset;
            k.slot_bit_size   = it.Attributes.BitSize;
        }
        ++k.slot_count;
        return;
    }
    if (page != kPageDesktop) return;
    if (usage == kUsageHat) {
        if (!out.hat.present()) fill(out.hat, it);
        return;
    }
    if (usage < kUsageX || usage > kUsageWheel) return;
    Field& f = out.field(axis_of_usage(usage));
    // Первое вхождение и выигрывает: у геймпадов ту же ось иногда
    // объявляют второй раз в другом отчёте.
    if (!f.present()) fill(f, it);
}
} // namespace

bool report_map_build(const uint8_t* desc, uint16_t len, ReportMap& out) {
    out = ReportMap{};
    if (desc == nullptr || len == 0) return false;
    s_out = &out;
    // Отбор всегда отвечает "не класть", поэтому принятых предметов ноль и
    // разборщик честно сообщает об этом отдельным кодом. Дескриптор при
    // этом разобран целиком; годность решается своей проверкой ниже.
    const uint8_t rc = USB_ProcessHIDReport(desc, len, &s_parsed);
    s_out            = nullptr;
    s_last_error     = rc;
    if (rc != HID_PARSE_Successful && rc != HID_PARSE_NoUnfilteredReportItems) return false;

    return out.has_axes() || out.button_count != 0 || out.keyboard.present() || out.keyboard.modifier_count != 0;
}

uint8_t report_map_last_error() {
    return s_last_error;
}

void report_map_boot_mouse(ReportMap& out) {
    out              = ReportMap{};
    out.button_count = 3;
    for (uint8_t i = 0; i < 3; ++i) {
        out.button[i].bit_offset = i;
    }
    Field& x      = out.field(Axis::X);
    Field& y      = out.field(Axis::Y);
    x.bit_size    = 8;
    x.bit_offset  = 8;
    x.logical_min = -127;
    x.logical_max = 127;
    y             = x;
    y.bit_offset  = 16;
}

bool report_field_read(const Field& f, const uint8_t* report, uint16_t len, int32_t& out) {
    if (!f.present() || report == nullptr) return false;
    if (!locate(f.report_id, report, len)) return false;
    const uint16_t need = static_cast<uint16_t>((f.bit_offset + f.bit_size + 7u) / 8u);
    if (need > len) return false;
    const uint32_t v = raw_bits(report, len, f.bit_offset, f.bit_size);
    out              = f.logical_min < 0 ? sign_extend(v, f.bit_size) : static_cast<int32_t>(v);
    return true;
}

bool report_key_read(const KeyboardMap& k, uint8_t slot, const uint8_t* report, uint16_t len, uint8_t& out) {
    if (slot >= k.slot_count || k.slot_bit_size == 0 || report == nullptr) return false;
    if (!locate(k.report_id, report, len)) return false;
    const uint16_t bit  = static_cast<uint16_t>(k.slot_bit_offset + slot * k.slot_bit_size);
    const uint16_t need = static_cast<uint16_t>((bit + k.slot_bit_size + 7u) / 8u);
    if (need > len) return false;
    out = static_cast<uint8_t>(raw_bits(report, len, bit, k.slot_bit_size));
    return true;
}

bool report_button_read(const ButtonBit& b, const uint8_t* report, uint16_t len) {
    if (report == nullptr) return false;
    if (!locate(b.report_id, report, len)) return false;
    const uint16_t byte = static_cast<uint16_t>(b.bit_offset >> 3);
    if (byte >= len) return false;
    return ((report[byte] >> (b.bit_offset & 7)) & 1u) != 0;
}

} // namespace devices::hid

// Отбор предметов для разборщика LUFA: он спрашивает про каждое поле,
// класть ли его в таблицу. Берём оси, шляпку, кнопки и клавиши - всё
// прочее выбрасывается на лету. Постоянные поля (добивку) разборщик
// отбрасывает сам, до этого вызова.
//
// Отобранное разбирается прямо здесь и кладётся в карту, а в таблицу
// разборщика не кладётся никогда - ответ всегда "не класть". Предмет к
// этому моменту заполнен целиком, включая смещение в отчёте и номер
// отчёта, так что промежуточное хранилище не нужно.
extern "C" bool CALLBACK_HIDParser_FilterHIDReportItem(HID_ReportItem_t* const item) {
    const uint16_t page  = item->Attributes.Usage.Page;
    const uint16_t usage = item->Attributes.Usage.Usage;
    using devices::hid::kPageButton;
    using devices::hid::kPageDesktop;
    using devices::hid::kPageKeyboard;
    using devices::hid::kUsageHat;
    using devices::hid::kUsageWheel;
    using devices::hid::kUsageX;
    const bool ours = page == kPageButton || page == kPageKeyboard ||                                               // кнопки и клавиши
                      (page == kPageDesktop && ((usage >= kUsageX && usage <= kUsageWheel) || usage == kUsageHat)); // оси, колесо, шляпка
    if (ours) devices::hid::consider_item(*item);
    return false;
}
