// SPDX-License-Identifier: MIT
#include "core/config/config_page.h"

#include <cstring>

#include "core/config/config_fields.h"

namespace soundsinth::config {

namespace {

static_assert(kPageFieldCount <= kConfigMaskBytes * 8u, "the availability mask has a bit per field");
static_assert(static_cast<uint32_t>(Kind::Key) < kKindCount, "a choice list per kind");

// Положить строку в область строк, вернуть её смещение. Повторы не ищутся:
// строки у ключей разные, а раздел повторяется один раз на раздел.
uint16_t put_string(const char* text, uint8_t* page, uint32_t cap, uint32_t& used, bool& ok) {
    if (text == nullptr) return 0;
    const uint32_t len = static_cast<uint32_t>(std::strlen(text)) + 1u;
    if (used + len > cap) {
        ok = false;
        return 0;
    }
    const uint32_t at = used;
    std::memcpy(page + at, text, len);
    used += len;
    return static_cast<uint16_t>(at);
}

uint8_t values_sum(const uint8_t* values, uint32_t count) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        sum = static_cast<uint8_t>(sum + values[i]);
    }
    return sum;
}

// Поле страницы: ключ таблицы или клавиша действия джойстика.
uint8_t* joy_slot(Settings& s, uint32_t i) {
    const uint32_t k = i - kKeyCount;
    return &s.joy_keys[k / kJoyKeysPerAction][k % kJoyKeysPerAction];
}

uint8_t field_value(const Settings& s, uint32_t i) {
    if (i < kKeyCount) return s.*(kKeys[i].field);
    const uint32_t k = i - kKeyCount;
    return s.joy_keys[k / kJoyKeysPerAction][k % kJoyKeysPerAction];
}

void field_set(Settings& s, uint32_t i, uint8_t v) {
    if (i < kKeyCount) {
        s.*(kKeys[i].field) = v;
    } else {
        *joy_slot(s, i) = v;
    }
}

// Заголовок на месте и наш? Страница живёт в памяти, которую пишет машина,
// и поверить ей на слово нельзя.
const ConfigPageHeader* header_of(const uint8_t* page, uint32_t cap) {
    if (page == nullptr || cap < sizeof(ConfigPageHeader)) return nullptr;
    const ConfigPageHeader* h = reinterpret_cast<const ConfigPageHeader*>(page);
    if (h->magic != kConfigPageMagic || h->version != kConfigPageVersion) return nullptr;
    if (h->field_count != kPageFieldCount) return nullptr;
    if (h->values_offset + h->field_count + 1u > cap) return nullptr;
    if (h->saved_offset + h->field_count > cap) return nullptr;
    if (h->mask_offset + kConfigMaskBytes > cap) return nullptr;
    if (h->capture_offset + 2u > cap) return nullptr;
    return h;
}

void write_values_and_mask(const Settings& s, uint8_t* page, const ConfigPageHeader& h) {
    uint8_t* values = page + h.values_offset;
    for (uint32_t i = 0; i < kPageFieldCount; ++i) {
        values[i] = field_value(s, i);
    }
    values[kPageFieldCount] = values_sum(values, kPageFieldCount);

    const uint64_t mask = settings_active_mask(s);
    uint8_t* bits       = page + h.mask_offset;
    for (uint32_t i = 0; i < kConfigMaskBytes; ++i) {
        bits[i] = static_cast<uint8_t>(mask >> (i * 8));
    }
}

// Описатель поля: имя, описание, раздел и вид.
struct FieldText {
    const char* name;
    const char* comment;
    const char* section;
    Kind kind;
    uint8_t min;
    uint8_t max;
};

FieldText field_text(uint32_t i) {
    if (i < kKeyCount) return {kKeys[i].name, kKeys[i].comment, kKeys[i].section, kKeys[i].kind, kKeys[i].min, kKeys[i].max};
    const uint32_t k = i - kKeyCount;
    return {kJoyKeyFieldNames[k], kJoyKeyComment, k == 0 ? kJoyKeySection : nullptr, Kind::Key, 0, 0};
}

} // namespace

