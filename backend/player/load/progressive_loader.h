// SPDX-License-Identifier: MIT
#pragma once

// Фоновая догрузка сэмплов под звучащий трек: выбор очередного сэмпла
// плана с упреждением и вытеснение отыгравших, когда память кончилась.
//
// Без шины, ядер и журнала: позицию, занятые голосами сэмплы и саму
// загрузку отдаёт вызывающий, план заполняет plan_playback_order.

#include <atomic>
#include <cstdint>

#include "core/engine/engine_defs.h"
#include "core/memory/track_memory.h"

#include "player/load/sample_prefetch.h" // kSampleNeverUsed

namespace player::load {

// Потолок числа сэмплов плана: три массива по 2 байта на сэмпл. Файл с
// большим числом сэмплов грузится целиком до старта звука - деградация, а
// не отказ.
//
// 810 - сколько влезает в буфер загрузчика. По архиву из 45095 файлов IT и
// XM (только они выходят за 512) больше всех у одного файла - 778 сэмплов.
inline constexpr uint16_t kProgressiveMaxSamples = 810;

// Память трёх массивов плана.
inline constexpr uint32_t kProgressivePlanBytes = 3u * kProgressiveMaxSamples * sizeof(uint16_t);

// Кольцо заказов живого потока (ProgressiveSource::Demand) - степень
// двойки: голова и хвост свободно бегущие, заворачиваются маской, и
// переполнение счётчика обязано попадать на границу кольца. Потолок плана
// степенью двойки быть не обязан: там очередь линейная.
inline constexpr uint16_t kDemandRingCapacity = 512;
static_assert((kDemandRingCapacity & (kDemandRingCapacity - 1u)) == 0, "the request ring is a power of two");
static_assert(kDemandRingCapacity <= kProgressiveMaxSamples, "the ring lives in the same plan arrays");

// Памяти вдоволь, пока свободно больше 1/kAmpleFreeDivisor зоны сэмплов:
// тогда упреждения нет.
inline constexpr uint32_t kAmpleFreeDivisor = 4;

// Сколько раз на шаг вытеснить сэмпл и повторить загрузку.
inline constexpr uint16_t kMaxEvictRetries = 32;

// Записей плана за шаг повторного прохода.
inline constexpr uint16_t kRescanPerStep = 4;

// Откуда запросы и кого вытеснять.
//   Plan   - файл: что и когда прозвучит, известно заранее (план по
//            позициям order); жертва - сэмпл, нужный позже всех (по карте
//            позиций), без карты - отыгравший.
//   Demand - живой поток: будущего нет; запросы по событиям
//            (progressive_request), жертва - дольше всех не звучавший.
// Каталог, загрузка, повтор после вытеснения и карта занятых - общие.
//
// Demand - задел под живой MIDI: на плате его пока не зовёт никто, живой
// сеанс ходит своим кольцом заказов и без вытеснения.
enum class ProgressiveSource : uint8_t { Plan, Demand };

// Три массива плана по capacity элементов: очередь, последние и первые
// позиции order по номеру сэмпла. indices nullptr - массивов нет.
struct PlanArrays {
    uint16_t* indices   = nullptr;
    uint16_t* last_use  = nullptr;
    uint16_t* first_use = nullptr;
    uint16_t capacity   = 0;
};

struct ProgressiveLoader {
    // Массивы плана по kProgressiveMaxSamples элементов даёт вызывающий: у
    // платы они в TrackMemory::loader_scratch_buffer на время сессии, у ПК
    // свои. Пока указателей нет, plan_count обязан быть 0.
    //
    // Plan: plan_indices - очередь (отсортирована по file_offset), голова
    // plan_next, конец plan_count. last_use и first_use - по номеру сэмпла:
    // последняя позиция order, где он звучит (жертва вытеснения без карты
    // позиций), и первая ("когда понадобится?", граница упреждения).
    //
    // Demand: plan_indices - кольцо запросов (голова plan_next, хвост
    // plan_count, по модулю kDemandRingCapacity). last_use - когда сэмпл
    // звучал или загружен в последний раз (часы progressive_note_in_use),
    // first_use - kSampleNeverUsed, если сэмпла нет в кольце.
    uint16_t* plan_indices   = nullptr;
    uint16_t* plan_last_use  = nullptr;
    uint16_t* plan_first_use = nullptr;
    uint16_t plan_count      = 0;
    uint16_t plan_next       = 0;
    // Курсор повторного прохода по плану, когда очередь пройдена: очередь
    // отдаёт каждую запись один раз, а вытесненный сэмпл или не влезший с
    // первого раза без повторного заказа не вернуть. Проход идёт по кругу,
    // по kRescanPerStep записей за шаг.
    uint16_t plan_rescan = 0;
    uint16_t reloaded    = 0; // сколько загружено повторным проходом
    // Сколько раз повторный проход не вернул сэмпл: не влез и после
    // одного вытеснения.
    uint16_t rescan_failed = 0;
    // Plan: на каких позициях звучит каждый сэмпл. Есть - жертва вытеснения
    // по самому дальнему следующему использованию, повторный проход
    // возвращает только нужное в пределах упреждения. Нет - по last_use.
    UseMap use_map;
    ProgressiveSource source = ProgressiveSource::Plan;
    // Demand: часы последнего progressive_note_in_use.
    uint16_t clock = 0;
    // Упреждение фоновой догрузки в позициях order: 0 - без упреждения
    // (весь план подряд). У платы - только у .mid.
    uint16_t lead_positions = 0;
    uint16_t plan_failed    = 0;
    uint16_t evicted        = 0;
    // Жертва отклонена потолком карты занятых, а не живым голосом: сэмплы с
    // номером от bit_count читатель считает занятыми вечно, и у богатого
    // файла их не вытеснить никогда. В логе это выглядит нехваткой PSRAM.
    uint16_t evict_capped = 0;
    // Вытеснения нет (progressive_eviction_blocked).
    bool no_eviction = false;
    // Докуда дошёл просмотр хвоста при упреждении и наименьшая первая
    // позиция среди пропущенного. Пока горизонт ниже её, пропущенное
    // пересматривать незачем - просмотр идёт с lead_scan.
    //
    // Выданный сэмпл вынимается сдвигом, а не перестановкой, поэтому
    // пропущенный префикс остаётся на месте относительно plan_next, и
    // отметка переживает выдачу.
    uint16_t lead_scan           = 0;
    uint16_t lead_skip_min_first = kSampleNeverUsed;
};

// Три массива плана - подряд в одном блоке kProgressivePlanBytes: очередь,
// последние позиции, первые позиции. У платы блок - loader_scratch_buffer.
// Раскладка блока - только в plan_arrays_in.
static_assert(kProgressivePlanBytes <= soundsinth::memory::kLoaderScratchBytes, "the plan arrays live in loader_scratch_buffer");
inline PlanArrays plan_arrays_in(uint16_t* mem) {
    return PlanArrays{mem, mem + kProgressiveMaxSamples, mem + 2u * kProgressiveMaxSamples, kProgressiveMaxSamples};
}

inline void progressive_attach_plan(ProgressiveLoader& pl, uint16_t* mem) {
    const PlanArrays a = plan_arrays_in(mem);
    pl.plan_indices    = a.indices;
    pl.plan_last_use   = a.last_use;
    pl.plan_first_use  = a.first_use;
}

// Массивы, привязанные к загрузчику; не привязаны - пустые.
inline PlanArrays plan_arrays_of(const ProgressiveLoader& pl) {
    if (pl.plan_indices == nullptr) return PlanArrays{};
    return PlanArrays{pl.plan_indices, pl.plan_last_use, pl.plan_first_use, kProgressiveMaxSamples};
}

// План построен заново: запомненное для Waiting относится к прежнему.
inline void progressive_plan_changed(ProgressiveLoader& pl) {
    pl.lead_scan           = 0;
    pl.lead_skip_min_first = kSampleNeverUsed;
}

// Что занято голосами прямо сейчас: бит на сэмпл. У платы карту пишет
// другое ядро, поэтому слова атомарные и читаются с acquire. Сэмплы с
// индексом >= bit_count считаются занятыми.
struct SamplesInUse {
    const std::atomic<uint32_t>* bits = nullptr;
    uint16_t bit_count                = 0;
    bool over_cap(uint16_t idx) const { return idx >= bit_count; }
    bool is_busy(uint16_t idx) const { return over_cap(idx) || (bits[idx / 32u].load(std::memory_order_acquire) & (1u << (idx % 32u))) != 0; }
};

// Ближе этого к концу прохода вытеснение выключено: после оборота песни
// звучат строки вступления, а позиция обновляется с опозданием.
inline constexpr uint32_t kPassEndGuardFrames = soundsinth::engine::kSampleRateHz / 4;

// Вытеснять на этом шаге нельзя: переход назад посреди прохода (секвенсор
// вернётся к позиции, которую план считает отыгравшей), трек без
// длительности или конец прохода ближе kPassEndGuardFrames. Вызывающий
// кладёт это в ProgressiveLoader::no_eviction перед шагом. Цепочка,
// освобождённая между sample_cache_find() и voice_trigger() на ядре звука,
// даёт мусор в звуке.
inline bool progressive_eviction_blocked(bool position_goes_back, uint32_t total_frames, uint32_t played_frames) {
    return position_goes_back || total_frames == 0 || played_frames + kPassEndGuardFrames >= total_frames;
}

// Загрузить сэмпл index в память трека. reason_out - причина отказа.
using ProgressiveLoadFn = bool (*)(void* user, uint16_t sample_index, const char** reason_out);

// Перевести в Demand: кольцо пусто, ни один сэмпл не звучал. Массивы уже
// привязаны (progressive_attach_plan).
void progressive_start_demand(ProgressiveLoader& pl);

// Demand: поставить сэмпл в очередь загрузки. Уже в очереди - true без
// второй записи. false - индекс за планом или кольцо полно.
bool progressive_request(ProgressiveLoader& pl, uint16_t sample_index);

// Demand: отметить звучащие сэмплы временем now (единицы выбирает
// вызывающий, сравнение по модулю 2^16: старше 65535 единиц считается
// свежим - жертва неоптимальна, не опасна). Звать перед шагом загрузки.
void progressive_note_in_use(ProgressiveLoader& pl, const soundsinth::memory::TrackMemory& mem, const SamplesInUse& in_use, uint16_t now);

// Вытеснить один сэмпл, которого не держат голоса. Plan с картой позиций:
// самый дальний по следующему использованию; без карты - отыгравший,
// последняя позиция, где он звучит, меньше order_pos. Demand: дольше всех
// не звучавший, order_pos не нужен. false - вытеснять нечего.
// incoming - сэмпл, ради которого место: с картой позиций жертва нужна
// позже него, иначе вытеснение лишь меняет одну молчащую ноту на другую.
bool progressive_evict_one(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos, const SamplesInUse& in_use,
                           uint16_t incoming = kSampleNeverUsed);

enum class ProgressiveStep : uint8_t {
    Loaded,  // очередной сэмпл загружен (или уже был в памяти)
    Failed,  // не влез и после вытеснения - будет молчать
    Waiting, // своевременных нет - ждать позицию; Demand - очередь пуста
    Done,    // план кончился (у Demand не бывает)
};

// Один шаг догрузки. loaded_index - какой сэмпл обработан (Loaded/Failed).
ProgressiveStep progressive_load_next(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos, const SamplesInUse& in_use,
                                      ProgressiveLoadFn load, void* user, uint16_t* loaded_index = nullptr, const char** reason_out = nullptr);

} // namespace player::load
