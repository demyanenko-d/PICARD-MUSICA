// SPDX-License-Identifier: MIT
// Режим загрузки на ПК: запроса нет и перемычки нет.
//
// Проверять здесь нечего - смысл запроса в том, что он переживает
// перезагрузку платы, а у процесса на ПК перезагрузки не бывает. Заглушка
// нужна, чтобы загрузка настроек собиралась тем же кодом.

#include "platform/boot_mode.h"

#include <cstdlib>

void platform::boot_reboot(const BootRequest&) {
    // Перезагружаться нечему: на ПК это конец работы.
    std::abort();
}

void platform::boot_reboot_saving(BootRequest, uint32_t, const uint8_t*, uint32_t) {
    std::abort();
}

bool platform::boot_request_take(BootRequest&) {
    return false;
}

bool platform::boot_jumper_closed() {
    return false;
}

bool platform::config_rom_active() {
    return false;
}

uint8_t* platform::config_rom_page() {
    return nullptr;
}

uint32_t platform::config_rom_page_bytes() {
    return 0;
}
