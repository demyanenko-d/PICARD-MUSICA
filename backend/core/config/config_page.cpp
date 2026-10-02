// SPDX-License-Identifier: MIT
#include "core/config/config_page.h"

#include <cstring>

#include "core/config/config_fields.h"

namespace soundsinth::config {

namespace {

static_assert(kKeyCount <= 32, "the availability mask is one word");

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

// Заголовок на месте и наш? Страница живёт в памяти, которую пишет машина,
// и поверить ей на слово нельзя.
const ConfigPageHeader* header_of(const uint8_t* page, uint32_t cap) {
    if (page == nullptr || cap < sizeof(ConfigPageHeader)) return nullptr;
    const ConfigPageHeader* h = reinterpret_cast<const ConfigPageHeader*>(page);
    if (h->magic != kConfigPageMagic || h->version != kConfigPageVersion) return nullptr;
    if (h->field_count != kKeyCount) return nullptr;
    if (h->values_offset + h->field_count + 1u > cap) return nullptr;
    if (h->saved_offset + h->field_count > cap) return nullptr;
    if (h->mask_offset + 4u > cap) return nullptr;
    return h;
}

void write_values_and_mask(const Settings& s, uint8_t* page, const ConfigPageHeader& h) {
    uint8_t* values = page + h.values_offset;
    for (uint32_t i = 0; i < kKeyCount; ++i) {
        values[i] = s.*(kKeys[i].field);
    }
    values[kKeyCount] = values_sum(values, kKeyCount);

    const uint32_t mask = settings_active_mask(s);
    uint8_t* bits       = page + h.mask_offset;
    for (uint32_t i = 0; i < 4; ++i) {
        bits[i] = static_cast<uint8_t>(mask >> (i * 8));
    }
}

} // namespace

uint32_t settings_active_mask(const Settings& s) {
    uint32_t mask = (kKeyCount >= 32) ? 0xFFFFFFFFu : ((1u << kKeyCount) - 1u);
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
        if (f == &Settings::mouse_speed || f == &Settings::mouse_swap_buttons) on = s.usb_mouse != 0;
        // Провод MIDI занимает вывод, который забирает Wi-Fi; при нём поле
        // остаётся правимым, но значение WIRE отсекает settings_clamp.
        if (!on) mask &= ~(1u << i);
    }
    return mask;
}

uint32_t config_page_field_count() {
    return kKeyCount;
}

uint32_t config_page_build(const Settings& s, uint8_t* page, uint32_t cap) {
    if (page == nullptr || cap < sizeof(ConfigPageHeader)) return 0;
    std::memset(page, 0, cap);

    const uint32_t desc_at   = sizeof(ConfigPageHeader);
    const uint32_t values_at = desc_at + kKeyCount * kConfigFieldBytes;
    const uint32_t saved_at  = values_at + kKeyCount + 1u;
    const uint32_t mask_at   = saved_at + kKeyCount;
    // Списки значений идут перед строками: их длина известна заранее.
    const uint32_t choices_at = mask_at + 4u;
    if (choices_at > cap) return 0;

    bool ok      = true;
    uint32_t put = choices_at;

    // Списки значений - по одному на вид, а не на поле: виды повторяются.
    uint16_t choice_at[8] = {};
    for (uint32_t i = 0; i < kKeyCount; ++i) {
        const NameTable t = names_of(kKeys[i].kind);
        if (t.items == nullptr) continue;
        const uint32_t k = static_cast<uint32_t>(kKeys[i].kind);
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
    for (uint32_t k = 0; k < 8; ++k) {
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

    for (uint32_t i = 0; i < kKeyCount; ++i) {
        ConfigPageField f{};
        f.name    = put_string(kKeys[i].name, page, cap, put, ok);
        f.comment = put_string(kKeys[i].comment, page, cap, put, ok);
        f.section = put_string(kKeys[i].section, page, cap, put, ok);
        if (!ok) return 0;
        f.choices = choice_at[static_cast<uint32_t>(kKeys[i].kind)];
        f.kind    = static_cast<uint8_t>(f.choices != 0 ? PageKind::Choice : PageKind::Number);
        if (f.kind == static_cast<uint8_t>(PageKind::Number)) {
            f.min = kKeys[i].min;
            f.max = kKeys[i].max;
        }
        std::memcpy(page + desc_at + i * kConfigFieldBytes, &f, sizeof(f));
    }

    ConfigPageHeader h{};
    h.magic          = kConfigPageMagic;
    h.version        = kConfigPageVersion;
    h.field_count    = static_cast<uint16_t>(kKeyCount);
    h.desc_offset    = static_cast<uint16_t>(desc_at);
    h.values_offset  = static_cast<uint16_t>(values_at);
    h.saved_offset   = static_cast<uint16_t>(saved_at);
    h.mask_offset    = static_cast<uint16_t>(mask_at);
    h.strings_offset = static_cast<uint16_t>(strings_at);
    std::memcpy(page, &h, sizeof(h));

    write_values_and_mask(s, page, h);
    // Снимок сохранённого: правка его не трогает, поэтому ПЗУ всегда
    // знает, от чего отличается то, что на экране.
    std::memcpy(page + saved_at, page + values_at, kKeyCount);
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
    if (values[kKeyCount] != values_sum(values, kKeyCount)) return false;

    Settings next = s;
    for (uint32_t i = 0; i < kKeyCount; ++i) {
        next.*(kKeys[i].field) = values[i];
    }
    settings_clamp(next);
    s = next;
    return true;
}

} // namespace soundsinth::config
