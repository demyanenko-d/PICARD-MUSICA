#pragma once

// Тик движка своей задачей. Рендер, дойдя до границы тика, будит её и ждёт
// здесь же: очередь строгая, забега вперёд нет. Звук от этого не меняется -
// порядок работы тот же, что и при тике внутри рендера, - а разделение
// готовит почву для разноса по ядрам, ради которого и писались кольцо команд
// и тень состояния голосов.
//
// Приоритет равен приоритету рендера: обе задачи звуковые, и отдавать одной
// предпочтение не за что, пока они ходят по очереди.

#include "platform/os.h"
#include "core/engine/tracker_engine.h"

namespace player::audio {

class SequencerTask {
public:
    explicit SequencerTask(soundsinth::engine::TrackerEngine& engine);
    ~SequencerTask();

    SequencerTask(const SequencerTask&) = delete;
    SequencerTask& operator=(const SequencerTask&) = delete;

    // Ставится движку как TickRunner: будит задачу и ждёт её.
    static void run_tick(void* self_untyped);

    // Блокирует вызывающего до полной остановки задачи. Звать после остановки
    // рендера: иначе он повиснет в ожидании тика, которого уже никто не
    // сделает.
    static constexpr uint32_t kStopWaitPortionMs = 500;
    void stop(void (*on_slow)(uint32_t waited_ms) = nullptr);

    // Сколько байт стека задачи не тронуто за её жизнь; верно после stop().
    uint32_t stack_unused_bytes() const { return stack_unused_bytes_; }
    // Тиков выполнено задачей - сверять с числом тиков трека.
    uint32_t ticks() const { return ticks_; }

private:
    static void task_fn(void* self_untyped);
    void run();

    soundsinth::engine::TrackerEngine& engine_;
    platform::Semaphore* go_;   // рендер -> секвенсор: пора делать тик
    platform::Semaphore* done_; // секвенсор -> рендер: тик сделан
    platform::Semaphore* stop_requested_;
    platform::Semaphore* stopped_sem_;
    uint32_t ticks_ = 0;
    uint32_t stack_unused_bytes_ = 0;
    bool stopped_ = false;
};

} // namespace player::audio
