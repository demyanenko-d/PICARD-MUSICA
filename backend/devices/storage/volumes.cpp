// SPDX-License-Identifier: MIT
#include "devices/storage/volumes.h"

namespace devices::storage {

namespace {

FATFS* s_fs[FF_VOLUMES];                              // смонтированный объект тома
uint32_t s_sectors[FF_VOLUMES], s_serial[FF_VOLUMES]; // носитель на монтировании

constexpr hal::Medium kMedium[kVolumeCount] = {hal::Medium::Card, hal::Medium::Usb};

} // namespace

hal::Medium volume_medium(uint8_t drive) {
    return kMedium[drive];
}

const hal::Medium* volume_medium_ptr(uint8_t drive) {
    return &kMedium[drive];
}

bool volume_present(uint8_t drive) {
    return hal::media_state(kMedium[drive]).present;
}

void volume_remember_medium(uint8_t drive) {
    const hal::MediaState& m = hal::media_state(kMedium[drive]);
    s_sectors[drive]         = m.sectors;
    s_serial[drive]          = m.serial;
}

bool volume_same_medium(uint8_t drive) {
    const hal::MediaState& m = hal::media_state(kMedium[drive]);
    return m.sectors == s_sectors[drive] && m.serial == s_serial[drive];
}

// Том уже смонтирован: окно FatFs - копия сектора с прошлого обращения, а
// машина через эмулятор карты и CHECK CARD сырой записью могли его
// переписать. Чистое окно сбрасывается (сектор перечитается), число
// свободных кластеров - неизвестно; иначе сброс окна или FSINFO вернул бы
// на носитель старый сектор FAT.
bool volume_attach(uint8_t drive, FATFS* fs) {
    if (drive >= FF_VOLUMES) return false;
    if (FATFS* const m = s_fs[drive]) {
        if (m->wflag == 0) m->winsect = static_cast<LBA_t>(0) - 1u;
        m->free_clst = 0xFFFFFFFFu;
        return true;
    }
    if (f_mount(fs, volume_prefix(drive), 1) != FR_OK) {
        f_mount(nullptr, volume_prefix(drive), 0);
        return false;
    }
    s_fs[drive] = fs;
    return true;
}

bool volume_mounted(uint8_t drive) {
    return drive < FF_VOLUMES && s_fs[drive] != nullptr;
}

void volume_detach(uint8_t drive) {
    if (drive >= FF_VOLUMES || s_fs[drive] == nullptr) return;
    f_mount(nullptr, volume_prefix(drive), 0);
    s_fs[drive] = nullptr;
}

} // namespace devices::storage
