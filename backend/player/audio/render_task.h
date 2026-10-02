// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>

#include <cstdint>

#include "platform/os.h"
#include "player/audio/buffer_pool.h"
#include "core/audio/mixbus.h"

namespace player::audio {

// Задача, которая рендерит bus в pool до stop().
//
// Сама задача ОС заводится один раз на всю работу прошивки и между треками
// стоит на семафоре: объект живёт трек, задача - дольше. Иначе повторный
// запуск приходился бы на ещё не снятую задачу.
class RenderTask {
public:
    // busy_us_counter - необязательный счётчик микросекунд, когда ядро считало
    // звук. Копится только время render(), без ожидания свободного буфера:
    // делённое на прошедшее время - доля ядра на аудио.
    //
    // Указателем, а не полем: RenderTask пересоздаётся на каждый трек, а
    // счётчик должен пережить смену и быть виден другому ядру.
    RenderTask(soundsinth::mixbus::MixBus& bus, BufferPool& pool, std::atomic<uint32_t>* busy_us_counter = nullptr);
    ~RenderTask();

    RenderTask(const RenderTask&)            = delete;
    RenderTask& operator=(const RenderTask&) = delete;

    // Блокирует вызывающего до полной остановки задачи. Потребитель
    // (sdl_sink, I2S DMA) должен забирать буферы с конструктора и до
    // возврата stop(): иначе задача зависнет в end_write() в ожидании места в
    // очереди rendered. Выбрасывать их вместо игры - терять начало трека.
    //
    // Возвращается только по остановке задачи: вернуть раньше - значит
    // разрушить движок под живой задачей. on_slow (может быть nullptr)
    // зовётся раз в kStopWaitPortionMs, пока задача не остановилась, с
    // прошедшим временем - признак жизни вместо молчаливого зависания.
    static constexpr uint32_t kStopWaitPortionMs = 500;
    void stop(void (*on_slow)(uint32_t waited_ms) = nullptr);

    // Сколько байт стека задачи не тронуто за её жизнь; верно после stop().
    uint32_t stack_unused_bytes() const { return stack_unused_bytes_; }

    // Заминка вывода видна только как "рендер не успел". Разделяют причины:
    // самая долгая отрисовка буфера и самый большой промежуток между
    // готовыми буферами.
    uint32_t worst_render_us() const { return worst_render_us_.load(std::memory_order_relaxed); }
    uint32_t worst_gap_us() const { return worst_gap_us_.load(std::memory_order_relaxed); }
    // Отрисовка, что шла в самом большом промежутке: по ней видно, сколько из
    // него ядро считало, а сколько задача ждала.
    uint32_t worst_gap_render_us() const { return worst_gap_render_us_.load(std::memory_order_relaxed); }
    // Сколько из промежутка ушло на ожидание свободного буфера и сколько - на
    // ожидание места в очереди готовых. Второе значит, что рендер убежал
    // вперёд и ждёт вывод: передышка, а не заминка. Что осталось сверх них и
    // отрисовки - время, когда задачу не пускали на ядро.
    uint32_t worst_gap_wait_us() const { return worst_gap_wait_us_.load(std::memory_order_relaxed); }
    uint32_t worst_gap_push_us() const { return worst_gap_push_us_.load(std::memory_order_relaxed); }

private:
    static void task_fn(void* unused);
    void run();

    soundsinth::mixbus::MixBus& bus_;
    BufferPool& pool_;
    std::atomic<uint32_t>* busy_us_counter_ = nullptr;
    // Пишет задача рендера, читает задача лога - отсюда atomic.
    std::atomic<uint32_t> worst_render_us_{0};
    std::atomic<uint32_t> worst_gap_us_{0};
    std::atomic<uint32_t> worst_gap_render_us_{0};
    std::atomic<uint32_t> worst_gap_wait_us_{0};
    std::atomic<uint32_t> worst_gap_push_us_{0};
    // Пишет задача перед сигналом done_, читает остановивший после stop().
    uint32_t stack_unused_bytes_ = 0;
    bool stopped_                = false;
};

} // namespace player::audio
