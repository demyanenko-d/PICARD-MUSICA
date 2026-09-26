// Носитель под арбитром (devices/hal/media.h) - карта на SPI1. Прослойка,
// а не прямые вызовы протокола: здесь место под второй носитель.

#include "devices/hal/media.h"

#include "devices/sd/card_protocol.h"
#include "platform/hot_path.h"

bool devices::hal::media_init() { return devices::sd::sd_card_init(); }

bool devices::hal::media_present() { return devices::sd::sd_card_info().present; }

uint32_t devices::hal::media_sector_count() { return devices::sd::sd_card_info().sector_count; }

SOUNDSINTH_HOT_PATH_ATTR("media_read")
bool devices::hal::media_read(uint32_t lba, uint8_t* dst) { return devices::sd::sd_card_read_sector(lba, dst); }

bool devices::hal::media_write(uint32_t lba, const uint8_t* src) { return devices::sd::sd_card_write_sector(lba, src); }

void devices::hal::media_log_health() { devices::sd::sd_card_log_health(); }
