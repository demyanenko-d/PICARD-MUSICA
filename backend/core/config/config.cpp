// SPDX-License-Identifier: MIT
#include "core/config/config.h"

namespace soundsinth::config {
namespace {

void clamp_flag(uint8_t& v) {
    if (v > 1u) v = 1u;
}

void clamp_media(uint8_t& v) {
    if (v > static_cast<uint8_t>(Media::Usb)) v = static_cast<uint8_t>(Media::Sd);
}

// Незнакомая роль - лучше никакая: чужой модификатор, вставший в CAPS
// SHIFT, ломает набор молча.
void clamp_mod(uint8_t& v) {
    if (v > static_cast<uint8_t>(ModRole::SymbolShift)) v = static_cast<uint8_t>(ModRole::None);
}

} // namespace

void settings_clamp(Settings& s) {
    clamp_flag(s.zcontroller);
    clamp_flag(s.gs);
    clamp_flag(s.reset_signal);
    clamp_flag(s.usb_mouse);
    clamp_flag(s.usb_gamepad);
    clamp_flag(s.mouse_swap_buttons);
    clamp_flag(s.auto_volume);
    clamp_flag(s.log);
    clamp_flag(s.wifi);

    // Незнакомая дисковая система - ни одной: подставлять чужую значит
    // отдать машине память или ПЗУ не тем способом.
    if (s.disksys > static_cast<uint8_t>(DiskSys::TrDos)) s.disksys = static_cast<uint8_t>(DiskSys::None);
    if (s.live_midi > static_cast<uint8_t>(LiveMidi::Wire)) s.live_midi = static_cast<uint8_t>(LiveMidi::None);
    // Провод и Wi-Fi делят единственный вывод приёма. Уступает MIDI:
    // Wi-Fi - это связь платы, без неё не добраться и до настроек.
    if (settings_conflict(s)) s.live_midi = static_cast<uint8_t>(LiveMidi::None);

    clamp_media(s.disksys_media);
    clamp_media(s.zcontroller_media);

    clamp_mod(s.mod_right_shift);
    clamp_mod(s.mod_left_ctrl);
    clamp_mod(s.mod_right_ctrl);
    clamp_mod(s.mod_left_alt);
    clamp_mod(s.mod_right_alt);

    // Незнакомая раскладка - не выключение клавиатуры, а наша: выключать
    // её из-за чужого номера пользователь не просил.
    if (s.keyboard_layout > static_cast<uint8_t>(Layout::Picard)) {
        s.keyboard_layout = static_cast<uint8_t>(Layout::Picard);
    }

    if (s.mouse_speed < kMouseSpeedMin) s.mouse_speed = kMouseSpeedMin;
    if (s.mouse_speed > kMouseSpeedMax) s.mouse_speed = kMouseSpeedMax;

    // DivMMC грузит машину со своей карты, а для этого обязан её сбросить:
    // без сброса машина уже стоит в чужом ПЗУ, и подставлять ей память
    // поздно.
    if (divmmc_selected(s)) s.reset_signal = 1;
}

} // namespace soundsinth::config
