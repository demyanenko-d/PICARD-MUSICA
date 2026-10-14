// SPDX-License-Identifier: MIT
#pragma once

// Запись блока настроек перед перезагрузкой ведёт Core0: обработчик
// doorbell ставится на Core0, до запуска Core1.

namespace rp2350::hal {

void boot_save_init();

} // namespace rp2350::hal
