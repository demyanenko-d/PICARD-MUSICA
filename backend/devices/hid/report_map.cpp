#include "devices/hid/report_map.h"

extern "C" {
#include "HIDParser.h"
}

namespace devices::hid {
namespace {

// Страницы назначений, которые нас касаются.
constexpr uint16_t kPageDesktop = 0x01;
constexpr uint16_t kPageKeyboard = 0x07;
constexpr uint16_t kPageButton = 0x09;

// Назначения Generic Desktop.
constexpr uint16_t kUsageX = 0x30;
constexpr uint16_t kUsageWheel = 0x38; // колесо мыши - последняя ось
constexpr uint16_t kUsageHat = 0x39;

// Таблица предметов нужна только на время разбора, поэтому она одна на
// всех и переиспользуется: устройства подключаются по очереди, из цикла
// Core1. Две с лишним тысячи байт держать по одной на интерфейс незачем.
HID_ReportInfo_t s_parsed;

// Знак у границ разборщик не расширяет - кладёт разряды как пришли. Поле
// знаковое, когда минимум больше максимума как беззнаковые.
int32_t sign_extend(uint32_t value, uint8_t bits) {
    if (bits == 0 || bits >= 32) return static_cast<int32_t>(value);
    const uint32_t sign = 1u << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

void fill(Field& f, const HID_ReportItem_t& it) {
    f.report_id = it.ReportID;
    f.bit_size = it.Attributes.BitSize;
    f.bit_offset = it.BitOffset;
    const uint32_t lo = it.Attributes.Logical.Minimum;
    const uint32_t hi = it.Attributes.Logical.Maximum;
    if (lo > hi) {
        f.logical_min = sign_extend(lo, it.Attributes.BitSize);
        f.logical_max = sign_extend(hi, it.Attributes.BitSize);
    } else {
        f.logical_min = static_cast<int32_t>(lo);
        f.logical_max = static_cast<int32_t>(hi);
    }
}

// Разряды поля из отчёта, младшим вперёд, без расширения знака.
uint32_t raw_bits(const uint8_t* report, uint16_t len, uint16_t bit_offset, uint8_t bit_size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < bit_size; ++i) {
        const uint16_t bit = static_cast<uint16_t>(bit_offset + i);
        const uint16_t byte = static_cast<uint16_t>(bit >> 3);
        if (byte >= len) break;
        if ((report[byte] >> (bit & 7)) & 1u) v |= (1u << i);
    }
    return v;
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

bool report_map_build(const uint8_t* desc, uint16_t len, ReportMap& out) {
    out = ReportMap{};
    if (desc == nullptr || len == 0) return false;
    if (USB_ProcessHIDReport(desc, len, &s_parsed) != HID_PARSE_Successful) return false;

    for (uint8_t i = 0; i < s_parsed.TotalReportItems; ++i) {
        const HID_ReportItem_t& it = s_parsed.ReportItems[i];
        // Только то, что устройство присылает само.
        if (it.ItemType != HID_REPORT_ITEM_In) continue;
        if (it.ReportID != 0) out.uses_report_ids = true;

        const uint16_t page = it.Attributes.Usage.Page;
        const uint16_t usage = it.Attributes.Usage.Usage;

        if (page == kPageButton) {
            if (out.button_count >= kMaxButtons) continue;
            ButtonBit& b = out.button[out.button_count++];
            b.report_id = it.ReportID;
            b.bit_offset = it.BitOffset;
            continue;
        }
        if (page == kPageKeyboard) {
            KeyboardMap& k = out.keyboard;
            if ((it.ItemFlags & HID_IOF_VARIABLE) != 0) {
                // Разряд: это модификатор, его назначение 0xE0..0xE7.
                if (k.modifier_count >= kMaxModifiers) continue;
                k.modifier[k.modifier_count].report_id = it.ReportID;
                k.modifier[k.modifier_count].bit_offset = it.BitOffset;
                k.modifier_usage[k.modifier_count] = static_cast<uint8_t>(usage);
                ++k.modifier_count;
                continue;
            }
            // Массив: слоты идут подряд, разборщик отдаёт их по одному.
            // Назначения слотов не значат ничего - код клавиши приходит
            // значением, а не номером поля.
            if (k.slot_count == 0) {
                k.report_id = it.ReportID;
                k.slot_bit_offset = it.BitOffset;
                k.slot_bit_size = it.Attributes.BitSize;
            }
            ++k.slot_count;
            continue;
        }
        if (page != kPageDesktop) continue;
        if (usage == kUsageHat) {
            if (!out.hat.present()) fill(out.hat, it);
            continue;
        }
        if (usage < kUsageX || usage > kUsageWheel) continue;
        Field& f = out.axis[usage - kUsageX];
        // Первое вхождение и выигрывает: у пультов ту же ось иногда
        // объявляют второй раз в другом отчёте.
        if (!f.present()) fill(f, it);
    }

    return out.has_axes() || out.button_count != 0 || out.keyboard.present() || out.keyboard.modifier_count != 0;
}

void report_map_boot_mouse(ReportMap& out) {
    out = ReportMap{};
    out.button_count = 3;
    for (uint8_t i = 0; i < 3; ++i) out.button[i].bit_offset = i;
    Field& x = out.axis[static_cast<uint8_t>(Axis::X)];
    Field& y = out.axis[static_cast<uint8_t>(Axis::Y)];
    x.bit_size = 8;
    x.bit_offset = 8;
    x.logical_min = -127;
    x.logical_max = 127;
    y = x;
    y.bit_offset = 16;
}

bool report_field_read(const Field& f, const uint8_t* report, uint16_t len, int32_t& out) {
    if (!f.present() || report == nullptr) return false;
    if (!locate(f.report_id, report, len)) return false;
    const uint16_t need = static_cast<uint16_t>((f.bit_offset + f.bit_size + 7u) / 8u);
    if (need > len) return false;
    const uint32_t v = raw_bits(report, len, f.bit_offset, f.bit_size);
    out = f.logical_min < 0 ? sign_extend(v, f.bit_size) : static_cast<int32_t>(v);
    return true;
}

bool report_key_read(const KeyboardMap& k, uint8_t slot, const uint8_t* report, uint16_t len, uint8_t& out) {
    if (slot >= k.slot_count || k.slot_bit_size == 0 || report == nullptr) return false;
    if (!locate(k.report_id, report, len)) return false;
    const uint16_t bit = static_cast<uint16_t>(k.slot_bit_offset + slot * k.slot_bit_size);
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
// прочее выбрасывается на лету, и таблица остаётся маленькой. Постоянные
// поля (добивку) разборщик отбрасывает сам.
extern "C" bool CALLBACK_HIDParser_FilterHIDReportItem(HID_ReportItem_t* const item) {
    const uint16_t page = item->Attributes.Usage.Page;
    if (page == 0x09 || page == 0x07) return true; // кнопки и клавиши
    if (page != 0x01) return false;                // не Generic Desktop
    const uint16_t usage = item->Attributes.Usage.Usage;
    return (usage >= 0x30 && usage <= 0x38) || usage == 0x39; // оси, колесо и шляпка
}
