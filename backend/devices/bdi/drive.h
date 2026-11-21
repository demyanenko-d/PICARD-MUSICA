// SPDX-License-Identifier: MIT
#pragma once

// Дисковод Beta Disk: головка над цилиндром, в нём диск. Дорожки цилиндра
// под головкой берутся у диска; пока диск их читает, дисковод ждёт. Вращение
// и индекс считает ВГ93 формулой от времени. Код - в банках BDI.

#include <cstdint>

#include "devices/bdi/disk.h"

namespace devices::bdi {

struct Drive {
    Disk* disk      = nullptr; // nullptr - пусто
    int cyl         = 0;
    Track* tracks   = nullptr; // дорожки цилиндра (стороны 0 и 1); nullptr - не взяты
    bool waiting    = false;   // диск читает цилиндр
    uint64_t wait_t = 0;       // с какого момента

    // Дорожки под головкой сменились: взять у диска заново.
    void drop() {
        tracks  = nullptr;
        waiting = false;
    }
    // Дорожка стороны side под головкой; nullptr - диска нет или он её ещё
    // читает. started - ожидание началось сейчас, в момент now.
    Track* track(int side, uint64_t now, bool& started);
    // Шаг головки на dir с упором на нуле и на последнем цилиндре: max_cyl
    // или дальше, если диск длиннее. true - головка сдвинулась.
    bool step(int dir, int max_cyl);
    bool write_protect() const { return disk == nullptr || disk->write_protect(); }
};

} // namespace devices::bdi
