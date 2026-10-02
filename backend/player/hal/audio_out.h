// SPDX-License-Identifier: MIT
#pragma once

// Куда звуковая цепочка отдаёт готовые буферы. Реализует порт.
//
// Пул буферов плеер строит сам и отдаёт сюда: вывод забирает из него
// готовые буферы и возвращает свободные. Как именно - I2S с DMA, звуковое
// устройство системы - плееру не видно.
//
// Счётчики нужны диагностике трека: они накопительные с запуска, разности
// считает читатель. Из обработчика прерывания ничего здесь не зовут.

#include <cstdint>

#include "player/audio/buffer_pool.h"
#include "player/config.h"

namespace player::hal {

// Настроить и запустить вывод. Зовётся один раз за работу: смену трека
// закрывает audio_out_set_silent.
void audio_out_start(player::audio::BufferPool& pool, uint32_t sample_rate_hz);

// Тишина вместо зацикленных буферов, когда трек снят. Можно звать до
// audio_out_start.
void audio_out_set_silent(bool silent);

// Сколько раз вывод не нашёл готового буфера и повторил прежний.
uint32_t audio_out_underruns();

// Из них - во время фоновой догрузки сэмпла. Все под догрузкой означает
// конкуренцию за память, а не нехватку времени на отрисовку.
uint32_t audio_out_underruns_bg();

// Сколько раз вывод опоздал больше чем на буфер и сыграл чужую память.
uint32_t audio_out_late();

// Запас очереди готовых: забирая буфер, вывод прибавляет к ready[k], если в
// очереди ждало k готовых. ready[0] - заминка, нижние корзины пусты -
// очередь можно укорачивать.
inline constexpr uint32_t kReadyBins = SOUNDSINTH_RENDERED_QUEUE_DEPTH + 1u;
struct ReadyStats {
    uint32_t ready[kReadyBins];
};
ReadyStats audio_out_ready_stats();

} // namespace player::hal
