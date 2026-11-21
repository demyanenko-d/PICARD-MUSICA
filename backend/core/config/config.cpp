// SPDX-License-Identifier: MIT
#include "core/config/config.h"

#include <cstring>

#include "core/config/config_fields.h"

namespace soundsinth::config {
namespace {

// Незнакомый носитель (его писала прошивка новее) - устройство выключено:
// отвечать машине с чужого носителя хуже, чем молчать.
void clamp_media(uint8_t& v) {
    if (v > static_cast<uint8_t>(Media::None)) v = static_cast<uint8_t>(Media::None);
}

// Незнакомая роль - лучше никакая: чужой модификатор, вставший в CAPS
// SHIFT, ломает набор молча.
void clamp_mod(uint8_t& v) {
    if (v > static_cast<uint8_t>(ModRole::SymbolShift)) v = static_cast<uint8_t>(ModRole::None);
}

// Строка обрывается на первом непечатном знаке и всегда кончается нулём.
template <uint32_t N>
void clamp_text(char (&t)[N]) {
    uint32_t i = 0;
    while (i < N - 1u && ascii_printable(static_cast<uint8_t>(t[i]))) {
        ++i;
    }
    std::memset(t + i, 0, N - i);
}

} // namespace

void settings_clamp(Settings& s) {
    // Флаги и числа - в пределы таблицы ключей: новый ключ приводится сам.
    for (const KeyDesc& k : kKeys) {
        uint8_t& v = s.*k.field;
        if (k.kind != Kind::Flag && k.kind != Kind::Number) continue;
        if (v < k.min) v = k.min;
        if (v > k.max) v = k.max;
    }
    clamp_text(s.wifi_ssid);
    clamp_text(s.wifi_pass);
    // Имя файла ПЗУ - как есть, с байтами кодовой страницы карты; только
    // ноль в конце.
    for (auto& name : s.rom_file) {
        name[sizeof(name) - 1] = 0;
    }

    // Незнакомая дисковая система - ни одной: подставлять чужую значит
    // отдать машине память или ПЗУ не тем способом.
    if (s.disksys > static_cast<uint8_t>(DiskSys::TrDos)) s.disksys = static_cast<uint8_t>(DiskSys::None);
    // Незнакомая область - ПЗУ машины: чужой образ машину не запустит.
    if (s.rom > static_cast<uint8_t>(RomSource::Slot3)) s.rom = static_cast<uint8_t>(RomSource::Board);
    if (s.live_midi > static_cast<uint8_t>(LiveMidi::Wire)) s.live_midi = static_cast<uint8_t>(LiveMidi::None);
    // Провод и Wi-Fi делят единственный вывод приёма. Уступает MIDI:
    // Wi-Fi - это связь платы, без неё не добраться и до настроек.
    if (settings_conflict(s)) s.live_midi = static_cast<uint8_t>(LiveMidi::None);

    if (s.sd_speed >= kSdSpeedCount) s.sd_speed = kSdSpeedDefault;
    if (s.ay_emulation > static_cast<uint8_t>(AyEmulation::TsfmYm2203)) s.ay_emulation = static_cast<uint8_t>(AyEmulation::None);
    if (s.ay_stereo > static_cast<uint8_t>(AyStereo::Mono)) s.ay_stereo = static_cast<uint8_t>(AyStereo::Acb);
    if (s.beeper > static_cast<uint8_t>(Beeper::TwoBit)) s.beeper = static_cast<uint8_t>(Beeper::None);
    clamp_media(s.divmmc_card1);
    clamp_media(s.divmmc_card2);
    clamp_media(s.zcontroller);

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

    // Esc занят отменой, коды ниже 4 - не клавиши, выше kKeyCodeMax - не
    // клавиши раскладки.
    for (auto& action : s.joy_keys) {
        for (uint8_t& code : action) {
            if (code == kKeyEsc || code == kKeyBackspace || code < kKeyCodeMin || code > kKeyCodeMax) code = 0;
        }
    }

    // DivMMC грузит машину со своей карты, а для этого обязан её сбросить:
    // без сброса машина уже стоит в чужом ПЗУ, и подставлять ей память
    // поздно.
    if (divmmc_selected(s)) s.reset_signal = 1;
    // DivMMC грузится с карты: без неё машине грузиться не с чего.
    if (divmmc_selected(s) && !device_on(s.divmmc_card1) && !device_on(s.divmmc_card2)) s.divmmc_card1 = static_cast<uint8_t>(Media::Sd);
}

} // namespace soundsinth::config
