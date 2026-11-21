// SPDX-License-Identifier: MIT
#pragma once

// Дисковод для ВГ93: дорожка - кольцо слотов по 2 такта CLK (2 мкс при
// CLK = 1 МГц), установленный бит - переход потока (импульс RAWREAD). Ячейка
// MFM занимает слот, FM - два. Оборот при 300 об/мин - 100 000 слотов,
// 12 500 байт на сторону, младший разряд байта - первый слот.
//
// Потоки дорожек хранит владелец дисковода: контроллер видит только
// дорожку под головкой. При шаге и смене стороны владелец подставляет
// новую через track_for.

#include <cstdint>

namespace devices::bdi {

inline constexpr uint32_t kSlotsPerRev = 100000;
inline constexpr uint32_t kTrackBytes  = (kSlotsPerRev + 7) / 8;
inline constexpr uint32_t kIndexSlots  = kSlotsPerRev / 50; // 4 мс
inline constexpr int kMaxCylinders     = 86;

struct SlotDrive;

// Поток дорожки cyl/side или nullptr - не размечена. create - запись в
// неразмеченную дорожку: вернуть чистую.
using TrackForFn = uint8_t* (*)(void* user, SlotDrive& d, int cyl, int side, bool create);

// Вход дисковода прижат: Drive - от дисковода, On - активен, Off - нет.
enum class Force : uint8_t { Drive, On, Off };

struct SlotDrive {
    uint32_t slots     = kSlotsPerRev;
    uint32_t pos       = 0; // слот под головкой
    uint32_t index_len = kIndexSlots;
    int cyl = 0, side = 0, ncyl = 80;
    bool wprot       = false;
    bool ready       = true;
    bool tr00_broken = false; // /TR00 не срабатывает никогда
    Force ip_force = Force::Drive, tr00_force = Force::Drive;
    // Выбор дисковода: снятый не отдаёт ни данных, ни индекса, ни TR00, ни
    // WPRT и не шагает. У Beta Disk Пентагона выбор разрешается HLD
    // контроллера (select_from_hld), ядро обновляет его по HLD в конце каждого слова.
    bool selected        = true;
    bool select_from_hld = false;
    bool disk            = true; // без диска нет индекса
    uint8_t written      = 0;    // стороны с записью: разряд на сторону

    uint8_t* track       = nullptr; // дорожка под головкой
    TrackForFn track_for = nullptr;
    void* user           = nullptr;

    // Дорожку под головкой взять заново: после шага, смены стороны, смены
    // образа.
    void refresh() { track = track_for ? track_for(user, *this, cyl, side, false) : nullptr; }

    bool get(uint32_t p) const { return selected && track != nullptr && ((track[p >> 3] >> (p & 7)) & 1u) != 0; }
    void put(uint32_t p, bool v) {
        if (!selected) return;
        if (track == nullptr && track_for != nullptr) track = track_for(user, *this, cyl, side, true);
        if (track == nullptr) return;
        if (v) {
            track[p >> 3] = static_cast<uint8_t>(track[p >> 3] | (1u << (p & 7)));
        } else {
            track[p >> 3] = static_cast<uint8_t>(track[p >> 3] & ~(1u << (p & 7)));
        }
        written = static_cast<uint8_t>(written | (1u << side));
    }
    void step(int dir) {
        if (!selected) return;
        cyl += dir;
        if (cyl < 0) cyl = 0;
        // Головка встаёт на последнем цилиндре дисковода.
        if (cyl > ncyl - 1) cyl = ncyl - 1;
        refresh();
    }
    void set_side(int s) {
        if (s == side) return;
        side = s;
        refresh();
    }
    bool index_at(uint32_t p) const { return ip_force != Force::Drive ? ip_force == Force::On : (selected && disk && p < index_len); }
    // Индекс идёт от вращения: /IP не прижат, дисковод выбран и диск вставлен.
    bool index_spins() const { return ip_force == Force::Drive && selected && disk; }
    bool index() const { return index_at(pos); }
    bool tr00() const { return tr00_force != Force::Drive ? tr00_force == Force::On : (selected && cyl == 0 && !tr00_broken); }
    bool wprot_in() const { return selected && wprot; }
};

} // namespace devices::bdi
