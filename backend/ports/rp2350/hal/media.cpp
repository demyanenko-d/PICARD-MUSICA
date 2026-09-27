// Носители под арбитром (devices/hal/media.h): карта на SPI1 и флешка на
// USB. Прослойка, а не прямые вызовы протоколов: выбор носителя делает
// арбитр, а какой драйвер за ним стоит - знают только эти строки.
//
// Оба обслуживаются циклом Core1 и только им: у карты обмен по SPI
// блокирующий, у флешки виток стека крутится там же.

#include "devices/hal/media.h"

#include "devices/sd/card_protocol.h"
#include "platform/hot_path.h"
#include "usb/usb_msc.h"

bool devices::hal::media_init() {
    // Поднимается только карта: флешку поднимает подключение, и ждать её
    // при старте незачем - машина к этому времени уже читает.
    return devices::sd::sd_card_init();
}

bool devices::hal::media_present(Medium m) {
    return m == Medium::Usb ? rp2350::usb::msc_present() : devices::sd::sd_card_info().present;
}

uint32_t devices::hal::media_sector_count(Medium m) {
    return m == Medium::Usb ? rp2350::usb::msc_sector_count() : devices::sd::sd_card_info().sector_count;
}

SOUNDSINTH_HOT_PATH_ATTR("media_read")
bool devices::hal::media_read(Medium m, uint32_t lba, uint8_t* dst) {
    return m == Medium::Usb ? rp2350::usb::msc_read(lba, dst) : devices::sd::sd_card_read_sector(lba, dst);
}

bool devices::hal::media_write(Medium m, uint32_t lba, const uint8_t* src) {
    return m == Medium::Usb ? rp2350::usb::msc_write(lba, src) : devices::sd::sd_card_write_sector(lba, src);
}

void devices::hal::media_log_health() { devices::sd::sd_card_log_health(); }
