// SPDX-License-Identifier: MIT
#pragma once

// Тома FatFs после загрузки: карта и флешка. Регистрирует том
// первый, кому он нужен, своим FATFS; остальные ходят по уже
// зарегистрированному: повторная регистрация сломала бы открытые файлы
// первого (банк с карты держит свой файл открытым всё время работы).

#include <cstdint>

#include "ff.h"

#include "devices/hal/media.h"

namespace devices::storage {

inline constexpr uint8_t kVolumeCard  = 0;
inline constexpr uint8_t kVolumeUsb   = 1;
inline constexpr uint8_t kVolumeCount = 2;
static_assert(kVolumeCount <= FF_VOLUMES, "a FatFs volume per medium");

// Том drive: уже есть - true сразу; иначе зарегистрировать с fs и
// смонтировать. false - на носителе нет FAT, регистрация снята.
bool volume_attach(uint8_t drive, FATFS* fs);
void volume_detach(uint8_t drive);
// Том смонтирован кем-то: снимать его может только тот, кто монтировал.
bool volume_mounted(uint8_t drive);

// Путь тома: "0:" или "1:".
inline const char* volume_prefix(uint8_t drive) {
    return drive == kVolumeCard ? "0:" : "1:";
}

// Правило "том 0 - карта, том 1 - флешка" живёт здесь: потребители не
// переводят номер сами. volume_medium_ptr - тот же носитель по адресу, для
// MediaIo::user.
hal::Medium volume_medium(uint8_t drive);
const hal::Medium* volume_medium_ptr(uint8_t drive);
// Носитель тома вставлен (у карты - по арбитру).
bool volume_present(uint8_t drive);
// Носитель тома запомнить (монтирование, disk_initialize) и сверить:
// другая карта или флешка под тем же диском - ёмкость или номер иные.
void volume_remember_medium(uint8_t drive);
bool volume_same_medium(uint8_t drive);

} // namespace devices::storage
