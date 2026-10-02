// SPDX-License-Identifier: MIT
// Блоки настроек на ПК - в памяти. Механизм проверяется целиком: выбор
// блока, чередование и переживание обрыва не зависят от того, флеш это
// или массив.

#include "platform/config_flash.h"

#include <cstring>

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
