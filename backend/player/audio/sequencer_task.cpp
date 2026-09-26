#include "player/audio/sequencer_task.h"

#include "player/config.h"

namespace player::audio {

SequencerTask::SequencerTask(soundsinth::engine::TrackerEngine& engine) : engine_(engine) {
    go_ = platform::os_sem_create(0, 1);
    done_ = platform::os_sem_create(0, 1);
    stop_requested_ = platform::os_sem_create(0, 1);
    stopped_sem_ = platform::os_sem_create(0, 1);
    platform::os_task_create(&SequencerTask::task_fn, this, "sequencer_task", SOUNDSINTH_SEQUENCER_TASK_STACK_WORDS,
                             SOUNDSINTH_RENDER_TASK_PRIORITY);
}

SequencerTask::~SequencerTask() {
    if (!stopped_) {
        stop();
    }
    platform::os_sem_destroy(go_);
    platform::os_sem_destroy(done_);
    platform::os_sem_destroy(stop_requested_);
    platform::os_sem_destroy(stopped_sem_);
}

void SequencerTask::run_tick(void* self_untyped) {
    auto* self = static_cast<SequencerTask*>(self_untyped);
    platform::os_sem_post(self->go_);
    platform::os_sem_wait(self->done_);
}

void SequencerTask::stop(void (*on_slow)(uint32_t waited_ms)) {
    platform::os_sem_post(stop_requested_);
    platform::os_sem_post(go_); // разбудить, если ждёт тика
    uint32_t waited = 0;
    while (!platform::os_sem_wait_ms(stopped_sem_, kStopWaitPortionMs)) {
        waited += kStopWaitPortionMs;
        if (on_slow) on_slow(waited);
    }
    stopped_ = true;
}

void SequencerTask::task_fn(void* self_untyped) {
    static_cast<SequencerTask*>(self_untyped)->run();
}

void SequencerTask::run() {
    while (true) {
        platform::os_sem_wait(go_);
        // Остановка проверяется после пробуждения: stop() будит этим же
        // семафором, и тик делать уже не нужно.
        if (platform::os_sem_try_wait(stop_requested_)) break;
        engine_.run_tick();
        ++ticks_;
        platform::os_sem_post(done_);
    }
    stack_unused_bytes_ = platform::os_task_stack_unused_bytes();
    platform::os_sem_post(stopped_sem_);
    platform::os_task_delete_self();
}

} // namespace player::audio