uint64_t settings_active_mask(const Settings& s) {
    uint64_t mask = (uint64_t{1} << kPageFieldCount) - 1u;
    for (uint32_t i = 0; i < kKeyCount; ++i) {
        const uint8_t Settings::*f = kKeys[i].field;
        bool on                    = true;
        // Носитель DivMMC имеет смысл только при включённом DivMMC.
        if (f == &Settings::disksys_media) on = divmmc_selected(s);
        // Носитель Z-Controller - только при включённом Z-Controller.
        if (f == &Settings::zcontroller_media) on = s.zcontroller != 0;
        // Сброс машины при DivMMC обязателен и принуждён в settings_clamp.
        if (f == &Settings::reset_signal) on = !divmmc_selected(s);
        // Раскладка модификаторов и скорость мыши ни к чему при выключенном
        // устройстве.
        if (f == &Settings::mod_right_shift || f == &Settings::mod_left_ctrl || f == &Settings::mod_right_ctrl || f == &Settings::mod_left_alt ||
            f == &Settings::mod_right_alt) {
            on = keyboard_enabled(s);
        }
        if (f == &Settings::mouse_speed || f == &Settings::mouse_swap_buttons || f == &Settings::mouse_autoswap) on = s.usb_mouse != 0;
        // Провод MIDI занимает вывод, который забирает Wi-Fi; при нём поле
        // остаётся правимым, но значение WIRE отсекает settings_clamp.
        if (!on) mask &= ~(uint64_t{1} << i);
    }
    // Привязки - клавиатуры: без неё их не нажать. Кнопки 2..4 - только у
    // восьмиразрядного Kempston.
    for (uint32_t k = 0; k < kJoyKeyFields; ++k) {
        const bool wide_only = k / kJoyKeysPerAction >= 5u;
        const bool on        = keyboard_enabled(s) && (!wide_only || s.joy_8bit != 0);
        if (!on) mask &= ~(uint64_t{1} << (kKeyCount + k));
    }
    return mask;
}

uint32_t config_page_field_count() {
    return kPageFieldCount;
}

