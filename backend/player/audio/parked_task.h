// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "platform/os.h"

namespace player::audio {

// Постоянная задача звука. Заводится один раз на всю работу прошивки и
// между треками стоит на семафоре start_: объект трека живёт трек, задача -
// дольше. Пересоздавать её на каждый трек нельзя - остановка возвращается,
// пока задача ещё доживает свой последний виток, и второй запуск на то же
// место порвал бы списки планировщика.
//
// Семафоры тоже одни на все треки: их владелец - задача, а не трек.
//
// T - объект трека с методом run(): тело одного трека, выход из него - по
// stop_requested().
class ParkedTask {
public:
    // Порция ожидания остановки: on_slow зовётся раз в столько миллисекунд.
    static constexpr uint32_t kStopWaitPortionMs = 500;

    bool created() const { return start_ != nullptr; }

    // Завести семафоры и задачу. Один раз, из первого конструктора трека.
    template <class T>
    void create(platform::TaskRole role, const char* name, uint32_t stack_words, uint32_t priority) {
        start_          = platform::os_sem_create(0, 1);
        finished_       = platform::os_sem_create(0, 1);
        stop_requested_ = platform::os_sem_create(0, 1);
        platform::os_task_create_static(role, &task_fn<T>, this, name, stack_words, priority);
    }

    // Пустить задачу по объекту трека. Просьба остановки с прошлого трека не
    // переносится: остаток обернулся бы мгновенной остановкой нового.
    // Гасится здесь, а не в задаче: задача сейчас заведомо стоит на start_.
    void launch(void* self) {
        while (platform::os_sem_try_wait(stop_requested_)) {
        }
        // Себя видимым до пробуждения: задача берёт указатель сразу после start_.
        current_ = self;
        platform::os_sem_post(start_);
    }

    void request_stop() { platform::os_sem_post(stop_requested_); }

    // Опрос из run(): true - пора выходить, просьба снята.
    bool stop_requested() { return platform::os_sem_try_wait(stop_requested_); }

    // Ждать конца витка задачи. Возвращается только по остановке: вернуть
    // раньше - значит разрушить объект трека под живой задачей. on_slow
    // (может быть nullptr) зовётся раз в kStopWaitPortionMs с прошедшим
    // временем.
    void wait_stopped(void (*on_slow)(uint32_t waited_ms)) {
        uint32_t waited = 0;
        while (!platform::os_sem_wait_ms(finished_, kStopWaitPortionMs)) {
            waited += kStopWaitPortionMs;
            if (on_slow) on_slow(waited);
        }
        current_ = nullptr;
    }

private:
    template <class T>
    static void task_fn(void* arg) {
        ParkedTask& t = *static_cast<ParkedTask*>(arg);
        for (;;) {
            platform::os_sem_wait(t.start_);
            T* const self = static_cast<T*>(t.current_);
            if (self != nullptr) self->run();
            platform::os_sem_post(t.finished_);
        }
    }

    platform::Semaphore* start_          = nullptr; // трек -> задача: работать
    platform::Semaphore* finished_       = nullptr; // задача -> трек: виток кончился
    platform::Semaphore* stop_requested_ = nullptr; // трек -> задача: кончай
    void* current_                       = nullptr;
};

} // namespace player::audio
