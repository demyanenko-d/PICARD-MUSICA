// SPDX-License-Identifier: MIT
#include "platform/os.h"

#include "platform/hot_path.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

// Реализация platform/os.h, единственная: собирают её оба порта (плата -
// GCC_ARM_CM33_NTZ_NONSECURE, ПК - симулятор Windows). Различие между ними
// одно, макрос SOUNDSINTH_OS_YIELD_FROM_ISR из FreeRTOSConfig.h.

// Настоящая длина тика. Порт объявляет её, когда она разошлась с
// configTICK_RATE_HZ: на плате SysTick считает от configCPU_CLOCK_HZ, вдвое
// меньшей системной частоты, и тик выходит 500 мкс при объявленной тысяче
// герц. pdMS_TO_TICKS верит объявленному и отмерил бы половину.
#ifndef SOUNDSINTH_OS_TICK_US
#define SOUNDSINTH_OS_TICK_US (1000000u / configTICK_RATE_HZ)
#endif

namespace platform {

// Тиков на миллисекунды, с округлением вверх: срок не должен выйти раньше
// названного. Счёт 32-битный, потолок - 4294 секунды: паузы задач и сроки
// ожидания здесь измеряются десятками миллисекунд.
namespace {
TickType_t ticks_for_ms(uint32_t ms) {
    const uint32_t us = ms * 1000u;
    return static_cast<TickType_t>((us + SOUNDSINTH_OS_TICK_US - 1u) / SOUNDSINTH_OS_TICK_US);
}
} // namespace

// Semaphore и Queue из os.h - сам хэндл FreeRTOS под другим именем: хэндл
// уже указатель на непрозрачную структуру, обёртка в куче не нужна.
namespace {
SemaphoreHandle_t handle(Semaphore* sem) {
    return reinterpret_cast<SemaphoreHandle_t>(sem);
}
QueueHandle_t handle(Queue* q) {
    return reinterpret_cast<QueueHandle_t>(q);
}
// Обратно - разными именами: SemaphoreHandle_t и QueueHandle_t - один тип.
Semaphore* as_semaphore(SemaphoreHandle_t h) {
    return reinterpret_cast<Semaphore*>(h);
}
Queue* as_queue(QueueHandle_t h) {
    return reinterpret_cast<Queue*>(h);
}
} // namespace

// Место семафоров и очередей - статический пул, а не куча: число их
// известно и невелико, а на плате кучи нет вовсе. Слот ищется линейно -
// заводят их считанные разы за трек, не в горячем пути.
//
// Пул кончился - остановка на configASSERT прямо на заведении: молча
// вернуть nullptr значило бы отказ звука где-то дальше и без причины.
namespace {

constexpr uint32_t kSemSlots = 12;
StaticSemaphore_t s_sem_slots[kSemSlots];
bool s_sem_busy[kSemSlots];

constexpr uint32_t kQueueSlots = 4;
// Потолок ёмкости: очереди пула буферов - kBufferCount и глубина готовых.
constexpr uint32_t kQueueCapacity = 8;
StaticQueue_t s_queue_slots[kQueueSlots];
void* s_queue_items[kQueueSlots][kQueueCapacity];
bool s_queue_busy[kQueueSlots];

} // namespace

Semaphore* os_sem_create(uint32_t initial_count, uint32_t max_count) {
    uint32_t slot = 0;
    while (slot < kSemSlots && s_sem_busy[slot]) {
        ++slot;
    }
    configASSERT(slot < kSemSlots);
    const SemaphoreHandle_t h = xSemaphoreCreateCountingStatic(max_count, initial_count, &s_sem_slots[slot]);
    configASSERT(h != nullptr);
    s_sem_busy[slot] = true;
    return as_semaphore(h);
}

void os_sem_destroy(Semaphore* sem) {
    const SemaphoreHandle_t h = handle(sem);
    // Хэндл статического семафора - это и есть адрес его слота.
    for (uint32_t i = 0; i < kSemSlots; ++i) {
        if (reinterpret_cast<void*>(h) == static_cast<void*>(&s_sem_slots[i])) {
            s_sem_busy[i] = false;
            break;
        }
    }
    vSemaphoreDelete(h);
}

void os_sem_wait(Semaphore* sem) {
    xSemaphoreTake(handle(sem), portMAX_DELAY);
}

bool os_sem_wait_ms(Semaphore* sem, uint32_t ms) {
    return xSemaphoreTake(handle(sem), ticks_for_ms(ms)) == pdTRUE;
}

void os_sem_post(Semaphore* sem) {
    xSemaphoreGive(handle(sem));
}

bool os_sem_try_wait(Semaphore* sem) {
    return xSemaphoreTake(handle(sem), 0) == pdTRUE;
}

#if configSUPPORT_DYNAMIC_ALLOCATION == 1
// Только там, где куча есть: на плате её нет, и обращение сюда должно быть
// ошибкой компоновки, а не отказом выделения посреди работы.
void os_task_create(TaskFn fn, void* arg, const char* name, uint32_t stack_words, uint32_t priority) {
    const BaseType_t ok =
        xTaskCreate(fn, name, static_cast<configSTACK_DEPTH_TYPE>(stack_words), arg, static_cast<UBaseType_t>(tskIDLE_PRIORITY + priority), nullptr);
    configASSERT(ok == pdPASS);
}
#endif

// Место постоянных задач. Массивы здесь, а не у вызывающего, потому что
// ширина слова стека - свойство порта: на плате четыре байта, в симуляторе
// Windows восемь.
namespace {

constexpr uint32_t kRoleCount = static_cast<uint32_t>(TaskRole::Count);
StackType_t s_stack_render[kOsTaskStackWords[static_cast<uint32_t>(TaskRole::Render)]];
StackType_t s_stack_seq[kOsTaskStackWords[static_cast<uint32_t>(TaskRole::Sequencer)]];
StackType_t* const s_stacks[kRoleCount] = {s_stack_render, s_stack_seq};
StaticTask_t s_tcbs[kRoleCount];
// Хэндлы держатся ради запаса стека: задача-логгер меряет чужие, а не свой.
TaskHandle_t s_role_handles[kRoleCount] = {};

} // namespace

void os_task_create_static(TaskRole role, TaskFn fn, void* arg, const char* name, uint32_t stack_words, uint32_t priority) {
    const uint32_t i = static_cast<uint32_t>(role);
    configASSERT(i < kRoleCount);
    // Просьба сверх потолка роли - остановка здесь: молча урезать стек
    // значило бы его переполнение под нагрузкой.
    configASSERT(stack_words <= kOsTaskStackWords[i]);
    const TaskHandle_t h = xTaskCreateStatic(fn, name, static_cast<configSTACK_DEPTH_TYPE>(stack_words), arg,
                                             static_cast<UBaseType_t>(tskIDLE_PRIORITY + priority), s_stacks[i], &s_tcbs[i]);
    configASSERT(h != nullptr);
    s_role_handles[i] = h;
}

uint32_t os_task_stack_unused_bytes(TaskRole role) {
    const uint32_t i = static_cast<uint32_t>(role);
    if (i >= kRoleCount || s_role_handles[i] == nullptr) return 0;
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_role_handles[i])) * sizeof(StackType_t);
}

