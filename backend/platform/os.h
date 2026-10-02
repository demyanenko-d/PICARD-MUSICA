// SPDX-License-Identifier: MIT
#pragma once

// Синхронизация и задачи FreeRTOS под своими именами.
//
// Задел под другую ОС это не даёт и не обещает: реализация одна,
// os_freertos.cpp, и её собирают оба порта - плата и ПК (там симулятор
// Windows). Различие между ними ровно одно, макрос
// SOUNDSINTH_OS_YIELD_FROM_ISR, и живёт оно в FreeRTOSConfig.h каждого
// порта, а не здесь.
//
// Заголовок существует ради другого: Semaphore и Queue тут непрозрачные, и
// потому FreeRTOS.h не попадает в общие заголовки. На ПК порт FreeRTOS
// тянет windows.h - в player/audio/buffer_pool.h ему делать нечего.
//
// Отсюда и правило: звать это должен тот, кто FreeRTOS не видит, то есть
// player. Порт включает FreeRTOS.h сам и обращается к ядру напрямую.

#include <cstdint>

namespace platform {

struct Semaphore;

Semaphore* os_sem_create(uint32_t initial_count, uint32_t max_count);
void os_sem_destroy(Semaphore* sem);
void os_sem_wait(Semaphore* sem);
// Ждать не дольше ms; false - срок вышел.
bool os_sem_wait_ms(Semaphore* sem, uint32_t ms);
void os_sem_post(Semaphore* sem);

// Неблокирующая проверка: true и захват, если счётчик > 0; иначе false без
// ожидания. Для опроса "не пора ли остановиться" из фоновой задачи.
bool os_sem_try_wait(Semaphore* sem);

using TaskFn = void (*)(void* arg);

// priority - относительно уровня IDLE (0 == IDLE); чем больше, тем выше
// приоритет. Абсолютная шкала FreeRTOS скрыта за этим сдвигом.
//
// Куча: стек и служебная запись задачи берутся из кучи ОС и возвращаются
// только после самоудаления. Для задач, живущих до перезагрузки, есть
// os_task_create_static - на плате кучи нет вовсе.
void os_task_create(TaskFn fn, void* arg, const char* name, uint32_t stack_words, uint32_t priority);

// Постоянные задачи: место под стек и служебную запись - статическое, по
// роли. Роли перечислены здесь, а не выводятся из имени, потому что размер
// стека надо знать линкеру; потолок каждой - kOsTaskStackWords рядом.
//
// Заведённая так задача снимается только перезагрузкой: повторный запуск на
// то же место порвал бы списки планировщика. Между работой её парковать -
// на семафоре, а не удалять.
// Роли только те, что заводит общий код: задачи порта порт создаёт сам,
// напрямую ядром, вместе со своим местом.
enum class TaskRole : uint8_t { Render = 0, Sequencer = 1, Count = 2 };

// Потолок стека по ролям, в словах. Просьба сверх потолка - остановка на
// configASSERT при заведении задачи, а не порча соседней памяти.
// Значения по замеру платы (строка "стек свободно"), сверяются со своими
// постоянными у вызывающих.
inline constexpr uint32_t kOsTaskStackWords[static_cast<uint32_t>(TaskRole::Count)] = {1024, 768};

void os_task_create_static(TaskRole role, TaskFn fn, void* arg, const char* name, uint32_t stack_words, uint32_t priority);

// Сколько байт стека постоянной задачи ни разу не тронуто. Ноль - задача
// ещё не заведена. Меряется со стороны, а не изнутри задачи: смотрит на неё
// логгер. Сканирует стек - не для горячего пути.
uint32_t os_task_stack_unused_bytes(TaskRole role);
void os_task_delay_ms(uint32_t ms);
// Вызывается изнутри TaskFn для самоудаления; управление в fn после этого
// вызова больше не возвращается (задача снята с диспетчеризации).
void os_task_delete_self();
// Сколько байт стека вызывающей задачи ни разу не тронуто с её старта.
// Сканирует стек от дна - не для горячего пути.
uint32_t os_task_stack_unused_bytes();

// Очередь указателей фиксированной ёмкости.
struct Queue;

Queue* os_queue_create(uint32_t capacity);
void os_queue_destroy(Queue* q);

// Контекст задачи: блокируют вызывающего, если очередь полна или пуста.
void os_queue_send(Queue* q, void* item);
void* os_queue_receive(Queue* q);
// Контекст задачи без ожидания: false - очередь пуста.
bool os_queue_try_receive(Queue* q, void** out_item);

// Контекст прерывания: на MCU настоящий ISR (DMA I2S опустошил буфер), на PC
// задача FreeRTOS в роли прерывания, внутри критической секции. Поток Windows
// вне FreeRTOS их звать не может. Не блокируют, false - если операция не
// удалась сразу.
bool os_queue_try_receive_from_isr(Queue* q, void** out_item);
bool os_queue_send_from_isr(Queue* q, void* item);
// Сколько элементов в очереди.
uint32_t os_queue_count_from_isr(Queue* q);

} // namespace platform
