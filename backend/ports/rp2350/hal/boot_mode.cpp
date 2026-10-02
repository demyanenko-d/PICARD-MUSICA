// SPDX-License-Identifier: MIT
// Режим загрузки и запрос на следующую: RP2350.
//
// Запрос лежит в регистрах POWMAN - всегда запитанного домена. Их восемь,
// старшие два заняты SDK под засыпание; шести хватает ровно: слово под
// метку с признаками и пять слов под двадцать байт настроек.
//
// Два других места не годятся. SRAM в секции без обнуления при старте
// перезагрузку не переживает, хотя запуск C её не трогает: она ниже
// __bss_start__ и __data_start__, значит чистит бутром. Сторожевой
// scratch[0] занят счётчиком загрузок, тем, что печатает строка
// "boot: reset".

#include "platform/boot_mode.h"

#include <cstring>

#include "hardware/gpio.h"
#include "hardware/structs/powman.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"

#include "bus.h" // PIN_BOOT_JUMPER
#include "platform/log.h"

namespace {

// Старшие три байта метки, младший - признаки.
constexpr uint32_t kMagic            = 0x43464700u; // 'CFG' и байт под признаки
constexpr uint32_t kMagicMask        = 0xFFFFFF00u;
constexpr uint32_t kFlagConfigurator = 1u << 0;
constexpr uint32_t kFlagSave         = 1u << 1;
constexpr uint32_t kFlagDropGui      = 1u << 2;

// Слово метки и пять слов настроек. Старшие два регистра POWMAN заняты
// SDK под засыпание, остальные свободны.
constexpr uint32_t kSlotFlags    = 0;
constexpr uint32_t kSlotSettings = 1;
constexpr uint32_t kSettingWords = sizeof(soundsinth::config::Settings) / sizeof(uint32_t);
static_assert(sizeof(soundsinth::config::Settings) % sizeof(uint32_t) == 0, "settings go into POWMAN whole words");
static_assert(kSlotSettings + kSettingWords <= 6, "the carryover fits the POWMAN registers free of the SDK");

} // namespace

void platform::boot_reboot(const BootRequest& req) {
    uint32_t words[kSettingWords];
    std::memcpy(words, &req.settings, sizeof(words));
    for (uint32_t i = 0; i < kSettingWords; ++i) {
        powman_hw->scratch[kSlotSettings + i] = words[i];
    }

    uint32_t flags = kMagic;
    if (req.configurator) flags |= kFlagConfigurator;
    if (req.save) flags |= kFlagSave;
    if (req.drop_open_gui) flags |= kFlagDropGui;
    // Метка кладётся последней: по ней запрос и читается, и до неё он
    // считается недописанным.
    powman_hw->scratch[kSlotFlags] = flags;

    // Перезагрузка без возврата; срок ненулевой, иначе сторож не успевает
    // взвестись.
    watchdog_reboot(0, 0, 1);
    for (;;) {
        tight_loop_contents();
    }
}

bool platform::boot_request_take(BootRequest& out) {
    const uint32_t flags = powman_hw->scratch[kSlotFlags];
    debug_logf("boot: carryover %08lx\n", static_cast<unsigned long>(flags));
    // Гасится в любом случае: запрос одноразовый, и битый не должен
    // пережить эту загрузку.
    powman_hw->scratch[kSlotFlags] = 0;

    if ((flags & kMagicMask) != kMagic) return false;

    out               = BootRequest{};
    out.configurator  = (flags & kFlagConfigurator) != 0u;
    out.drop_open_gui = (flags & kFlagDropGui) != 0u;
    out.save          = (flags & kFlagSave) != 0u;
    if (out.save) {
        uint32_t words[kSettingWords];
        for (uint32_t i = 0; i < kSettingWords; ++i) {
            words[i] = powman_hw->scratch[kSlotSettings + i];
        }
        std::memcpy(&out.settings, words, sizeof(words));
    }
    return true;
}

bool platform::boot_jumper_closed() {
    gpio_init(bus::PIN_BOOT_JUMPER);
    gpio_set_dir(bus::PIN_BOOT_JUMPER, GPIO_IN);
    gpio_pull_up(bus::PIN_BOOT_JUMPER);
    // Подтяжке нужно время поднять вход: ёмкость провода к перемычке на
    // порядок больше ёмкости пада.
    busy_wait_us(100);
    const bool closed = !gpio_get(bus::PIN_BOOT_JUMPER);
    return closed;
}

bool platform::config_rom_active() {
    return bus::config_rom_enabled();
}

uint8_t* platform::config_rom_page() {
    return bus::config_rom_enabled() ? bus::config_rom_page() : nullptr;
}

uint32_t platform::config_rom_page_bytes() {
    return bus::config_rom_page_bytes();
}
