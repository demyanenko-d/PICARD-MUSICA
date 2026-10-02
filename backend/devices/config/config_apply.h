// SPDX-License-Identifier: MIT
#pragma once

// Разложить настройки по устройствам.
//
// Отдельно от config_boot, потому что применяется не всё сразу: роли
// модификаторов и настройка мыши ложатся один раз при старте, а признаки
// "включено" спрашиваются там, где устройство поднимается.

#include "core/config/config.h"

namespace devices::config {

void config_apply(const soundsinth::config::Settings& s);

} // namespace devices::config
