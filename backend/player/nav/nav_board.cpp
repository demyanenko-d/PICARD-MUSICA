// SPDX-License-Identifier: MIT
// Навигатор образов на плате: каталоги носителей через FatFs, действия с
// дисководами - через Beta Disk, страница обмена - в цикле Core1.

#include <new>

#include "ff.h"

#include "platform/compiler.h"

#include "devices/bdi/bdi.h"
#include "devices/hal/z80_ports.h"
#include "devices/storage/storage.h"
#include "devices/storage/volumes.h"

#include "player/nav/nav.h"

namespace player::nav {

using namespace devices;
namespace {

constexpr int kItems = 1024;

// Состояние - в PSRAM. Память последних - отдельно и не конструируется:
// переживает сброс платы.
struct State {
    Navigator nav;
    Place picked;
    FATFS fs[storage::kVolumeCount];
    Item items[kItems];
};

State* s_state = nullptr;
Memory* s_mem  = nullptr;

static_assert(kVolSd == storage::kVolumeCard && kVolUsb == storage::kVolumeUsb && kVolWifi == storage::kVolumeCount,
              "navigator volumes are FatFs volumes, then WIFI");
static_assert(kNameBytes > FF_LFN_BUF, "a navigator item holds the whole long name");

bool present(int vol) {
    return storage::volume_present(static_cast<uint8_t>(vol));
}

int list(int vol, const char* path, Item* out, int cap) {
    if (!storage::volume_attach(static_cast<uint8_t>(vol), &s_state->fs[vol])) return -1;
    char full[3 + kPathBytes];
    cut(full, sizeof(full), {storage::volume_prefix(static_cast<uint8_t>(vol)), path});
    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, full) != FR_OK) return -1;
    int n = 0;
    while (n < cap && f_readdir(&dir, &fi) == FR_OK && fi.fname[0] != 0) {
        if (fi.fname[0] == '.' || (fi.fattrib & (AM_HID | AM_SYS)) != 0) continue;
        const Kind kind = (fi.fattrib & AM_DIR) != 0 ? Kind::Dir : bdi::bdi_image_name(fi.fname) ? Kind::Image : Kind::File;
        Item& it        = out[n++];
        it              = Item{kind, static_cast<uint8_t>(vol), true, {}, false, static_cast<uint32_t>(fi.fsize)};
        cut(it.name, sizeof(it.name), {fi.fname});
    }
    f_closedir(&dir);
    return n;
}

// Том подключается и здесь: RECENT ведёт к образу без захода в каталог
// (банк во флеше, последний каталог на другом носителе, перезапуск платы).
bool mount(int drive, const Place& p, bool read_only) {
    if (!storage::volume_attach(p.vol, &s_state->fs[p.vol])) return false;
    char full[3 + kPathBytes];
    cut(full, sizeof(full), {storage::volume_prefix(p.vol), p.path});
    return bdi::bdi_mount(drive, p.vol, full, read_only);
}

// Диск TR-DOS из отмеченных файлов каталога навигатора. Ответ -
// nav_page::Result; файл, на котором сборка встала, - в bad.
uint8_t mount_set(int drive, const Navigator& nv, int& bad) {
    const uint8_t vol     = nv.where().vol;
    const char* const dir = nv.where().path;
    if (!bdi::bdi_set_begin(drive, vol)) return nav_page::Fail;
    for (int i = 0; i < nv.count(); i++) {
        const Item& it = nv.item(i);
        if (!it.marked) continue;
        char full[3 + kPathBytes + 1 + kNameBytes];
        cut(full, sizeof(full), {storage::volume_prefix(vol), dir, dir[1] != 0 ? "/" : "", it.name});
        const bdi::SetAdd r = bdi::bdi_set_add(full, it.name, it.size);
        if (r != bdi::SetAdd::Ok) {
            bad = i;
            return r == bdi::SetAdd::NoFile ? nav_page::NoFile : nav_page::Fragments;
        }
    }
    char where[3 + kPathBytes];
    cut(where, sizeof(where), {storage::volume_prefix(vol), dir});
    bdi::bdi_set_end(where);
    return nav_page::Ok;
}

// Отдельно от nav_reserve: конструктор навигатора - большой кадр стека.
SOUNDSINTH_NOINLINE void setup(uint8_t* mem) {
    s_state = new (mem) State;
    s_state->nav.init(s_state->items, kItems, s_mem, Media{&present, &list});
}

} // namespace

bool nav_reserve(bdi::PsramTakeFn take) {
    s_mem            = reinterpret_cast<Memory*>(take((sizeof(Memory) + 7u) & ~size_t{7}));
    uint8_t* const m = take(sizeof(State));
    if (s_mem == nullptr || m == nullptr) return false;
    setup(m);
    return true;
}

void nav_poll() {
    if (bdi::g_bdi_host == nullptr || s_state == nullptr) return;
    if (uint8_t* const page = hal::z80_nav_page(); page != nullptr) {
        serve(s_state->nav, s_state->picked, page, hal::z80_port_7ffd(), Board{&mount, &bdi::bdi_eject, &bdi::bdi_drive_name, &mount_set});
    }
}

} // namespace player::nav
