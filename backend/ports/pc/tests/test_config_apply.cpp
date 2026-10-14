// SPDX-License-Identifier: MIT
// Применение настроек: config_apply разносит поля блока по устройствам.
//
// Ошибка здесь не ломает сборку и не меняет звук: устройство молча
// остаётся с прежним значением.

#include "testing.h"

#include <cstdio>

#include "core/config/config.h"
#include "devices/config/config_apply.h"
#include "devices/hid/usb_map.h"
#include "devices/sd/spi_emu.h"
#include "devices/storage/storage.h"

namespace cfg = soundsinth::config;

namespace {

// Роли модификаторов: числа в настройках и в раскладке обязаны совпадать,
// иначе CAPS SHIFT из файла станет SYMBOL SHIFT на матрице. В самом
// config_apply это держат static_assert, здесь проверяется перенос.
void roles_reach_the_keymap() {
    cfg::Settings s;
    s.mod_right_shift = static_cast<uint8_t>(cfg::ModRole::SymbolShift);
    s.mod_left_ctrl   = static_cast<uint8_t>(cfg::ModRole::None);
    s.mod_right_ctrl  = static_cast<uint8_t>(cfg::ModRole::CapsShift);
    s.mod_left_alt    = static_cast<uint8_t>(cfg::ModRole::SymbolShift);
    s.mod_right_alt   = static_cast<uint8_t>(cfg::ModRole::CapsShift);
    devices::config::config_apply(s);

    const devices::hid::ModifierRoles r = devices::hid::usb_map_modifiers();
    CHECK(r.right_shift == devices::hid::ModRole::SymbolShift);
    CHECK(r.left_ctrl == devices::hid::ModRole::None);
    CHECK(r.right_ctrl == devices::hid::ModRole::CapsShift);
    CHECK(r.left_alt == devices::hid::ModRole::SymbolShift);
    CHECK(r.right_alt == devices::hid::ModRole::CapsShift);
}

// Мышь: перестановка кнопок доходит до раскладки, а жест обмена при
// выключенной настройке не действует.
void mouse_settings_reach_the_keymap() {
    cfg::Settings s;
    s.mouse_swap_buttons = 1;
    devices::config::config_apply(s);
    CHECK(devices::hid::usb_map_mouse_swapped());

    s.mouse_swap_buttons = 0;
    devices::config::config_apply(s);
    CHECK(!devices::hid::usb_map_mouse_swapped());
}

// Носитель каждого эмулятора ставится отдельно, подмены нет: DivMMC может
// работать с флешки, пока Z-Controller работает с карты.
void media_reach_each_emulator() {
    cfg::Settings s;
    s.disksys_media     = static_cast<uint8_t>(cfg::Media::Usb);
    s.zcontroller_media = static_cast<uint8_t>(cfg::Media::Sd);
    devices::config::config_apply(s);
    CHECK(devices::storage::storage_emulator_medium(devices::sd::SdOwner::DivMmc) == devices::hal::Medium::Usb);
    CHECK(devices::storage::storage_emulator_medium(devices::sd::SdOwner::ZController) == devices::hal::Medium::Card);

    s.disksys_media     = static_cast<uint8_t>(cfg::Media::Sd);
    s.zcontroller_media = static_cast<uint8_t>(cfg::Media::Usb);
    devices::config::config_apply(s);
    CHECK(devices::storage::storage_emulator_medium(devices::sd::SdOwner::DivMmc) == devices::hal::Medium::Card);
    CHECK(devices::storage::storage_emulator_medium(devices::sd::SdOwner::ZController) == devices::hal::Medium::Usb);
}

// Умолчания блока доходят как умолчания устройств: кнопки не переставлены,
// оба носителя - карта.
void defaults_reach_devices() {
    const cfg::Settings s;
    devices::config::config_apply(s);
    CHECK(!devices::hid::usb_map_mouse_swapped());
    CHECK(devices::storage::storage_emulator_medium(devices::sd::SdOwner::DivMmc) == devices::hal::Medium::Card);
    CHECK(devices::storage::storage_emulator_medium(devices::sd::SdOwner::ZController) == devices::hal::Medium::Card);
}

} // namespace

void run_config_apply_tests() {
    std::printf("config: settings reach the devices\n");
    roles_reach_the_keymap();
    mouse_settings_reach_the_keymap();
    media_reach_each_emulator();
    defaults_reach_devices();
}
