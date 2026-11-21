// SPDX-License-Identifier: MIT
// Два блока настроек во флеше RP2350.
//
// Место - последние две гранулы первого мегабайта, сразу под банком
// инструментов. Прошивка кончается много раньше, банк начинается выше:
// ни та, ни другая заливка сюда не пишет, и настройки их переживают.

#include <cstring>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico.h"

#include "platform/config_flash.h"

#include "core/config/config_store.h"

#include "firmware_config.h"
#include "psram/psram_driver.h"

namespace {

constexpr uint32_t kPageBytes = soundsinth::config::kPageBytes;
constexpr uint32_t kPages     = soundsinth::config::kPageCount;

static_assert(kPageBytes == FLASH_SECTOR_SIZE, "the settings block is exactly one erase granule");
static_assert(platform::kRomSlots == soundsinth::config::kRomSlots, "one flash slot per ROM setting");

// Ниже банка на две гранулы.
constexpr uint32_t kBaseOffset = SOUNDSINTH_BANK_FLASH_OFFSET - kPages * kPageBytes;
static_assert(kBaseOffset % FLASH_SECTOR_SIZE == 0, "blocks are aligned to the granule");

uint32_t offset_of(uint32_t slot) {
    return kBaseOffset + slot * kPageBytes;
}

} // namespace

uint32_t platform::config_flash_pages() {
    return kPages;
}

const uint8_t* platform::config_flash_view(uint32_t slot) {
    if (slot >= kPages) return nullptr;
    return reinterpret_cast<const uint8_t*>(XIP_BASE + offset_of(slot));
}

// Пока идёт стирание, XIP недоступен: прерывания гасятся, а второе ядро на
// этом месте ещё не запущено. Дальше по флешу ходит только бутром, и код
// для этого он берёт из своего ПЗУ.
//
// После записи бутром возвращает окно флеша тем же boot2, что и при
// включении, но окно PSRAM он не знает - его ставим заново. Порядок
// такой же, как при загрузке, и на этом месте PSRAM ещё никем не занята.
bool platform::config_flash_write(uint32_t slot, const uint8_t* page, uint32_t bytes) {
    if (slot >= kPages || page == nullptr || bytes != kPageBytes) return false;
    const uint32_t off = offset_of(slot);

    const uint32_t mask = save_and_disable_interrupts();
    flash_range_erase(off, kPageBytes);
    flash_range_program(off, page, kPageBytes);
    restore_interrupts(mask);

    psram::psram_init();

    return std::memcmp(reinterpret_cast<const uint8_t*>(XIP_BASE + off), page, bytes) == 0;
}

// Области своих ПЗУ: та же запись, что у блоков настроек, и в то же окно загрузки.
const uint8_t* platform::rom_slot_view(uint32_t slot) {
    if (slot >= kRomSlots) return nullptr;
    return reinterpret_cast<const uint8_t*>(XIP_BASE + SOUNDSINTH_ROM_SLOTS_FLASH_OFFSET + slot * SOUNDSINTH_ROM_SLOT_BYTES);
}

bool platform::rom_slot_write(uint32_t slot, uint32_t sector, const uint8_t* data) {
    static_assert(kRomSectorBytes == FLASH_SECTOR_SIZE, "a ROM slot is written by erase granules");
    static_assert(kRomSlotBytes == SOUNDSINTH_ROM_SLOT_BYTES, "the ROM slot size agrees with the flash layout");
    if (slot >= kRomSlots || sector >= kRomSlotBytes / kRomSectorBytes || data == nullptr) return false;
    const uint32_t off = SOUNDSINTH_ROM_SLOTS_FLASH_OFFSET + slot * SOUNDSINTH_ROM_SLOT_BYTES + sector * kRomSectorBytes;

    const uint32_t mask = save_and_disable_interrupts();
    flash_range_erase(off, kRomSectorBytes);
    flash_range_program(off, data, kRomSectorBytes);
    restore_interrupts(mask);

    psram::psram_init();

    return std::memcmp(reinterpret_cast<const uint8_t*>(XIP_BASE + off), data, kRomSectorBytes) == 0;
}
