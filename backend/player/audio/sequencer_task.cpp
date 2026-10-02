// SPDX-License-Identifier: MIT
#include "player/audio/sequencer_task.h"

#include "player/config.h"

namespace player::audio {

namespace {

// Задача и её семафоры - одни на всю работу прошивки, как у рендера:
// между треками задача стоит на go_ и ждёт следующего трека на s_start.
platform::Semaphore* s_start          = nullptr; // трек -> задача: работать
platform::Semaphore* s_go             = nullptr; // рендер -> секвенсор: пора делать тик
platform::Semaphore* s_done           = nullptr; // секвенсор -> рендер: тик сделан
platform::Semaphore* s_stop_requested = nullptr; // трек -> задача: кончай
platform::Semaphore* s_stopped        = nullptr; // задача -> трек: виток кончился
SequencerTask* s_current              = nullptr;

} // namespace

SequencerTask::SequencerTask(soundsinth::engine::TrackerEngine& engine) : engine_(engine) {
    if (s_start == nullptr) {
        s_start          = platform::os_sem_create(0, 1);
        s_go             = platform::os_sem_create(0, 1);
        s_done           = platform::os_sem_create(0, 1);
        s_stop_requested = platform::os_sem_create(0, 1);
        s_stopped        = platform::os_sem_create(0, 1);
        platform::os_task_create_static(platform::TaskRole::Sequencer, &SequencerTask::task_fn, nullptr, "sequencer_task",
                                        SOUNDSINTH_SEQUENCER_TASK_STACK_WORDS, SOUNDSINTH_RENDER_TASK_PRIORITY);
    }
    // Счётчики с прошлого трека не переносятся: остаток обернулся бы
    // пропущенным тиком или мгновенной остановкой нового. Сбрасываются
    // здесь, а не в задаче: задача сейчас заведомо стоит на s_start, а
    // рендер нового трека ещё не заведён и тика не просил.
    while (platform::os_sem_try_wait(s_go)) {
    }
    while (platform::os_sem_try_wait(s_done)) {
    }
    while (platform::os_sem_try_wait(s_stop_requested)) {
    }
    s_current = this;
    platform::os_sem_post(s_start);
}

SequencerTask::~SequencerTask() {
    if (!stopped_) {
        stop();
    }
}

void SequencerTask::run_tick(void*) {
    platform::os_sem_post(s_go);
    platform::os_sem_wait(s_done);
}

void SequencerTask::stop(void (*on_slow)(uint32_t waited_ms)) {
    platform::os_sem_post(s_stop_requested);
    platform::os_sem_post(s_go); // разбудить, если ждёт тика
    uint32_t waited = 0;
    while (!platform::os_sem_wait_ms(s_stopped, kStopWaitPortionMs)) {
        waited += kStopWaitPortionMs;
        if (on_slow) on_slow(waited);
    }
    stopped_  = true;
    s_current = nullptr;
}

void SequencerTask::task_fn(void*) {
    for (;;) {
        platform::os_sem_wait(s_start);
        SequencerTask* const self = s_current;
        if (self != nullptr) self->run();
        platform::os_sem_post(s_stopped);
    }
}

void SequencerTask::run() {
    while (true) {
        platform::os_sem_wait(s_go);
        // Остановка проверяется после пробуждения: stop() будит этим же
        // семафором, и тик делать уже не нужно.
        if (platform::os_sem_try_wait(s_stop_requested)) break;
        engine_.run_tick();
        ++ticks_;
        platform::os_sem_post(s_done);
    }
    stack_unused_bytes_ = platform::os_task_stack_unused_bytes();
}

} // namespace player::audio
