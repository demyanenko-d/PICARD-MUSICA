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

// Разряды поля из отчёта, младшим вперёд, без расширения знака. Длину
// отчёта проверяет вызывающий: байты поля лежат в нём целиком.
//
// Байтами, а не по разряду: отчёт мыши приходит до тысячи раз в секунду,
// а поле до 32 разрядов с произвольного смещения лежит не более чем в
// пяти байтах.
uint32_t raw_bits(const uint8_t* report, uint16_t bit_offset, uint8_t bit_size) {
    if (bit_size == 0 || bit_size > 32) return 0;
    const uint8_t* const first = report + (bit_offset >> 3);
    const uint8_t shift        = static_cast<uint8_t>(bit_offset & 7u);
    uint64_t word              = 0;
    const uint16_t need        = static_cast<uint16_t>((shift + bit_size + 7u) / 8u); // не больше пяти
    for (uint16_t k = 0; k < need; ++k) {
        word |= static_cast<uint64_t>(first[k]) << (k * 8u);
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

// Один предмет дескриптора в карту. Зовётся из отбора разборщика, по
// предмету за раз. Берутся оси, шляпка, кнопки и клавиши, всё прочее
// выбрасывается; номер отчёта отмечается только у взятых.
void consider_item(ReportMap& out, const HID_ReportItem_t& it) {
    // Только то, что устройство присылает само.
    if (it.ItemType != HID_REPORT_ITEM_In) return;
    const uint16_t page  = it.Attributes.Usage.Page;
    const uint16_t usage = it.Attributes.Usage.Usage;
    const bool desktop   = page == kPageDesktop && ((usage >= kUsageX && usage <= kUsageWheel) || usage == kUsageHat);
    if (page != kPageButton && page != kPageKeyboard && !desktop) return;
    if (it.ReportID != 0) out.uses_report_ids = true;

    if (page == kPageButton) {
        // Только разряд с назначением кнопки: поле-массив несёт номер
        // кнопки значением, а поле без назначения (добивка с флагом Data)
        // в покое стоит единицей - FIRE был бы зажат всегда.
        if ((it.ItemFlags & HID_IOF_VARIABLE) == 0 || usage == 0 || usage > kMaxButtons) return;
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
    if (usage == kUsageHat) {
        if (!out.hat.present()) fill(out.hat, it);
        return;
    }
    Field& f = out.field(axis_of_usage(usage));
    // Первое вхождение и выигрывает: у геймпадов ту же ось иногда
    // объявляют второй раз в другом отчёте.
    if (!f.present()) fill(f, it);
}

} // namespace

bool ReportMap::has_axes() const {
    for (const Field& f : axis) {
        if (f.present()) return true;
    }
    return hat.present();
}

bool report_map_build(const uint8_t* desc, uint16_t len, ReportMap& out) {
    out = ReportMap{};
    if (desc == nullptr || len == 0) return false;
    s_out = &out;
    // Таблица предметов разборщика - на стеке: нужна только здесь. Места в
    // ней на один предмет: отбор разбирает предмет на лету и в таблицу не
    // кладёт, потолок таблицы не достигается при любом числе кнопок.
    HID_ReportInfo_t parsed;
    // Отбор всегда отвечает "не класть", поэтому принятых предметов ноль и
    // разборщик честно сообщает об этом отдельным кодом. Дескриптор при
    // этом разобран целиком; годность решается своей проверкой ниже.
    const uint8_t rc = USB_ProcessHIDReport(desc, len, &parsed);
    s_out            = nullptr;
    s_last_error     = rc;
    if (rc != HID_PARSE_Successful && rc != HID_PARSE_NoUnfilteredReportItems) return false;

    return out.has_axes() || out.button_count != 0 || out.keyboard.present() || out.keyboard.modifier_count != 0;
}

uint8_t report_map_last_error() {
    return s_last_error;
}

uint16_t report_map_bytes(const ReportMap& map) {
    uint32_t bits  = 0;
    const auto end = [&bits](uint32_t e) {
        if (e > bits) bits = e;
    };
    for (const Field& f : map.axis) {
        if (f.present()) end(f.bit_offset + f.bit_size);
    }
    if (map.hat.present()) end(map.hat.bit_offset + map.hat.bit_size);
    for (uint8_t i = 0; i < map.button_count; ++i) {
        end(map.button[i].bit_offset + 1u);
    }
    const KeyboardMap& k = map.keyboard;
    if (k.present()) end(k.slot_bit_offset + static_cast<uint32_t>(k.slot_count) * k.slot_bit_size);
    for (uint8_t i = 0; i < k.modifier_count; ++i) {
        end(k.modifier[i].bit_offset + 1u);
    }
    if (bits == 0) return 0;
    return static_cast<uint16_t>((bits + 7u) / 8u + (map.uses_report_ids ? 1u : 0u));
}

// Короткие предметы до первой коллекции: страница и назначение у неё и
// есть верхнее назначение устройства. Длинные предметы (0xFE) в
// дескрипторах мышей и пультов не встречаются - на них разбор кончается.
bool report_top_usage(const uint8_t* desc, uint16_t len, uint16_t& page, uint16_t& usage) {
    constexpr uint8_t kLongItem      = 0xfe;
    constexpr uint8_t kTagUsagePage  = 0x04; // тип Global
    constexpr uint8_t kTagUsage      = 0x08; // тип Local
    constexpr uint8_t kTagCollection = 0xa0; // тип Main
    page                             = 0;
    usage                            = 0;
    for (uint16_t i = 0; desc != nullptr && i < len;) {
        const uint8_t head = desc[i];
        if (head == kLongItem) return false;
        const uint8_t size = static_cast<uint8_t>((head & 3u) == 3u ? 4u : (head & 3u));
        if (i + 1u + size > len) return false;
        uint32_t v = 0;
        for (uint8_t k = 0; k < size; ++k) {
            v |= static_cast<uint32_t>(desc[i + 1u + k]) << (8u * k);
        }
        const uint8_t tag = static_cast<uint8_t>(head & 0xfcu);
        if (tag == kTagCollection) return page != 0 || usage != 0;
        if (tag == kTagUsagePage) page = static_cast<uint16_t>(v);
        if (tag == kTagUsage) {
            // Назначение в четыре байта несёт страницу в старшей половине.
            if (size == 4) page = static_cast<uint16_t>(v >> 16);
            usage = static_cast<uint16_t>(v);
        }
        i = static_cast<uint16_t>(i + 1u + size);
    }
    return false;
}

bool report_field_read(const Field& f, const uint8_t* report, uint16_t len, int32_t& out) {
    if (!f.present() || report == nullptr) return false;
    if (!locate(f.report_id, report, len)) return false;
    const uint16_t need = static_cast<uint16_t>((f.bit_offset + f.bit_size + 7u) / 8u);
    if (need > len) return false;
    const uint32_t v = raw_bits(report, f.bit_offset, f.bit_size);
    out              = f.logical_min < 0 ? sign_extend(v, f.bit_size) : static_cast<int32_t>(v);
    return true;
}

bool report_key_read(const KeyboardMap& k, uint8_t slot, const uint8_t* report, uint16_t len, uint8_t& out) {
    if (slot >= k.slot_count || k.slot_bit_size == 0 || report == nullptr) return false;
    if (!locate(k.report_id, report, len)) return false;
    const uint16_t bit  = static_cast<uint16_t>(k.slot_bit_offset + slot * k.slot_bit_size);
    const uint16_t need = static_cast<uint16_t>((bit + k.slot_bit_size + 7u) / 8u);
    if (need > len) return false;
    out = static_cast<uint8_t>(raw_bits(report, bit, k.slot_bit_size));
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
// класть ли его в таблицу. Постоянные поля (добивку) разборщик отбрасывает
// сам, до этого вызова.
//
// Поле разбирается прямо здесь и кладётся в карту, а в таблицу разборщика
// не кладётся никогда - ответ всегда "не класть". Предмет к этому моменту
// заполнен целиком, включая смещение в отчёте и номер отчёта, так что
// промежуточное хранилище не нужно.
extern "C" bool CALLBACK_HIDParser_FilterHIDReportItem(HID_ReportItem_t* const item) {
    if (devices::hid::s_out != nullptr) devices::hid::consider_item(*devices::hid::s_out, *item);
    return false;
}
