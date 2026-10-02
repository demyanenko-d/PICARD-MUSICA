// SPDX-License-Identifier: MIT
#include "pc/sdl_sink.h"

#include <SDL.h>

#include <atomic>
#include <cstring>

#include "FreeRTOS.h"
#include "task.h"

#include "platform/os.h"
#include "player/config.h"

namespace platform_pc {
namespace {

using player::audio::BufferPool;

// Колбэк SDL2 живёт в своём потоке Windows, вне FreeRTOS, и очередей пула не
// трогает: порт FreeRTOS для Windows не даёт такому потоку ни маски
// прерываний, ни критической секции, и одновременная операция задачи над
// той же очередью теряет или удваивает буфер. Пул вычерпывает задача
// FreeRTOS в роли прерывания I2S и копирует буферы в кольцо на атомиках;
// колбэк читает кольцо.
constexpr uint32_t kRingSlots      = 4;
constexpr uint32_t kFrames         = BufferPool::kFramesPerBuffer;
constexpr uint32_t kFeedStackWords = 256;

struct SdlSinkState {
    BufferPool* pool         = nullptr;
    SDL_AudioDeviceID device = 0;
    int16_t ring[kRingSlots][kFrames * 2];
    std::atomic<uint32_t> head{0}; // пишет задача
    std::atomic<uint32_t> tail{0}; // пишет колбэк
    // Последний сыгранный буфер. При голодании колбэк повторяет его вместо
    // тишины, как DMA-ISR на плате.
    int16_t last[kFrames * 2];
    bool have_last          = false;
    uint32_t underrun_count = 0; // диагностика
    std::atomic<bool> quit{false};
    platform::Semaphore* exited = nullptr;
};

SdlSinkState s_state;

// Приоритет выше задачи рендера, как у прерывания. *_from_isr - в
// критической секции: порт для Windows в них прерывания не маскирует.
void feed_task(void* arg) {
    auto* state = static_cast<SdlSinkState*>(arg);
    while (!state->quit.load()) {
        const uint32_t head = state->head.load(std::memory_order_relaxed);
        if (head - state->tail.load(std::memory_order_acquire) == kRingSlots) {
            platform::os_task_delay_ms(1);
            continue;
        }
        taskENTER_CRITICAL();
        const int16_t* buf = state->pool->try_begin_read_from_isr();
        taskEXIT_CRITICAL();
        if (buf == nullptr) {
            platform::os_task_delay_ms(1);
            continue;
        }
        std::memcpy(state->ring[head % kRingSlots], buf, sizeof(state->ring[0]));
        taskENTER_CRITICAL();
        state->pool->end_read_from_isr(buf);
        taskEXIT_CRITICAL();
        state->head.store(head + 1, std::memory_order_release);
    }
    platform::os_sem_post(state->exited);
    platform::os_task_delete_self();
}

void audio_callback(void* userdata, Uint8* stream, int len) {
    auto* state                    = static_cast<SdlSinkState*>(userdata);
    const uint32_t bytes_per_frame = 2 * sizeof(int16_t);
    uint32_t frames_needed         = static_cast<uint32_t>(len) / bytes_per_frame;
    uint8_t* out                   = stream;

    while (frames_needed > 0) {
        const uint32_t tail = state->tail.load(std::memory_order_relaxed);
        if (tail != state->head.load(std::memory_order_acquire)) {
            std::memcpy(state->last, state->ring[tail % kRingSlots], sizeof(state->last));
            state->tail.store(tail + 1, std::memory_order_release);
            state->have_last = true;
        } else if (state->have_last) {
            ++state->underrun_count;
        } else {
            // Ещё ничего не отрендерили - повторять нечего, тишина.
            std::memset(out, 0, frames_needed * bytes_per_frame);
            return;
        }

        // SDL-фрагмент задан равным буферу пула (sdl_sink_start), так что
        // здесь всегда take == frames_needed за один проход.
        const uint32_t take = kFrames < frames_needed ? kFrames : frames_needed;
        std::memcpy(out, state->last, static_cast<size_t>(take) * bytes_per_frame);
        out           += take * bytes_per_frame;
        frames_needed -= take;
    }
}

} // namespace

bool sdl_sink_start(player::audio::BufferPool& pool) {
    SdlSinkState* state = &s_state;
    state->pool         = &pool;
    state->head.store(0);
    state->tail.store(0);
    state->have_last = false;
    state->quit.store(false);
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        return false;
    }

    SDL_AudioSpec want{};
    want.freq     = 44100;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = static_cast<Uint16>(kFrames);
    want.callback = audio_callback;
    want.userdata = state;

    SDL_AudioSpec have{};
    state->device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (state->device == 0) {
        return false;
    }
    state->exited = platform::os_sem_create(0, 1);
    platform::os_task_create(&feed_task, state, "sdl_feed", kFeedStackWords, SOUNDSINTH_RENDER_TASK_PRIORITY + 1);
    SDL_PauseAudioDevice(state->device, 0);
    return true;
}

void sdl_sink_stop() {
    SdlSinkState* state = &s_state;
    if (state->device == 0) return;
    state->quit.store(true);
    platform::os_sem_wait(state->exited);
    platform::os_sem_destroy(state->exited);
    state->exited = nullptr;
    // Блокирует до полной остановки callback-потока SDL.
    SDL_CloseAudioDevice(state->device);
    state->device = 0;
}

} // namespace platform_pc
