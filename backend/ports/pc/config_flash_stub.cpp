// SPDX-License-Identifier: MIT
// Блоки настроек на ПК - в памяти. Механизм проверяется целиком: выбор
// блока, чередование и переживание обрыва не зависят от того, флеш это
// или массив.

#include <cstring>

#include "platform/config_flash.h"

#include "core/config/config_store.h"

namespace {

constexpr uint32_t kPages = soundsinth::config::kPageCount;
constexpr uint32_t kBytes = soundsinth::config::kPageBytes;

uint8_t s_page[kPages][kBytes];
bool s_ready = false;

void ensure() {
    if (s_ready) return;
    std::memset(s_page, 0xFF, sizeof(s_page));
    s_ready = true;
}

} // namespace

uint32_t platform::config_flash_pages() {
    return kPages;
}

const uint8_t* platform::config_flash_view(uint32_t slot) {
    if (slot >= kPages) return nullptr;
    ensure();
    return s_page[slot];
}

bool platform::config_flash_write(uint32_t slot, const uint8_t* page, uint32_t bytes) {
    if (slot >= kPages || page == nullptr || bytes != kBytes) return false;
    ensure();
    std::memcpy(s_page[slot], page, bytes);
    return true;
}

// Области своих ПЗУ на ПК - в памяти, как блоки настроек.
namespace {
uint8_t s_rom[3][platform::kRomSlotBytes];
bool s_rom_ready = false;
} // namespace

const uint8_t* platform::rom_slot_view(uint32_t slot) {
    if (slot >= 3u) return nullptr;
    if (!s_rom_ready) {
        std::memset(s_rom, 0xFF, sizeof(s_rom));
        s_rom_ready = true;
    }
    return s_rom[slot];
}

bool platform::rom_slot_write(uint32_t slot, uint32_t sector, const uint8_t* data) {
    if (slot >= 3u || sector >= kRomSlotBytes / kRomSectorBytes || data == nullptr) return false;
    (void)rom_slot_view(slot);
    std::memcpy(s_rom[slot] + sector * kRomSectorBytes, data, kRomSectorBytes);
    return true;
}
