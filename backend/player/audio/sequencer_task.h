// SPDX-License-Identifier: MIT
#pragma once

// Тик движка своей задачей. Рендер, дойдя до границы тика, будит её и ждёт
// здесь же: очередь строгая, забега вперёд нет. Звук от этого не меняется:
// порядок работы тот же, что при тике внутри рендера.
//
// Приоритет равен приоритету рендера: обе задачи звуковые, и отдавать одной
// предпочтение не за что, пока они ходят по очереди.
//
// Задача ОС заводится один раз на всю работу прошивки и между треками стоит
// на семафоре: объект живёт трек, задача - дольше.

#include <cstdint>

#include "platform/os.h"
#include "core/engine/tracker_engine.h"

namespace player::audio {

class SequencerTask {
public:
    explicit SequencerTask(soundsinth::engine::TrackerEngine& engine);
    ~SequencerTask();

    SequencerTask(const SequencerTask&)            = delete;
    SequencerTask& operator=(const SequencerTask&) = delete;

    // Ставится движку как TickRunner: будит задачу и ждёт её. Аргумент не
    // нужен - семафоры общие у задачи, а задача одна.
    static void run_tick(void* unused);

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
    static void task_fn(void* unused);
    void run();

    soundsinth::engine::TrackerEngine& engine_;
    uint32_t ticks_              = 0;
    uint32_t stack_unused_bytes_ = 0;
    bool stopped_                = false;
};

} // namespace player::audio
