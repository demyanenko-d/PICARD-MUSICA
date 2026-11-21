// SPDX-License-Identifier: MIT
#include "player/config/config_page.h"

#include <cstring>

#include "core/config/config_fields.h"

namespace soundsinth::config {

// Поле страницы за ключами таблицы - клавиша действия джойстика.
namespace {
template <class S>
auto* joy_slot(S& s, uint32_t i) {
    const uint32_t k = i - kKeyCount;
    return &s.joy_keys[k / kJoyKeysPerAction][k % kJoyKeysPerAction];
}
} // namespace

uint8_t config_field_value(const Settings& s, uint32_t i) {
    if (i >= kTextFieldFirst) return static_cast<uint8_t>(std::strlen(text_of(s, kTextKeys[i - kTextFieldFirst])));
    if (i < kKeyCount) return s.*(kKeys[i].field);
    return *joy_slot(s, i);
}

void config_field_set(Settings& s, uint32_t i, uint8_t v) {
    if (i >= kTextFieldFirst) return; // строка приходит своим буфером
    if (i < kKeyCount) {
        s.*(kKeys[i].field) = v;
    } else {
        *joy_slot(s, i) = v;
    }
}

char* config_field_text(Settings& s, uint32_t i) {
    return i >= kTextFieldFirst && i < kPageFieldCount ? text_of(s, kTextKeys[i - kTextFieldFirst]) : nullptr;
}

const char* config_field_text(const Settings& s, uint32_t i) {
    return i >= kTextFieldFirst && i < kPageFieldCount ? text_of(s, kTextKeys[i - kTextFieldFirst]) : nullptr;
}

PageFieldInfo config_field_info(uint32_t i) {
    if (i >= kTextFieldFirst) {
        const TextDesc& t = kTextKeys[i - kTextFieldFirst];
        return {t.name, Kind::Number, t.file ? PageKind::File : PageKind::Text, static_cast<uint8_t>(t.secret ? 1u : 0u), t.max};
    }
    if (i >= kKeyCount) return {kJoyKeyFieldNames[i - kKeyCount], Kind::Key, PageKind::Key, 0, 0};
    const KeyDesc& k = kKeys[i];
    PageKind pk      = PageKind::Number;
    if (k.kind == Kind::Key) {
        pk = PageKind::Key;
    } else if (k.kind == Kind::Flag) {
        pk = PageKind::Flag;
    } else if (names_of(k.kind).items != nullptr) {
        pk = PageKind::Choice;
    }
    return {k.name, k.kind, pk, k.kind == Kind::Flag ? uint8_t{0} : k.min, k.kind == Kind::Flag ? uint8_t{1} : k.max};
}

uint8_t config_field_by_name(const char* name) {
    if (name == nullptr) return kNoField;
    for (uint32_t i = 0; i < kPageFieldCount; ++i) {
        if (std::strcmp(config_field_info(i).name, name) == 0) return static_cast<uint8_t>(i);
    }
    return kNoField;
}

// Вместо SLOT1..3 - имя файла области, если оно задано: в списке выбора
// ПЗУ человек ищет файл, а не номер.
const char* config_choice_name(const Settings& s, Kind kind, uint32_t n) {
    const NameTable t = names_of(kind);
    if (kind == Kind::Rom && n != 0 && n <= kRomSlots && s.rom_file[n - 1u][0] != 0) return s.rom_file[n - 1u];
    return t.items != nullptr && n < t.count ? t.items[n].name : nullptr;
}

// Поле должно помещаться в маску: бит на поле.
static_assert(kPageFieldCount <= 64u, "the availability mask has a bit per field");

uint64_t settings_active_mask(const Settings& s) {
    uint64_t mask = (uint64_t{1} << kPageFieldCount) - 1u;
    for (uint32_t i = 0; i < kKeyCount; ++i) {
        const uint8_t Settings::*f = kKeys[i].field;
        bool on                    = true;
        // Сброс машины при DivMMC обязателен и принуждён в settings_clamp.
        if (f == &Settings::reset_signal) on = !divmmc_selected(s);
        // Своё ПЗУ подставляет DivMMC: выбор ПЗУ - без него.
        if (f == &Settings::rom) on = custom_rom_allowed(s);
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
    // Сеть - только при включённом Wi-Fi, файлы ПЗУ - как поле ROM.
    for (uint32_t k = 0; k < kTextKeyCount; ++k) {
        const bool on = kTextKeys[k].file ? custom_rom_allowed(s) : s.wifi != 0;
        if (!on) mask &= ~(uint64_t{1} << (kTextFieldFirst + k));
    }
    return mask;
}

} // namespace soundsinth::config
