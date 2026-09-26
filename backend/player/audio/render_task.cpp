#include "player/audio/render_task.h"

#include "platform/clock.h"
#include "player/config.h"

namespace player::audio {

RenderTask::RenderTask(soundsinth::mixbus::MixBus& bus, BufferPool& pool, std::atomic<uint32_t>* busy_us_counter)
    : bus_(bus), pool_(pool), busy_us_counter_(busy_us_counter) {
    stop_requested_ = platform::os_sem_create(0, 1);
    done_ = platform::os_sem_create(0, 1);
    platform::os_task_create(&RenderTask::task_fn, this, "render_task", SOUNDSINTH_RENDER_TASK_STACK_WORDS,
                              SOUNDSINTH_RENDER_TASK_PRIORITY);
}

RenderTask::~RenderTask() {
    if (!stopped_) {
        stop();
    }
    platform::os_sem_destroy(stop_requested_);
    platform::os_sem_destroy(done_);
}

void RenderTask::stop(void (*on_slow)(uint32_t waited_ms)) {
    platform::os_sem_post(stop_requested_);
    uint32_t waited = 0;
    while (!platform::os_sem_wait_ms(done_, kStopWaitPortionMs)) {
        waited += kStopWaitPortionMs;
        if (on_slow) on_slow(waited);
    }
    stopped_ = true;
}

void RenderTask::task_fn(void* self_untyped) {
    static_cast<RenderTask*>(self_untyped)->run();
}

void RenderTask::run() {
    uint32_t prev_done_us = 0;
    // Ожидание места в очереди готовых у прошлого буфера: оно попадает в
    // нынешний промежуток, а замеряется в конце прошлого оборота.
    uint32_t prev_push_us = 0;
    bool have_prev = false;
    while (!platform::os_sem_try_wait(stop_requested_)) {
        const uint32_t wait0 = platform::time_us();
        int16_t* w = pool_.begin_write(); // ожидание, это не занятость ядра
        const uint32_t t0 = platform::time_us();
        const uint32_t wait_us = t0 - wait0;
        bus_.render(w, pool_.frames_per_buffer());
        const uint32_t done_us = platform::time_us();
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
        have_prev = true;
        pool_.end_write(w);
        prev_push_us = platform::time_us() - done_us;
    }
    stack_unused_bytes_ = platform::os_task_stack_unused_bytes();
    platform::os_sem_post(done_);
    platform::os_task_delete_self();
}

} // namespace player::audio
