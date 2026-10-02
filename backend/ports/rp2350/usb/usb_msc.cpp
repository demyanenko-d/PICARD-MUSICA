// SPDX-License-Identifier: MIT
#include "usb_msc.h"

#include "tusb.h"

#include <cinttypes>

#include "devices/hal/media.h"
#include "platform/mono_time.h"
#include "platform/log.h"
#include "usb_host.h"

namespace rp2350::usb {
namespace {

// Дисков держим один: второй флешке на плате делать нечего, а каждая
// занимает адрес и буфер. Появится второй - будет видно в журнале.
uint8_t s_addr     = 0;
uint8_t s_lun      = 0;
uint32_t s_sectors = 0;
uint8_t s_drives   = 0;

// Обмен ждёт этих двух: колбэк зовётся из витка стека, то есть из того же
// потока, поэтому ни атомиков, ни барьеров тут не нужно.
bool s_busy = false;
bool s_ok   = false;

// Дольше этого устройство не отвечает, только если его выдернули: у
// флешки сектор идёт единицы миллисекунд.
constexpr uint32_t kTimeoutUs = 2u * 1000u * 1000u;

MscStats s_stats{};

bool on_complete(uint8_t, const tuh_msc_complete_data_t* cb_data) {
    s_ok   = cb_data != nullptr && cb_data->csw != nullptr && cb_data->csw->status == 0u;
    s_busy = false;
    return true;
}

// Ждать обмен, не бросая шину: её обслуживает поставленная функция, а
// стек крутится здесь же - без витка колбэк не придёт никогда.
bool wait_done() {
    const uint32_t start = platform::mono_us();
    while (s_busy) {
        tuh_task();
        yield_to_bus();
        if (platform::mono_us() - start > kTimeoutUs) {
            s_busy = false;
            ++s_stats.timeouts;
            debug_log("usb: disk did not answer, transfer abandoned\n");
            return false;
        }
    }
    // Ненулевой статус CSW: устройство ответило, но отказом.
    if (!s_ok) ++s_stats.csw_bad;
    return s_ok;
}

bool transfer(uint32_t lba, uint32_t count, void* buf, bool write) {
    if (s_addr == 0 || buf == nullptr || count == 0) return false;
    if (s_busy) {
        ++s_stats.reentry;
        ++s_stats.failed;
        return false; // обмен уже идёт: звать можно только из цикла Core1
    }
    s_busy                    = true;
    s_ok                      = false;
    const uint32_t started_us = platform::mono_us();
    const bool started =
        write ? tuh_msc_write10(s_addr, s_lun, buf, lba, count, on_complete, 0) : tuh_msc_read10(s_addr, s_lun, buf, lba, count, on_complete, 0);
    if (!started) {
        s_busy = false;
        ++s_stats.not_started;
        ++s_stats.failed;
        return false;
    }
    const bool ok = wait_done();
    // Считается весь круг, а не одни данные: именно круг и решает,
    // стоит ли просить пачку секторов одной командой.
    const uint32_t spent = platform::mono_us() - started_us;
    if (!ok) {
        ++s_stats.failed;
        return false;
    }
    if (write) {
        ++s_stats.writes;
        s_stats.write_total_us += spent;
        if (spent > s_stats.write_max_us) s_stats.write_max_us = spent;
    } else {
        ++s_stats.reads;
        s_stats.read_total_us += spent;
        if (spent > s_stats.read_max_us) s_stats.read_max_us = spent;
    }
    return true;
}

} // namespace

MscStats msc_stats() {
    return s_stats;
}

void msc_task() {
    // Своего витка у MSC нет: всё делает tuh_task. Функция оставлена
    // точкой, где появится фоновая работа с диском.
    //
    // Длинных обменов здесь быть не должно: момент появления диска -
    // середина перечисления следующего устройства. Замерочная
    // самопроверка на шестнадцать чтений занимала виток на 29 мс, и
    // клавиатура в это окно не попадала вовсе.
}

bool msc_present() {
    return s_addr != 0 && tuh_msc_mounted(s_addr);
}

uint32_t msc_sector_count() {
    return s_sectors;
}

uint8_t msc_drive_taken() {
    return s_addr != 0 ? 1u : 0u;
}

uint8_t msc_drive_count() {
    return s_drives;
}

bool msc_read(uint32_t lba, uint8_t* dst) {
    return transfer(lba, 1, dst, false);
}

bool msc_read_run(uint32_t lba, uint32_t count, uint8_t* dst) {
    return transfer(lba, count, dst, false);
}

bool msc_write(uint32_t lba, const uint8_t* src) {
    return transfer(lba, 1, const_cast<uint8_t*>(src), true);
}

void msc_note_mount(uint8_t dev_addr) {
    ++s_drives;
    if (s_addr != 0) {
        debug_logf("usb: second disk %u not taken, %u is in use\n", dev_addr, s_addr);
        return;
    }
    s_addr               = dev_addr;
    s_lun                = 0;
    const uint32_t block = tuh_msc_get_block_size(dev_addr, s_lun);
    s_sectors            = tuh_msc_get_block_count(dev_addr, s_lun);
    if (block != kMscSectorBytes) {
        // Сектор не 512 байт: файловая система платы такого не читает.
        debug_logf("usb: disk %u with a %" PRIu32 " byte sector - not taken\n", dev_addr, block);
        s_addr    = 0;
        s_sectors = 0;
        return;
    }
    devices::hal::media_state_publish(devices::hal::Medium::Usb);
    debug_logf("usb: disk %u, sectors %" PRIu32 " of %" PRIu32 " bytes\n", dev_addr, s_sectors, block);
}

void msc_note_umount(uint8_t dev_addr) {
    if (s_drives > 0) --s_drives;
    if (dev_addr != s_addr) return;
    s_addr    = 0;
    s_sectors = 0;
    s_busy    = false;
    devices::hal::media_state_publish(devices::hal::Medium::Usb);
    debug_logf("usb: disk %u detached\n", dev_addr);
}

} // namespace rp2350::usb

extern "C" {

void tuh_msc_mount_cb(uint8_t dev_addr) {
    rp2350::usb::msc_note_mount(dev_addr);
}

void tuh_msc_umount_cb(uint8_t dev_addr) {
    rp2350::usb::msc_note_umount(dev_addr);
}

} // extern "C"
