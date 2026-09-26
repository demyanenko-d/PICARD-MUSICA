#pragma once

// Фоновая догрузка сэмплов под звучащий трек: выбор очередного сэмпла
// плана с упреждением и вытеснение отыгравших, когда память кончилась.
//
// Без шины, ядер и журнала: позицию, занятые голосами сэмплы и саму
// загрузку отдаёт вызывающий, план заполняет plan_playback_order.

#include <atomic>
#include <cstdint>

#include "core/engine/engine_defs.h"
#include "player/load/sample_prefetch.h" // kSampleNeverUsed
#include "core/memory/track_memory.h"

namespace player::load {

// Потолок числа сэмплов плана: три массива по 2 байта на сэмпл, 6 КБ. Файл
// с большим числом сэмплов грузится целиком до старта звука - деградация,
// а не отказ. 512 не хватало: bz_ult9.it (535 сэмплов) грузился целиком,
// 9.4 МБ до первого звука.
inline constexpr uint16_t kProgressiveMaxSamples = 1024;

// Память трёх массивов плана.
inline constexpr uint32_t kProgressivePlanBytes = 3u * kProgressiveMaxSamples * sizeof(uint16_t);

// Памяти вдоволь, пока свободно больше 1/kAmpleFreeDivisor зоны сэмплов:
// тогда упреждения нет.
inline constexpr uint32_t kAmpleFreeDivisor = 4;

// Сколько раз на шаг вытеснить отыгравший и повторить загрузку.
inline constexpr uint16_t kMaxEvictRetries = 32;

// Откуда запросы и кого вытеснять.
//   Plan   - файл: что и когда прозвучит, известно заранее (план по
//            позициям order); жертва - сэмпл, чья последняя позиция пройдена.
//   Demand - живой поток: будущего нет; запросы по событиям
//            (progressive_request), жертва - дольше всех не звучавший.
// Каталог, загрузка, повтор после вытеснения и карта занятых - общие.
enum class ProgressiveSource : uint8_t { Plan, Demand };

struct ProgressiveLoader {
    // Массивы плана по kProgressiveMaxSamples элементов даёт вызывающий: у
    // платы они в TrackMemory::loader_scratch_buffer на время сессии, у ПК
    // свои. Пока указателей нет, plan_count обязан быть 0.
    //
    // Plan: plan_indices - очередь (отсортирована по file_offset), голова
    // plan_next, конец plan_count. last_use и first_use - по номеру сэмпла:
    // последняя позиция order, где он звучит ("уже отыграл?", жертва
    // вытеснения), и первая ("когда понадобится?", граница упреждения).
    //
    // Demand: plan_indices - кольцо запросов (голова plan_next, хвост
    // plan_count, по модулю kProgressiveMaxSamples). last_use - когда сэмпл
    // звучал или загружен в последний раз (часы progressive_note_in_use),
    // first_use - kSampleNeverUsed, если сэмпла нет в кольце.
    uint16_t* plan_indices = nullptr;
    uint16_t* plan_last_use = nullptr;
    uint16_t* plan_first_use = nullptr;
    uint16_t plan_count = 0;
    uint16_t plan_next = 0;
    ProgressiveSource source = ProgressiveSource::Plan;
    // Demand: часы последнего progressive_note_in_use.
    uint16_t clock = 0;
    // Упреждение фоновой догрузки в позициях order: 0 - без упреждения
    // (весь план подряд). У платы - только у .mid.
    uint16_t lead_positions = 0;
    uint16_t plan_failed = 0;
    uint16_t evicted = 0;
    // Вытеснения нет (progressive_eviction_blocked).
    bool no_eviction = false;
    // Последний Waiting: наименьшая первая позиция по [plan_next, plan_count).
    // Пока план тот же, горизонт ниже неё - снова Waiting без просмотра.
    // kSampleNeverUsed - не запомнено.
    uint16_t waiting_min_first_use = kSampleNeverUsed;
    uint16_t waiting_plan_next = 0;
};

// Три массива плана - подряд в одном блоке kProgressivePlanBytes: очередь,
// последние позиции, первые позиции. У платы блок - loader_scratch_buffer.
static_assert(kProgressivePlanBytes <= soundsinth::memory::kLoaderScratchBytes, "массивы плана живут в loader_scratch_buffer");
inline void progressive_attach_plan(ProgressiveLoader& pl, uint16_t* mem) {
    pl.plan_indices = mem;
    pl.plan_last_use = mem + kProgressiveMaxSamples;
    pl.plan_first_use = mem + 2u * kProgressiveMaxSamples;
}

// План построен заново: запомненное для Waiting относится к прежнему.
inline void progressive_plan_changed(ProgressiveLoader& pl) {
    pl.waiting_min_first_use = kSampleNeverUsed;
}

// Что занято голосами прямо сейчас: бит на сэмпл. У платы карту пишет
// другое ядро, поэтому слова атомарные и читаются с acquire. Сэмплы с
// индексом >= bit_count считаются занятыми.
struct SamplesInUse {
    const std::atomic<uint32_t>* bits = nullptr;
    uint16_t bit_count = 0;
    bool is_busy(uint16_t idx) const {
        return idx >= bit_count || (bits[idx / 32u].load(std::memory_order_acquire) & (1u << (idx % 32u))) != 0;
    }
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

static_assert((kProgressiveMaxSamples & (kProgressiveMaxSamples - 1u)) == 0, "кольцо запросов - маской");

// Перевести в Demand: кольцо пусто, ни один сэмпл не звучал. Массивы уже
// привязаны (progressive_attach_plan).
void progressive_start_demand(ProgressiveLoader& pl);

// Demand: поставить сэмпл в очередь загрузки. Уже в очереди - true без
// второй записи. false - индекс за планом или кольцо полно.
bool progressive_request(ProgressiveLoader& pl, uint16_t sample_index);

// Demand: отметить звучащие сэмплы временем now (единицы выбирает
// вызывающий, сравнение по модулю 2^16: старше 65535 единиц считается
// свежим - жертва неоптимальна, не опасна). Звать перед шагом загрузки.
void progressive_note_in_use(ProgressiveLoader& pl, const soundsinth::memory::TrackMemory& mem, const SamplesInUse& in_use,
                             uint16_t now);

// Вытеснить один сэмпл, которого не держат голоса. Plan: отыгравший -
// последняя позиция, где он звучит, меньше order_pos. Demand: дольше всех
// не звучавший, order_pos не нужен. false - вытеснять нечего.
bool progressive_evict_one(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos,
                           const SamplesInUse& in_use);

enum class ProgressiveStep : uint8_t {
    Loaded,   // очередной сэмпл загружен (или уже был в памяти)
    Failed,   // не влез и после вытеснения - будет молчать
    Waiting,  // своевременных нет - ждать позицию; Demand - очередь пуста
    Done,     // план кончился (у Demand не бывает)
};

// Один шаг догрузки. loaded_index - какой сэмпл обработан (Loaded/Failed).
ProgressiveStep progressive_load_next(ProgressiveLoader& pl, soundsinth::memory::TrackMemory& mem, uint16_t order_pos,
                                      const SamplesInUse& in_use, ProgressiveLoadFn load, void* user,
                                      uint16_t* loaded_index = nullptr, const char** reason_out = nullptr);

} // namespace player::load
