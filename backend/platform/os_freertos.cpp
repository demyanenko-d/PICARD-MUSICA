#include "platform/os.h"

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
SemaphoreHandle_t handle(Semaphore* sem) { return reinterpret_cast<SemaphoreHandle_t>(sem); }
QueueHandle_t handle(Queue* q) { return reinterpret_cast<QueueHandle_t>(q); }
// Обратно - разными именами: SemaphoreHandle_t и QueueHandle_t - один тип.
Semaphore* as_semaphore(SemaphoreHandle_t h) { return reinterpret_cast<Semaphore*>(h); }
Queue* as_queue(QueueHandle_t h) { return reinterpret_cast<Queue*>(h); }
} // namespace

Semaphore* os_sem_create(uint32_t initial_count, uint32_t max_count) {
    const SemaphoreHandle_t h = xSemaphoreCreateCounting(max_count, initial_count);
    configASSERT(h != nullptr);
    return as_semaphore(h);
}

void os_sem_destroy(Semaphore* sem) {
    vSemaphoreDelete(handle(sem));
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

void os_task_create(TaskFn fn, void* arg, const char* name, uint32_t stack_words, uint32_t priority) {
    const BaseType_t ok =
        xTaskCreate(fn, name, static_cast<configSTACK_DEPTH_TYPE>(stack_words), arg,
                    static_cast<UBaseType_t>(tskIDLE_PRIORITY + priority), nullptr);
    configASSERT(ok == pdPASS);
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
    const QueueHandle_t h = xQueueCreate(capacity, sizeof(void*));
    configASSERT(h != nullptr);
    return as_queue(h);
}

void os_queue_destroy(Queue* q) {
    vQueueDelete(handle(q));
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

bool os_queue_try_receive_from_isr(Queue* q, void** out_item) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    const bool ok = xQueueReceiveFromISR(handle(q), out_item, &higher_priority_task_woken) == pdTRUE;
    SOUNDSINTH_OS_YIELD_FROM_ISR(higher_priority_task_woken);
    return ok;
}

bool os_queue_send_from_isr(Queue* q, void* item) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    const bool ok = xQueueSendFromISR(handle(q), &item, &higher_priority_task_woken) == pdTRUE;
    SOUNDSINTH_OS_YIELD_FROM_ISR(higher_priority_task_woken);
    return ok;
}

uint32_t os_queue_count_from_isr(Queue* q) {
    return static_cast<uint32_t>(uxQueueMessagesWaitingFromISR(handle(q)));
}

} // namespace platform
