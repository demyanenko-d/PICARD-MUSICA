#pragma once

// Вывод I2S (PIO и DMA ping-pong). Буферы из BufferPool забирает DMA ISR
// (try_begin_read_from_isr/end_read_from_isr).

#include <cstdint>

#include "player/hal/audio_out.h"
#include "player/audio/buffer_pool.h"

namespace rp2350::audio {

// База GPIO 16 блоку звука (I2S на GPIO 44-46). Ставится, только пока в
// блоке нет программ, а блок делят звук и шина: звать на старте платы, до
// любых программ в нём.
void i2s_sink_set_block_base();

// Настраивает пины, PIO и DMA и запускает вывод; звать один раз, синк
// живёт до конца работы, смену трека закрывает i2s_sink_set_silent.
void i2s_sink_start(player::audio::BufferPool& pool, uint32_t sample_rate_hz);

// Диагностика: сколько раз DMA IRQ не нашёл готового буфера в pool и
// снова поставил в канал его прежний буфер.
uint32_t i2s_sink_underrun_count();
// Из них - во время фоновой догрузки сэмпла.
uint32_t i2s_sink_underrun_bg_count();
// Сколько раз DMA IRQ опоздал больше чем на буфер: канал успел перезапуститься
// и сыграл память за прошлым буфером.
uint32_t i2s_sink_late_count();

// Запас очереди готовых буферов: когда канал забирает следующий буфер во
// время игры, ready[k] +1, если в очереди ждало k готовых. ready[0] -
// заминка. Накопительно с запуска. Нижние корзины пусты - очередь можно
// укорачивать.
// Тип и число корзин - контракта вывода: переходник отдаёт их как есть.
using player::hal::ReadyStats;
inline constexpr uint32_t kReadyBins = player::hal::kReadyBins;
ReadyStats i2s_sink_ready_stats();

// Тишина вместо зацикленных буферов, когда трек снят. Звать с Core0, можно
// до i2s_sink_start.
void i2s_sink_set_silent(bool silent);

} // namespace rp2350::audio
