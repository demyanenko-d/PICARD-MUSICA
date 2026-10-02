// SPDX-License-Identifier: MIT
// Носители под арбитром: карта на SPI1 и флешка на
// USB. Прослойка, а не прямые вызовы протоколов: выбор носителя делает
// арбитр, а какой драйвер за ним стоит - знают только эти строки.
//
// Оба обслуживаются циклом Core1 и только им: у карты обмен по SPI
// блокирующий, у флешки виток стека крутится там же.

#include "devices/hal/media.h"

#include <atomic>
#include <cinttypes>

#include "devices/sd/card_protocol.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "usb/usb_msc.h"

namespace {

// Снимок живёт в SRAM и читается с горячего пути; пишут его события, и
// все они идут из цикла Core1. Поля публикуются раньше поколения, а
// поколение - с release: читатель на другом ядре, увидев новое поколение,
// увидит и поля.
devices::hal::MediaState s_state[2];
std::atomic<uint32_t> s_generation{0};

} // namespace

bool devices::hal::media_init() {
    // Поднимается только карта: флешку поднимает подключение, и ждать её
    // при старте незачем - машина к этому времени уже читает.
    const bool ok = devices::sd::sd_card_init();
    media_state_publish(Medium::Card);
    return ok;
}

SOUNDSINTH_HOT_PATH_ATTR("media_state")
const devices::hal::MediaState& devices::hal::media_state(Medium m) {
    return s_state[m == Medium::Usb ? 1u : 0u];
}

SOUNDSINTH_HOT_PATH_ATTR("media_state_generation")
uint32_t devices::hal::media_state_generation() {
    return s_generation.load(std::memory_order_acquire);
}

void devices::hal::media_state_publish(Medium m) {
    MediaState& st         = s_state[m == Medium::Usb ? 1u : 0u];
    const bool present     = media_present(m);
    const uint32_t sectors = media_sector_count(m);
    if (st.present == present && st.sectors == sectors) return;
    st.present = present;
    st.sectors = sectors;
    s_generation.fetch_add(1, std::memory_order_release);
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

SOUNDSINTH_HOT_PATH_ATTR("media_read_run")
bool devices::hal::media_read_run(Medium m, uint32_t lba, uint32_t count, uint8_t* dst) {
    if (count == 0) return false;
    if (m == Medium::Usb) return rp2350::usb::msc_read_run(lba, count, dst);
    if (count == 1) return devices::sd::sd_card_read_sector(lba, dst);
    // Пачка не собралась - перечитать по одному: сбой одного сектора не
    // должен выглядеть как сбой всей области, а CMD18 бросает её целиком.
    if (devices::sd::sd_card_read_run(lba, count, dst)) return true;
    for (uint32_t i = 0; i < count; ++i) {
        if (!devices::sd::sd_card_read_sector(lba + i, dst + i * kSectorBytes)) return false;
    }
    return true;
}

uint32_t devices::hal::media_run_sectors(Medium /*m*/) {
    // Флешка: замер платы дал T(n) = 469 + 426*n мкс. Шестнадцать секторов -
    // 455 мкс на сектор против 485 при восьми и 895 одиночным; дальше почти
    // ничего. Шестнадцать - это 8 КБ, размер блока кэша.
    //
    // Карта: те же шестнадцать. Экономится не передача (512 байт на 25 МГц
    // идут 164 мкс), а обвязка на каждом секторе - команда, ожидание
    // готовности, разрыв выбора; из них и складывается основная часть
    // цены в 1060 мкс.
    return 16u;
}

bool devices::hal::media_write(Medium m, uint32_t lba, const uint8_t* src) {
    return m == Medium::Usb ? rp2350::usb::msc_write(lba, src) : devices::sd::sd_card_write_sector(lba, src);
}

void devices::hal::media_log_health() {
    devices::sd::sd_card_log_health();
    // Флешка: у неё в журнале не было ни одной строки, а при DISKSYS_MEDIA=usb
    // машина читает именно её. Печатается только когда диск подключали:
    // иначе строка идёт нулями каждый период.
    const uint8_t attached = rp2350::usb::msc_drive_count();
    if (attached == 0) return;
    const rp2350::usb::MscStats m = rp2350::usb::msc_stats();
    debug_logf("usb: disk - attached %u, taken %u, sectors read %" PRIu32 ", written %" PRIu32 "\n", attached, rp2350::usb::msc_drive_taken(), m.reads,
               m.writes);
    // Причины отказов - своей строкой и только когда они есть: в одну строку
    // буфера debug_logf всё это не умещается.
    if (m.failed != 0) {
        debug_logf("usb: disk failures %" PRIu32 " (not started %" PRIu32 ", status %" PRIu32 ", timeout %" PRIu32 ", reentry %" PRIu32 ")\n", m.failed,
                   m.not_started, m.csw_bad, m.timeouts, m.reentry);
    }
}
