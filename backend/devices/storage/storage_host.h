// SPDX-License-Identifier: MIT
#pragma once

// Запись носителя хостом.
//
// Отдельный заголовок, чтобы этот путь был виден только эмулятору карты.
// Запись насквозь, грязных секторов не бывает.

#include <cstdint>

#include "devices/storage/storage.h"

namespace devices::storage {

// Записать сектор на носитель хозяина. Номер хозяина обязателен: стороны
// смотрят в разные носители, и писать надо туда же, откуда читали.
// Запись насквозь, кэш обновляется: хост сразу видит своё значение.
bool storage_write(uint8_t owner, uint32_t lba, const uint8_t* src);

} // namespace devices::storage
