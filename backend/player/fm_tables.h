// SPDX-License-Identifier: MIT
#pragma once

// Таблицы FM ядер ymfm: одни на все чипы платы, в .bss. Считаются при
// первом запросе.

namespace ymfm {
struct fm_tables;
}

namespace player {

const ymfm::fm_tables& fm_tables();

} // namespace player
