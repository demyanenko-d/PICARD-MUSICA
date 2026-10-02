// SPDX-License-Identifier: MIT
#pragma once

// Задача логгера на Core0: расписание периодических строк.

#include "FreeRTOS.h"
#include "task.h"

namespace rp2350 {

// Тело задачи FreeRTOS. Не возвращается.
void log_task(void* arg);

// Хэндл задачи синтезатора - для её запаса стека в строке heap:.
void log_task_set_app_handle(TaskHandle_t h);

} // namespace rp2350