void os_task_delay_ms(uint32_t ms) {
    vTaskDelay(ticks_for_ms(ms));
}

void os_task_delete_self() {
    vTaskDelete(nullptr);
}

uint32_t os_task_stack_unused_bytes() {
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) * sizeof(StackType_t);
}

Queue* os_queue_create(uint32_t capacity) {
    configASSERT(capacity <= kQueueCapacity);
    uint32_t slot = 0;
    while (slot < kQueueSlots && s_queue_busy[slot]) {
        ++slot;
    }
    configASSERT(slot < kQueueSlots);
    const QueueHandle_t h = xQueueCreateStatic(capacity, sizeof(void*), reinterpret_cast<uint8_t*>(s_queue_items[slot]), &s_queue_slots[slot]);
    configASSERT(h != nullptr);
    s_queue_busy[slot] = true;
    return as_queue(h);
}

void os_queue_destroy(Queue* q) {
    const QueueHandle_t h = handle(q);
    for (uint32_t i = 0; i < kQueueSlots; ++i) {
        if (reinterpret_cast<void*>(h) == static_cast<void*>(&s_queue_slots[i])) {
            s_queue_busy[i] = false;
            break;
        }
    }
    vQueueDelete(h);
}

void os_queue_send(Queue* q, void* item) {
    xQueueSend(handle(q), &item, portMAX_DELAY);
}

void* os_queue_receive(Queue* q) {
    void* item = nullptr;
    xQueueReceive(handle(q), &item, portMAX_DELAY);
    return item;
}

bool os_queue_try_receive(Queue* q, void** out_item) {
    return xQueueReceive(handle(q), out_item, 0) == pdTRUE;
}

bool SOUNDSINTH_HOT_PATH(os_queue_try_receive_from_isr)(Queue* q, void** out_item) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    const bool ok                         = xQueueReceiveFromISR(handle(q), out_item, &higher_priority_task_woken) == pdTRUE;
    SOUNDSINTH_OS_YIELD_FROM_ISR(higher_priority_task_woken);
    return ok;
}

bool SOUNDSINTH_HOT_PATH(os_queue_send_from_isr)(Queue* q, void* item) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    const bool ok                         = xQueueSendFromISR(handle(q), &item, &higher_priority_task_woken) == pdTRUE;
    SOUNDSINTH_OS_YIELD_FROM_ISR(higher_priority_task_woken);
    return ok;
}

uint32_t SOUNDSINTH_HOT_PATH(os_queue_count_from_isr)(Queue* q) {
    return static_cast<uint32_t>(uxQueueMessagesWaitingFromISR(handle(q)));
}

} // namespace platform
