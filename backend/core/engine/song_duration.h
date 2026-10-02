// SPDX-License-Identifier: MIT
#pragma once

// Длительность трека и позиции префетча по времени, общий код ПК и платы.

#include <cstdint>

#include "core/engine/sequencer.h"
#include "core/model/song.h"
#include "core/memory/psram_store.h"
#include "core/memory/track_memory.h"

namespace soundsinth::engine {

// Длина трека в выходных отсчётах (kSampleRateHz). Трекерные файлы почти
// всегда зациклены через restart_position, и song_ended сам почти не
// наступает. "Весь трек" здесь - один проход, пока пара (order_pos, row)
// не начнёт повторяться; работает и для интро с возвратом внутрь трека.
// Прогоняется только секвенсор (sequencer_init/tick), без голосов и
// эффектов: дёшево, основному TrackerEngine не мешает (тот же Song и
// PSRAM с паттернами, свой PlayState).
//
// Буфер прохода даёт вызывающий: отметки посещённых (order_pos, row),
// 256x256 бит, и PlayState прохода. Ни стек, ни статика - сценарий
// DurationPass: длительность и префетч считаются после разбора, до
// перепаковки сэмплов, когда буфер свободен.
inline constexpr uint32_t kVisitedScratchBytes  = 8192;
inline constexpr uint32_t kDurationScratchBytes = kVisitedScratchBytes + static_cast<uint32_t>(sizeof(PlayState));
static_assert(kDurationScratchBytes <= memory::kDurationPassBytes, "the sequencer pass does not fit its scratch");

// scratch - буфер не меньше kDurationScratchBytes, выровненный под
// PlayState, содержимое затирается. Меньше или nullptr - возвращает 0
// ("посчитать не удалось"): без отметок цикл (order_pos, row) не
// замкнётся, и длительность вышла бы неверной без предупреждения.
//
// stats (может быть nullptr) - как кончился проход: для строки лога,
// по которой видна неверная длительность.
enum class DurationStop : uint8_t { None, Repeat, SongEnd, RowLimit, TickGuard, FrameLimit };
struct DurationStats {
    uint32_t rows           = 0;
    uint32_t ticks          = 0;
    DurationStop stop       = DurationStop::None;
    uint16_t stop_order_pos = 0; // последняя строка прохода (при Repeat - повторённая)
    uint16_t stop_row       = 0;
    // Позиция order уменьшилась посреди прохода (переход Bxx назад на ещё не
    // игранную строку). Вытеснение "последняя позиция сэмпла пройдена" у
    // такого трека неверно: секвенсор вернётся к позиции, которую план
    // считает отыгравшей. Возврат на пройденную строку - конец прохода, не
    // признак.
    bool position_goes_back = false;
};
// Наблюдатель строки: та же строка, что видит секвенсор, с позицией order.
// Нужен плану подкачки - он собирается этим же проходом, а не вторым
// обходом песни: данные те же, а у .mid второй обход - весь трек через
// конвертер заново.
//
// Зовётся на настоящей смене строки, в порядке воспроизведения. Позиция
// может и убывать (переход назад), поэтому первая и последняя позиции
// сэмпла считаются минимумом и максимумом, а не присваиванием.
using RowObserver = void (*)(void* user, const soundsinth::model::PatternCell* cells, uint8_t channel_count, uint16_t order_pos);

// Чем занять ожидание: проход идёт по всему треку и на плате занимает
// секунды, а виток Core1 всё это время не крутится - эмулятор карты стоит,
// и заказ сектора от esxDOS ждёт конца прохода. Зовётся на границе позиции
// order, не чаще. nullptr - обслуживать нечем (ПК).
using ServiceFn = void (*)(void* user);

uint32_t compute_song_total_frames(const soundsinth::model::Song& song, memory::PsramStore& psram, uint8_t* scratch, uint32_t scratch_bytes,
                                   DurationStats* stats = nullptr, RowObserver on_row = nullptr, void* on_row_user = nullptr, ServiceFn service = nullptr,
                                   void* service_user = nullptr);
const char* duration_stop_name(DurationStop stop);

// Сколько позиций order трек проходит за первые frames выходных отсчётов.
// Нужно префетчу: "загрузить первые N секунд и стартовать".
//
// Считается тем же проходом секвенсора, что и длительность: темп и
// скорость меняются эффектами в паттернах (у .mid на каждой смене темпа),
// приближение промахнулось бы там, где темп рваный.
//
// Возвращает число вошедших позиций (минимум 1), если трек кончился
// раньше - все его позиции. 0 - посчитать не удалось (нет буфера).
uint16_t compute_order_positions_in_frames(const soundsinth::model::Song& song, memory::PsramStore& psram, uint8_t* scratch, uint32_t scratch_bytes,
                                           uint32_t frames);

} // namespace soundsinth::engine
