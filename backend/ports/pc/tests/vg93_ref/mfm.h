// SPDX-License-Identifier: MIT
#pragma once

// Дорожка MFM как поток слотов дисковода, ячейка на слот: кодирование из
// списка секторов, разбор обратно и перевод из байтов дорожки (Track).
// Нужно образам ПК и сверке с Vg93w; на плате этого кода нет.

#include <cstddef>
#include <cstdint>

#include "devices/bdi/disk.h"

namespace devices::bdi {

// Дорожка IBM: track_build, затем track_to_cells; bits - kTrackBytes.
// Хвост, не влезший в оборот, отбрасывается.
void build_mfm_track(uint8_t* bits, const SectorSpec* s, int count, int gap3);

struct FoundSector {
    uint8_t c, h, r, n;
    bool id_crc_ok;
    bool has_data, data_crc_ok, deleted;
    uint32_t data_off; // смещение данных в буфере decode_mfm_track
};

// Сектора дорожки MFM по меткам A1 A1 A1 + FE / F8..FB. Данные кладутся
// подряд в data (до data_cap байт; сектор, который не влез, остаётся без
// данных). Возвращает число найденных заголовков (не больше cap).
int decode_mfm_track(const uint8_t* bits, uint32_t slots, FoundSector* out, int cap, uint8_t* data, size_t data_cap);

// Поток ячеек (kTrackBytes) и обратно. Байты - по сетке 16 ячеек от начала
// оборота.
void track_from_cells(Track& t, const uint8_t* bits);
void track_to_cells(const Track& t, uint8_t* bits);

} // namespace devices::bdi
