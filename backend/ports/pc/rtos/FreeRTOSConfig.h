// SPDX-License-Identifier: MIT
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// Минимальная конфигурация FreeRTOS для Windows-симулятора (порт
// MSVC_MINGW), на котором стоит platform/os.h на PC. Урезана до того, что
// использует SoundSinth: счётные семафоры и задачи. Конфигурация платы -
// backend/ports/rp2350/freertos_config/FreeRTOSConfig.h.

#include <stdio.h>
#include <stdlib.h>

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      ((unsigned long)20000000)
#define configTICK_RATE_HZ                      (1000) /* симулятор - не real-time */
#define configMAX_PRIORITIES                    (5)
#define configMINIMAL_STACK_SIZE                ((unsigned short)128)
#define configMAX_TASK_NAME_LEN                 (16)
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1

#define configUSE_MUTEXES                     1
#define configUSE_RECURSIVE_MUTEXES           0
#define configUSE_COUNTING_SEMAPHORES         1
#define configQUEUE_REGISTRY_SIZE             0
#define configUSE_QUEUE_SETS                  0
#define configUSE_TASK_NOTIFICATIONS          1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1

#define configSUPPORT_STATIC_ALLOCATION     1
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define configTOTAL_HEAP_SIZE               ((size_t)(64 * 1024)) /* только объекты FreeRTOS (TCB/семафоры) - арены проекта отдельно, через platform::memory.h */

#define configUSE_IDLE_HOOK                0
#define configUSE_TICK_HOOK                0
#define configUSE_MALLOC_FAILED_HOOK       0
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0
#define configCHECK_FOR_STACK_OVERFLOW     0
#define configUSE_TRACE_FACILITY           0
#define configGENERATE_RUN_TIME_STATS      0

#define configUSE_TIMERS 0

#define configASSERT(x)                                                                                                                                        \
    if ((x) == 0) {                                                                                                                                            \
        fprintf(stderr, "FreeRTOS configASSERT failed: %s:%d\n", __FILE__, __LINE__);                                                                          \
        abort();                                                                                                                                               \
    }

#define INCLUDE_vTaskDelay                  1
#define INCLUDE_vTaskDelete                 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_eTaskGetState               1

/* platform::os_*_from_isr: зовёт задача в роли прерывания внутри критической
 * секции; разбуженная задача получит процессор, когда вызывающая
 * заблокируется или на ближайшем тике. */
#define SOUNDSINTH_OS_YIELD_FROM_ISR(woken) ((void)(woken))

#endif /* FREERTOS_CONFIG_H */
