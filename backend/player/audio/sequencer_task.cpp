// SPDX-License-Identifier: MIT
#include "player/audio/sequencer_task.h"

#include "player/config.h"

namespace player::audio {

// Память стека задаёт потолок роли в платформе, ключ только передаётся в
// заведение задачи: разойдутся - ужатие ключом не освободит ни байта.
static_assert(SOUNDSINTH_SEQUENCER_TASK_STACK_WORDS == platform::kOsTaskStackWords[static_cast<uint32_t>(platform::TaskRole::Sequencer)],
              "stack size key and the platform stack ceiling disagree");

namespace {

// Задача и её семафоры - одни на всю работу прошивки.
ParkedTask s_task;
platform::Semaphore* s_go   = nullptr; // рендер -> секвенсор: пора делать тик
platform::Semaphore* s_done = nullptr; // секвенсор -> рендер: тик сделан

} // namespace

SequencerTask::SequencerTask(soundsinth::engine::TrackerEngine& engine) : engine_(engine) {
    if (!s_task.created()) {
        s_go   = platform::os_sem_create(0, 1);
        s_done = platform::os_sem_create(0, 1);
        s_task.create<SequencerTask>(platform::TaskRole::Sequencer, "sequencer_task", SOUNDSINTH_SEQUENCER_TASK_STACK_WORDS, SOUNDSINTH_RENDER_TASK_PRIORITY);
    }
    // Тик с прошлого трека не переносится: остаток обернулся бы пропущенным
    // тиком. Сбрасывается здесь, а не в задаче: задача сейчас заведомо стоит
    // на старте, а рендер нового трека ещё не заведён и тика не просил.
    while (platform::os_sem_try_wait(s_go)) {
    }
    while (platform::os_sem_try_wait(s_done)) {
    }
    s_task.launch(this);
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
    s_task.request_stop();
    platform::os_sem_post(s_go); // разбудить, если ждёт тика
    s_task.wait_stopped(on_slow);
    stopped_ = true;
}

void SequencerTask::run() {
    while (true) {
        platform::os_sem_wait(s_go);
        // Остановка проверяется после пробуждения: stop() будит этим же
        // семафором, и тик делать уже не нужно.
        if (s_task.stop_requested()) break;
        engine_.run_tick();
        ++ticks_;
        platform::os_sem_post(s_done);
    }
    stack_unused_bytes_ = platform::os_task_stack_unused_bytes();
}

} // namespace player::audio
