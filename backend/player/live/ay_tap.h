#pragma once

// Отвод записей в порты AY для живого MIDI (порт A, бит 2 - линия MIDI OUT
// Spectrum 128). Обработчик записи шины (Core1) кладёт в кольцо, разбирает
// потребитель на другом ядре (core/live_midi/ay_midi.h).
//
// Кольцо без блокировок, один писатель и один читатель - как заказы PCM
// живого потока рядом. Кладут из прерывания шины, поэтому push держится в
// быстрой памяти.

#include <cstdint>

namespace player::live {

// Ёмкость кольца. MIDI - до ~3100 байт/с, по 10 записей на байт.
inline constexpr uint32_t kAyTapRing = 1024; // степень двойки
inline constexpr uint32_t kAyTapBytesPerSec = 3100;
inline constexpr uint32_t kAyTapWritesPerByte = 10;

// Сколько миллисекунд плотного потока кольцо переживает без читателя. По
// этому числу выбирается, как часто его разбирать: реже - и первые записи
// потока теряются. Читатель сверяется с ним static_assert'ом.
inline constexpr uint32_t kAyTapSpanMs = kAyTapRing * 1000u / (kAyTapBytesPerSec * kAyTapWritesPerByte);

// Запись в порт AY: #FFFD - выбор регистра (kAyWriteSelect), #BFFD - данные.
// Из обработчика записи; кольцо полно - запись теряется и считается.
void ay_tap_push(uint16_t ay_write);

// Следующая запись. false - кольцо пусто.
bool ay_tap_pop(uint16_t& ay_write);

// Потеряно на полном кольце с загрузки.
uint32_t ay_tap_lost();

// Пока синтеза нет: разобрать кольцо и напечатать события (не больше
// нескольких строк за проход), и строка счётчиков, если что-то пришло.
// Звать из задачи логгера: первое - каждый проход, второе - по расписанию.
void ay_tap_drain_and_log();
void ay_tap_log_stats();

} // namespace player::live
