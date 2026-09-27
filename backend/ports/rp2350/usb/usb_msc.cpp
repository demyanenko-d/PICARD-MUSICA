#include "usb_msc.h"

#include "tusb.h"

#include <cinttypes>

#include "platform/mono_time.h"
#include "platform/log.h"
#include "usb_host.h"

namespace rp2350::usb {
namespace {

// Дисков держим один: второй флешке на плате делать нечего, а каждая
// занимает адрес и буфер. Появится второй - будет видно в журнале.
uint8_t s_addr = 0;
uint8_t s_lun = 0;
uint32_t s_sectors = 0;
uint8_t s_drives = 0;


// Обмен ждёт этих двух: колбэк зовётся из витка стека, то есть из того же
// потока, поэтому ни атомиков, ни барьеров тут не нужно.
bool s_busy = false;
bool s_ok = false;

// Дольше этого устройство не отвечает, только если его выдернули: у
// флешки сектор идёт единицы миллисекунд.
constexpr uint32_t kTimeoutUs = 2u * 1000u * 1000u;

bool on_complete(uint8_t, const tuh_msc_complete_data_t* cb_data) {
    s_ok = cb_data != nullptr && cb_data->csw != nullptr && cb_data->csw->status == 0u;
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
            debug_log("usb: диск не ответил, обмен брошен\n");
            return false;
        }
    }
    return s_ok;
}

bool transfer(uint32_t lba, void* buf, bool write) {
    if (s_addr == 0 || buf == nullptr) return false;
    if (s_busy) return false; // обмен уже идёт: звать можно только из цикла Core1
    s_busy = true;
    s_ok = false;
    const bool started = write ? tuh_msc_write10(s_addr, s_lun, buf, lba, 1, on_complete, 0)
                               : tuh_msc_read10(s_addr, s_lun, buf, lba, 1, on_complete, 0);
    if (!started) {
        s_busy = false;
        return false;
    }
    return wait_done();
}

} // namespace

void msc_task() {
    // Своего витка у MSC нет: всё делает tuh_task. Функция оставлена
    // точкой, где появится фоновая работа с диском.
}

bool msc_present() {
    return s_addr != 0 && tuh_msc_mounted(s_addr);
}

uint32_t msc_sector_count() {
    return s_sectors;
}

uint8_t msc_drive_count() {
    return s_drives;
}

bool msc_read(uint32_t lba, uint8_t* dst) {
    return transfer(lba, dst, false);
}

bool msc_write(uint32_t lba, const uint8_t* src) {
    return transfer(lba, const_cast<uint8_t*>(src), true);
}


void msc_note_mount(uint8_t dev_addr) {
    ++s_drives;
    if (s_addr != 0) {
        debug_logf("usb: второй диск %u не берётся, работает %u\n", dev_addr, s_addr);
        return;
    }
    s_addr = dev_addr;
    s_lun = 0;
    const uint32_t block = tuh_msc_get_block_size(dev_addr, s_lun);
    s_sectors = tuh_msc_get_block_count(dev_addr, s_lun);
    if (block != kMscSectorBytes) {
        // Сектор не 512 байт: файловая система платы такого не читает.
        debug_logf("usb: диск %u с сектором %" PRIu32 " байт - не берём\n", dev_addr, block);
        s_addr = 0;
        s_sectors = 0;
        return;
    }
    debug_logf("usb: диск %u, секторов %" PRIu32 " по %" PRIu32 " байт\n", dev_addr, s_sectors, block);
}

void msc_note_umount(uint8_t dev_addr) {
    if (s_drives > 0) --s_drives;
    if (dev_addr != s_addr) return;
    s_addr = 0;
    s_sectors = 0;
    s_busy = false;
    debug_logf("usb: диск %u отключён\n", dev_addr);
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
