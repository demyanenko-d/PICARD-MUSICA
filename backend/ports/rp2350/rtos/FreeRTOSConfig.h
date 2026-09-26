#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// FreeRTOS для RP2350B (Cortex-M33, порт GCC_ARM_CM33_NTZ_NONSECURE: без
// TrustZone, RP2350 целиком в Secure state). Раскладка по ядрам: Core1 -
// шина и загрузка без FreeRTOS, Core0 - vTaskStartScheduler() и звук.
// Одноядерный FreeRTOS (configNUMBER_OF_CORES=1, не SMP):
// планировщик на ядре, где вызван vTaskStartScheduler(); у каждого ядра
// свой NVIC и SysTick, Core1 это не затрагивает.

#include <stdio.h>
#include <stdlib.h>

// --- Порт Cortex-M33 NTZ, обязательные поля ---
#define configENABLE_FPU                1  // у RP2350 есть FPU
#define configENABLE_MPU                0
#define configENABLE_TRUSTZONE          0  // NTZ = без TrustZone
// RP2350 работает в Secure state: без этого EXC_RETURN указывал бы в
// Non-Secure, и первое же исключение - SecureFault.
#define configRUN_FREERTOS_SECURE_ONLY  1

#define configNUMBER_OF_CORES           1  // одноядерный

// pico-sdk называет векторы isr_svcall/isr_pendsv/isr_systick, а не
// SVC_Handler/PendSV_Handler/SysTick_Handler, как ждёт порт: алиасы
// заданы в CMakeLists.txt (--defsym), проверка установки векторов
// отключена (своих имён она бы не нашла).
#define configCHECK_HANDLER_INSTALLATION 0

// --- Базовые настройки ---
#define configUSE_PREEMPTION            1
#define configUSE_TIME_SLICING          1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0

// 150 МГц при системной 300 МГц (с kSysClockKhz сверяет static_assert):
// SysTick тактуется от ядра (portNVIC_SYSTICK_CLK_BIT), перезагрузка
// считается как configSYSTICK_CLOCK_HZ / configTICK_RATE_HZ от этого
// значения, и тик выходит 0.5 мс против объявленной ядру тысячи герц.
// Само configTICK_RATE_HZ не правится: от него пляшет перезарядка SysTick.
// Настоящую длину тика берёт отсюда platform/os.h - без неё pdMS_TO_TICKS
// отмерял бы вдвое меньше названного.
#define configCPU_CLOCK_HZ              150000000UL
#define configTICK_RATE_HZ              1000  // на деле 2000 Гц
#define SOUNDSINTH_OS_TICK_US           500u
#define configTICK_TYPE_WIDTH_IN_BITS   TICK_TYPE_WIDTH_32_BITS
#define configMAX_PRIORITIES            8
#define configMINIMAL_STACK_SIZE        256   // слов (1 КБ), idle-задача
#define configMAX_TASK_NAME_LEN         16

// --- Куча (heap_4) ---
// app_task (4096 слов = 16 КБ), log_task (2048 слов = 8 КБ), RenderTask
// (SOUNDSINTH_RENDER_TASK_STACK_WORDS = 2048 слов = 8 КБ), idle-задача,
// очереди BufferPool и семафоры RenderTask, TCB и накладные heap_4.
//
// Размер по замеру, а не по расчёту: heap_4 помнит минимум за всё время.
// В режиме эмуляции ПЗУ (без задачи рендера) занято 19968 байт, минимум
// равен текущему - всё выделяется на старте. С играющим треком занято
// 34.6 КБ (free 6384 of 40960 при прежних 40 КБ; прогноз был 28.5 КБ).
// Отсюда 48 КБ.
//
// Строка "heap:" в log_task печатает минимум за всё время -
// смотреть её.
#define configSUPPORT_STATIC_ALLOCATION  0
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE            (48 * 1024)

// --- Возможности ---
#define configUSE_MUTEXES               1
#define configUSE_RECURSIVE_MUTEXES     0
#define configUSE_COUNTING_SEMAPHORES   1  // platform::os_sem_* (RenderTask)
#define configQUEUE_REGISTRY_SIZE       0
#define configUSE_TASK_NOTIFICATIONS    1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configUSE_QUEUE_SETS            0
#define configUSE_CO_ROUTINES           0
#define configUSE_TIMERS                0

// --- Idle-задача ---
#define configUSE_IDLE_HOOK             0
#define configUSE_TICK_HOOK             0
#define configIDLE_SHOULD_YIELD         1

// --- Защита стека ---
#define configCHECK_FOR_STACK_OVERFLOW  2
#define configUSE_MALLOC_FAILED_HOOK    1

// --- Приоритеты прерываний ---
// У RP2350 значимы старшие 4 бита приоритета, configPRIO_BITS = 3 берёт
// старшие три: порог *FromISR 4 << 5 = 0x80. SysTick и PendSV порт пишет
// числом 255, на четырёх битах это 0xF0; configKERNEL_INTERRUPT_PRIORITY
// порт не читает. ISR, вызывающие *FromISR(), - с числом не меньше 0x80.
// IRQ DMA I2S - ровно 0x80: наивысший, совместимый с *FromISR.
#define configPRIO_BITS                 3
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      7
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 4

#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

// --- Статистика и трассировка: выключены ---
#define configGENERATE_RUN_TIME_STATS   0
#define configUSE_TRACE_FACILITY        0

// --- Отображение стандартных функций ---
// Строка с местом (номер строки и адрес), затем bkpt. Имя файла не
// передаётся: полные пути легли бы во флеш.
#ifndef __ASSEMBLER__
#ifdef __cplusplus
extern "C" {
#endif
void soundsinth_assert_failed(int line);
#ifdef __cplusplus
}
#endif
#endif
#define vAssertCalled(file, line)  do { (void)(file); soundsinth_assert_failed(line); } while(0)
#define configASSERT(x)            do { if( (x) == 0 ) vAssertCalled(__FILE__, __LINE__); } while(0)

#define INCLUDE_vTaskDelay                 1
#define INCLUDE_vTaskDelayUntil            1
#define INCLUDE_xTaskGetCurrentTaskHandle  1
#define INCLUDE_vTaskDelete                1
#define INCLUDE_xTaskGetHandle             0
#define INCLUDE_uxTaskGetStackHighWaterMark 1 // запас стеков задач в строках лога
#define INCLUDE_uxTaskPriorityGet          1
#define INCLUDE_vTaskPrioritySet           1
#define INCLUDE_vTaskSuspend               1
#define INCLUDE_xTaskAbortDelay            0

// platform::os_*_from_isr: разбуженная задача выше приоритетом получает ядро
// сразу по выходе из прерывания, а не на следующем тике.
#define SOUNDSINTH_OS_YIELD_FROM_ISR(woken) portYIELD_FROM_ISR(woken)

#endif // FREERTOS_CONFIG_H
