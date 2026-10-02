// SPDX-License-Identifier: MIT
#include "player/audio/buffer_pool.h"

#include "platform/hot_path.h"
#include "player/config.h"

namespace player::audio {

BufferPool::BufferPool() : free_queue_(platform::os_queue_create(kBufferCount)), rendered_queue_(platform::os_queue_create(SOUNDSINTH_RENDERED_QUEUE_DEPTH)) {
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        platform::os_queue_send(free_queue_, &storage_[i * kFramesPerBuffer * 2]);
    }
    // Хвост не выдаётся никому и обязан быть тишиной: в него попадает
    // перебег опоздавшего обработчика I2S. Обнуляется здесь, а не полагается
    // на .bss: пул строится размещающим new, и во второй раз там лежал бы
    // прежний звук.
    for (uint32_t i = kBufferCount * kFramesPerBuffer * 2; i < (kBufferCount + 1) * kFramesPerBuffer * 2; ++i) {
        storage_[i] = 0;
    }
}

BufferPool::~BufferPool() {
    platform::os_queue_destroy(free_queue_);
    platform::os_queue_destroy(rendered_queue_);
}

int16_t* BufferPool::begin_write() {
    return static_cast<int16_t*>(platform::os_queue_receive(free_queue_));
}

void BufferPool::end_write(int16_t* buf) {
    platform::os_queue_send(rendered_queue_, buf);
}

void BufferPool::end_read(const int16_t* buf) {
    platform::os_queue_send(free_queue_, const_cast<int16_t*>(buf));
}

void BufferPool::drain_rendered() {
    void* item = nullptr;
    while (platform::os_queue_try_receive(rendered_queue_, &item)) {
        platform::os_queue_send(free_queue_, item);
    }
}

const int16_t* SOUNDSINTH_HOT_PATH(BufferPool::try_begin_read_from_isr)() {
    void* item = nullptr;
    if (!platform::os_queue_try_receive_from_isr(rendered_queue_, &item)) {
        return nullptr;
    }
    return static_cast<const int16_t*>(item);
}

uint32_t SOUNDSINTH_HOT_PATH(BufferPool::rendered_count_from_isr)() const {
    return platform::os_queue_count_from_isr(rendered_queue_);
}

void SOUNDSINTH_HOT_PATH(BufferPool::end_read_from_isr)(const int16_t* buf) {
    platform::os_queue_send_from_isr(free_queue_, const_cast<int16_t*>(buf));
}

} // namespace player::audio
