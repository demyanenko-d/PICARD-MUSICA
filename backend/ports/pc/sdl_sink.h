#pragma once

#include "player/audio/buffer_pool.h"

namespace platform_pc {

// Вывод на звуковую карту через SDL2: открывает устройство на 44100 Гц /
// 16 бит / стерео; pool вычерпывает задача FreeRTOS, колбэк SDL играет её
// копии. false - устройство не открылось.
bool sdl_sink_start(player::audio::BufferPool& pool);

// Останавливает задачу вывода и закрывает устройство. Звать после остановки
// производителя: иначе он повиснет в begin_write().
void sdl_sink_stop();

} // namespace platform_pc
