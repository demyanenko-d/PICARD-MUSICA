// SPDX-License-Identifier: MIT
#include "player/audio/render_task.h"

#include "platform/clock.h"
#include "player/config.h"

namespace player::audio {

namespace {

// Задача рендера заводится один раз и живёт до перезагрузки: между треками
// она стоит на s_start. Пересоздавать её на каждый трек нельзя - stop()
// возвращается, пока задача ещё доживает свой последний виток, и второй
// запуск на то же место порвал бы списки планировщика.
//
// Семафоры тоже одни на все треки: их владелец - задача, а не трек.
platform::Semaphore* s_start          = nullptr; // трек -> задача: работать
platform::Semaphore* s_finished       = nullptr; // задача -> трек: виток кончился
platform::Semaphore* s_stop_requested = nullptr; // трек -> задача: кончай
RenderTask* s_current                 = nullptr;

} // namespace

RenderTask::RenderTask(soundsinth::mixbus::MixBus& bus, BufferPool& pool, std::atomic<uint32_t>* busy_us_counter)
    : bus_(bus)
    , pool_(pool)
    , busy_us_counter_(busy_us_counter) {
    if (s_start == nullptr) {
        s_start          = platform::os_sem_create(0, 1);
        s_finished       = platform::os_sem_create(0, 1);
        s_stop_requested = platform::os_sem_create(0, 1);
        platform::os_task_create_static(platform::TaskRole::Render, &RenderTask::task_fn, nullptr, "render_task", SOUNDSINTH_RENDER_TASK_STACK_WORDS,
                                        SOUNDSINTH_RENDER_TASK_PRIORITY);
    }
    // Счётчик с прошлого трека не переносится: остаток обернулся бы
    // мгновенной остановкой нового. Сбрасывается здесь, а не в задаче:
    // задача сейчас заведомо стоит на s_start и помешать не может.
    while (platform::os_sem_try_wait(s_stop_requested)) {
    }
    // Себя видимым до пробуждения: задача берёт указатель сразу после s_start.
    s_current = this;
    platform::os_sem_post(s_start);
}

RenderTask::~RenderTask() {
    if (!stopped_) {
        stop();
    }
}

void RenderTask::stop(void (*on_slow)(uint32_t waited_ms)) {
    platform::os_sem_post(s_stop_requested);
    uint32_t waited = 0;
    while (!platform::os_sem_wait_ms(s_finished, kStopWaitPortionMs)) {
        waited += kStopWaitPortionMs;
        if (on_slow) on_slow(waited);
    }
    stopped_  = true;
    s_current = nullptr;
}

void RenderTask::task_fn(void*) {
    for (;;) {
        platform::os_sem_wait(s_start);
        RenderTask* const self = s_current;
        if (self != nullptr) self->run();
        platform::os_sem_post(s_finished);
    }
}

void RenderTask::run() {
    uint32_t prev_done_us = 0;
    // Ожидание места в очереди готовых у прошлого буфера: оно попадает в
    // нынешний промежуток, а замеряется в конце прошлого оборота.
    uint32_t prev_push_us = 0;
    bool have_prev        = false;
    while (!platform::os_sem_try_wait(s_stop_requested)) {
        const uint32_t wait0   = platform::time_us();
        int16_t* w             = pool_.begin_write(); // ожидание, это не занятость ядра
        const uint32_t t0      = platform::time_us();
        const uint32_t wait_us = t0 - wait0;
        bus_.render(w, pool_.frames_per_buffer());
        const uint32_t done_us   = platform::time_us();
        const uint32_t render_us = done_us - t0;
        // Писатель один - load и store, без RMW (читает другое ядро).
        if (busy_us_counter_ != nullptr) {
            const uint32_t prev = busy_us_counter_->load(std::memory_order_relaxed);
            busy_us_counter_->store(prev + render_us, std::memory_order_relaxed);
        }
        if (render_us > worst_render_us_.load(std::memory_order_relaxed)) {
            worst_render_us_.store(render_us, std::memory_order_relaxed);
        }
        // Промежуток между готовыми буферами: в него входит и ожидание
        // свободного буфера в begin_write, но пока синк успевает, ждать там
        // нечего - буфер освобождается раньше.
        if (have_prev) {
            const uint32_t gap = done_us - prev_done_us;
            if (gap > worst_gap_us_.load(std::memory_order_relaxed)) {
                worst_gap_us_.store(gap, std::memory_order_relaxed);
                worst_gap_render_us_.store(render_us, std::memory_order_relaxed);
                worst_gap_wait_us_.store(wait_us, std::memory_order_relaxed);
                worst_gap_push_us_.store(prev_push_us, std::memory_order_relaxed);
            }
        }
        prev_done_us = done_us;
        have_prev    = true;
        pool_.end_write(w);
        prev_push_us = platform::time_us() - done_us;
    }
    stack_unused_bytes_ = platform::os_task_stack_unused_bytes();
}

} // namespace player::audio
