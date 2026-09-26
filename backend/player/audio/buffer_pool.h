#pragma once

#include <cstdint>

#include "platform/os.h"
#include "player/config.h"

namespace player::audio {

// Буферы стерео int16 (чередование) и две очереди указателей: свободные и
// готовые. Один производитель, один потребитель. Потребитель - из
// прерывания (DMA I2S на плате, задача в роли прерывания на ПК), не блокируется: нет
// готового буфера - nullptr, что играть вместо, решает он.
//
// Буферы - член класса: на плате объект статический, 4 КБ
class BufferPool {
public:
    static constexpr uint32_t kBufferCount = SOUNDSINTH_AUDIO_BUFFER_COUNT;
    static constexpr uint32_t kFramesPerBuffer = SOUNDSINTH_AUDIO_BUFFER_FRAMES;

    BufferPool();
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    uint32_t frames_per_buffer() const { return kFramesPerBuffer; }

    // --- Производитель: задача рендера ---
    int16_t* begin_write();           // ждёт свободный буфер
    void     end_write(int16_t* buf); // ждёт места в очереди готовых

    // --- Потребитель: прерывание ---
    const int16_t* try_begin_read_from_isr();
    void            end_read_from_isr(const int16_t* buf);
    // Сколько готовых буферов ждёт в очереди, 0..SOUNDSINTH_RENDERED_QUEUE_DEPTH.
    uint32_t        rendered_count_from_isr() const;

    // Вернуть буфер в свободные из задачи - при остановке потребителя.
    void end_read(const int16_t* buf);

    // Из задачи, когда производитель остановлен: готовые, но не сыгранные
    // буферы - в свободные. Иначе новый трек начнётся с конца старого.
    void drain_rendered();

private:
    // alignas(4): стерео-кадр можно писать одним словом.
    alignas(4) int16_t storage_[kBufferCount * kFramesPerBuffer * 2];
    platform::Queue* free_queue_;
    platform::Queue* rendered_queue_;
};

} // namespace player::audio
