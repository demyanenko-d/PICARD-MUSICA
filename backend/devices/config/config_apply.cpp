// SPDX-License-Identifier: MIT
#include "devices/config/config_apply.h"

#include "devices/sd/spi_emu.h"
#include "devices/storage/storage.h"
#include "devices/hid/usb_map.h"

namespace devices::config {
namespace {

using soundsinth::config::ModRole;

// Числа ролей в настройках и в раскладке совпадают, и это часть договора:
// иначе CAPS SHIFT из файла оказался бы SYMBOL SHIFT на матрице.
static_assert(static_cast<uint8_t>(ModRole::None) == static_cast<uint8_t>(hid::ModRole::None), "roles: None");
static_assert(static_cast<uint8_t>(ModRole::CapsShift) == static_cast<uint8_t>(hid::ModRole::CapsShift), "roles: CapsShift");
static_assert(static_cast<uint8_t>(ModRole::SymbolShift) == static_cast<uint8_t>(hid::ModRole::SymbolShift), "roles: SymbolShift");

hid::ModRole role_of(uint8_t v) {
    return static_cast<hid::ModRole>(v);
}

} // namespace

void config_apply(const soundsinth::config::Settings& s) {
    hid::ModifierRoles roles;
    roles.right_shift = role_of(s.mod_right_shift);
    roles.left_ctrl   = role_of(s.mod_left_ctrl);
    roles.right_ctrl  = role_of(s.mod_right_ctrl);
    roles.left_alt    = role_of(s.mod_left_alt);
    roles.right_alt   = role_of(s.mod_right_alt);
    hid::usb_map_set_modifiers(roles);

    hid::MouseTuning mouse;
    mouse.speed        = s.mouse_speed;
    mouse.swap_buttons = s.mouse_swap_buttons != 0;
    hid::usb_map_set_mouse(mouse);

    // Носитель каждого эмулятора: выбор явный, подмены нет. Нет носителя
    // физически - эмулятор скажет машине, что карты нет.
    const auto medium = [](uint8_t v) { return v == static_cast<uint8_t>(soundsinth::config::Media::Usb) ? hal::Medium::Usb : hal::Medium::Card; };
    storage::storage_set_emulator_medium(static_cast<uint8_t>(sd::SdOwner::DivMmc), medium(s.disksys_media));
    storage::storage_set_emulator_medium(static_cast<uint8_t>(sd::SdOwner::ZController), medium(s.zcontroller_media));
}

} // namespace devices::config