uint32_t config_page_build(const Settings& s, uint8_t* page, uint32_t cap) {
    if (page == nullptr || cap < sizeof(ConfigPageHeader)) return 0;
    std::memset(page, 0, cap);

    const uint32_t desc_at    = sizeof(ConfigPageHeader);
    const uint32_t values_at  = desc_at + kPageFieldCount * kConfigFieldBytes;
    const uint32_t saved_at   = values_at + kPageFieldCount + 1u;
    const uint32_t mask_at    = saved_at + kPageFieldCount;
    const uint32_t capture_at = mask_at + kConfigMaskBytes;
    // Списки значений идут перед строками: их длина известна заранее.
    const uint32_t choices_at = capture_at + 2u;
    if (choices_at > cap) return 0;

    bool ok      = true;
    uint32_t put = choices_at;

    // Списки значений - по одному на вид, а не на поле: виды повторяются.
    uint16_t choice_at[kKindCount] = {};
    for (uint32_t i = 0; i < kPageFieldCount; ++i) {
        const Kind kind   = field_text(i).kind;
        const NameTable t = names_of(kind);
        if (t.items == nullptr) continue;
        const uint32_t k = static_cast<uint32_t>(kind);
        if (choice_at[k] != 0) continue;
        const uint32_t need = 4u + t.count * sizeof(ConfigPageChoice);
        if (put + need > cap) return 0;
        choice_at[k]  = static_cast<uint16_t>(put);
        page[put]     = static_cast<uint8_t>(t.count);
        put          += 4u; // счётчик с набивкой: записи ложатся по четыре байта
        for (uint32_t n = 0; n < t.count; ++n) {
            ConfigPageChoice c{};
            c.value = t.items[n].value;
            std::memcpy(page + put, &c, sizeof(c));
            put += sizeof(ConfigPageChoice);
        }
    }

    const uint32_t strings_at = put;

    // Имена значений: кладутся после того, как списки заняли место, и
    // прописываются в уже разложенные записи.
    for (uint32_t k = 0; k < kKindCount; ++k) {
        if (choice_at[k] == 0) continue;
        const NameTable t = names_of(static_cast<Kind>(k));
        for (uint32_t n = 0; n < t.count; ++n) {
            const uint16_t at = put_string(t.items[n].name, page, cap, put, ok);
            if (!ok) return 0;
            ConfigPageChoice c{};
            const uint32_t slot = choice_at[k] + 4u + n * sizeof(ConfigPageChoice);
            std::memcpy(&c, page + slot, sizeof(c));
            c.name = at;
            std::memcpy(page + slot, &c, sizeof(c));
        }
    }

    // Описание у всех привязок одно: кладётся один раз.
    uint16_t joy_comment = 0;
    for (uint32_t i = 0; i < kPageFieldCount; ++i) {
        const FieldText t = field_text(i);
        ConfigPageField f{};
        f.name = put_string(t.name, page, cap, put, ok);
        if (t.comment == kJoyKeyComment) {
            if (joy_comment == 0) joy_comment = put_string(t.comment, page, cap, put, ok);
            f.comment = joy_comment;
        } else {
            f.comment = put_string(t.comment, page, cap, put, ok);
        }
        f.section = put_string(t.section, page, cap, put, ok);
        if (!ok) return 0;
        f.choices = choice_at[static_cast<uint32_t>(t.kind)];
        if (t.kind == Kind::Key) {
            f.kind = static_cast<uint8_t>(PageKind::Key);
        } else if (f.choices != 0) {
            f.kind = static_cast<uint8_t>(PageKind::Choice);
        } else {
            f.kind = static_cast<uint8_t>(PageKind::Number);
            f.min  = t.min;
            f.max  = t.max;
        }
        std::memcpy(page + desc_at + i * kConfigFieldBytes, &f, sizeof(f));
    }

    ConfigPageHeader h{};
    h.magic          = kConfigPageMagic;
    h.version        = kConfigPageVersion;
    h.field_count    = static_cast<uint16_t>(kPageFieldCount);
    h.desc_offset    = static_cast<uint16_t>(desc_at);
    h.values_offset  = static_cast<uint16_t>(values_at);
    h.saved_offset   = static_cast<uint16_t>(saved_at);
    h.mask_offset    = static_cast<uint16_t>(mask_at);
    h.strings_offset = static_cast<uint16_t>(strings_at);
    h.capture_offset = static_cast<uint16_t>(capture_at);
    std::memcpy(page, &h, sizeof(h));

    write_values_and_mask(s, page, h);
    // Снимок сохранённого: правка его не трогает, поэтому ПЗУ всегда
    // знает, от чего отличается то, что на экране.
    std::memcpy(page + saved_at, page + values_at, kPageFieldCount);
    return put;
}

bool config_page_refresh(const Settings& s, uint8_t* page, uint32_t cap) {
    const ConfigPageHeader* h = header_of(page, cap);
    if (h == nullptr) return false;
    write_values_and_mask(s, page, *h);
    return true;
}

bool config_page_apply(const uint8_t* page, uint32_t cap, Settings& s) {
    const ConfigPageHeader* h = header_of(page, cap);
    if (h == nullptr) return false;
    const uint8_t* values = page + h->values_offset;
    if (values[kPageFieldCount] != values_sum(values, kPageFieldCount)) return false;

    Settings next = s;
    for (uint32_t i = 0; i < kPageFieldCount; ++i) {
        field_set(next, i, values[i]);
    }
    settings_clamp(next);
    s = next;
    return true;
}

uint8_t* config_page_capture(uint8_t* page, uint32_t cap) {
    const ConfigPageHeader* h = header_of(page, cap);
    return h == nullptr ? nullptr : page + h->capture_offset;
}

} // namespace soundsinth::config
