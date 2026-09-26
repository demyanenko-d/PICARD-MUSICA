#pragma once

// Звуковая часть движка: то, что работает на каждом выходном отсчёте, -
// голоса, их фильтры, сглаживание громкости, гашение, сведение в шину и
// посыл в ревербератор. Ничего не решает: список голосов, усиления,
// коэффициенты фильтров, посыл и маршрут ставит управляющая часть раз в тик
// (TrackerEngine), здесь они только применяются к отсчётам.

#include <cstdint>

#include "core/config.h"
#include "core/engine/engine_defs.h"
#include "core/engine/resonant_filter.h"
#include "core/engine/voice.h"
#include "core/memory/psram_store.h"
#include "core/audio/mixbus.h"

namespace soundsinth::engine {

// Дробных бит у усиления голоса: Q0.16 громкостей, сдвинутое в масштаб
// шины Q24.8 (mixbus::kMixFracBits). SMLAWB отсчёта на такое усиление
// кладёт в накопитель шины 8 бит дроби.
inline constexpr uint32_t kGainQ24Bits = kQ16Bits + mixbus::kMixFracBits;

// Сглаживание громкости и гашение снятого голоса, на слот. 32 байта без
// дырок: адрес слота - сдвиг.
struct VoiceRamp {
    int32_t gain_l = 0; // текущая сглаженная громкость, масштаб усилений голоса
    int32_t gain_r = 0;
    int32_t step_l = 0; // прибавка на отсчёт, знаковая
    int32_t step_r = 0;
    // Усиление в начале гашения: величина - кривая от остатка, а не
    // накопление шага.
    int32_t tail_start_gain_l = 0;
    int32_t tail_start_gain_r = 0;

    int16_t last_output = 0; // последнее выданное значение живого голоса
    // Гасимое значение. Отдельно от last_output: гашение, не уложившееся в
    // батч, к следующему батчу видело бы там отсчёт новой ноты.
    int16_t fading_sample = 0;

    uint8_t ramp_remaining = 0; // отсчётов сглаживания осталось
    // Голос кончается там, где кончились данные, редко в нуле, и обрыв
    // слышен как ступенька: последнее выданное значение гасится до тишины.
    // У .mid вместо замороженного значения гасится продолжение самого голоса
    // (волновое гашение): заморозка даёт постоянную составляющую на всё
    // время гашения.
    uint8_t wave_remaining = 0; // отсчётов волнового гашения осталось
    uint8_t tail_remaining = 0; // отсчётов гашения осталось
    // Голос перезапущен новой нотой: начинать с тишины, а не с громкости
    // предыдущей ноты, иначе на склейке тот же щелчок.
    bool restart = true;

    bool fading() const { return tail_remaining != 0 || wave_remaining != 0; }
};
static_assert(sizeof(VoiceRamp) == 32, "VoiceRamp - 32 байта, адрес слота сдвигом");

// Цена голоса по модели SOUNDSINTH_VOICE_COST_*: из кодека, шага и
// включённости фильтра. Абсолютные наносекунды приблизительны, соотношение
// между голосами верное. Формула одна на обе стороны: звуковая считает её по
// голосу, управляющая - по своим шагу и кодеку.
inline uint32_t voice_cost_ns(soundsinth::model::ResidentEncoding encoding, uint32_t step, bool filter_on) {
    uint32_t cost = SOUNDSINTH_VOICE_COST_BASE_NS;
    if (soundsinth::model::resident_is_direct(encoding)) {
        cost += SOUNDSINTH_VOICE_COST_JUMP_NS; // прыжок за постоянное время - высота на цену не влияет
    } else {
        // Dpcm8: распаковка последовательная, step вызовов на выходной отсчёт.
        cost += static_cast<uint32_t>((static_cast<uint64_t>(step) * SOUNDSINTH_VOICE_COST_DECODE_NS) >> kQ16Bits);
    }
    if (filter_on) cost += SOUNDSINTH_VOICE_COST_FILTER_NS;
    return cost;
}

// Слот - живой канал или фоновый голос NNA, индексация общая с состоянием
// каналов управляющей части.
class VoiceMixer {
public:
    // --- Состояние на отсчёт, на слот ---
    Voice voices[SOUNDSINTH_MAX_SLOTS];
    VoiceRamp ramp[SOUNDSINTH_MAX_SLOTS];
    // Резонансный фильтр IT: коэффициенты ставит тик, состояние живёт между
    // тиками. Здесь, а не в Voice: Voice на самом горячем пути декодера.
    FilterCoeffs filter_coeffs[SOUNDSINTH_MAX_SLOTS];
    FilterState filter_state[SOUNDSINTH_MAX_SLOTS];

    // --- Ставит тик, на слот ---
    // Множители L/R в масштабе шины (единица 1 << kGainQ24Bits): громкость,
    // огибающая, панорама и общие множители свёрнуты в одно число. Знаковые:
    // у surround правое отрицательное.
    int32_t gain_l_q24[SOUNDSINTH_MAX_SLOTS] = {};
    int32_t gain_r_q24[SOUNDSINTH_MAX_SLOTS] = {};
    // Посыл в ревербератор 0..127 (0 - нет) и маршрут: true - свести в
    // discard (solo другого канала). Нули с самого начала: гаснущий слот
    // сводится и до того, как тик впервые задал ему маршрут, а discard_l без
    // solo - нулевой указатель.
    uint8_t reverb_send[SOUNDSINTH_MAX_SLOTS] = {};
    bool discard[SOUNDSINTH_MAX_SLOTS] = {};
    // Голоса, которые сводятся на этом тике.
    uint8_t active[SOUNDSINTH_MAX_SLOTS];
    uint8_t active_count = 0;
    // Слоты, которые только догашиваются: голоса в них нет, и список тика их
    // не несёт - он про звучащее. Собираются здесь раз в батч, иначе гашение
    // обрывалось бы на границе тика щелчком.
    uint8_t tails[SOUNDSINTH_MAX_SLOTS];
    uint8_t tail_count = 0;

    // Волновое гашение (.mid) доигрывает в слоте умершего голоса и держит его
    // занятым ramp_samples отсчётов. Понадобился слот раньше - хвост
    // обрывается: started сколько начато, cut_note сколько убито новой нотой
    // слота, cut_move переездом голоса NNA.
    uint32_t wave_tail_started = 0;
    uint32_t wave_tail_cut_note = 0;
    uint32_t wave_tail_cut_move = 0;

    // --- Постоянное на трек ---
    uint32_t ramp_samples = 0;
    int32_t* reverb_bus = nullptr; // nullptr - посылов нет
    int32_t* discard_l = nullptr;
    int32_t* discard_r = nullptr;

    // --- Команды тика на голос слота ---

    // Голос звучит (читает сэмпл).
    bool playing(uint8_t slot) const { return voices[slot].active; }
    // Снять голос; гашение последнего значения - на пересборке списка.
    void stop(uint8_t slot) { voices[slot].active = false; }

    // Нота или Retrigger: голос с отсчёта offset, разгон громкости с тишины,
    // память фильтра с нуля - как в OpenMPT: иначе новая нота стартует с
    // хвоста прошлой в обратной связи. Начало ноты разобрано тиком, шаг ему
    // же ставить: высота приходит отдельной командой до рендера.
    void trigger(uint8_t slot, memory::PsramStore& psram, const soundsinth::model::SampleDescriptor& sample,
                 uint16_t first_page, uint16_t checkpoint_first_page, const TriggerStart& start, bool hermite) {
        voice_trigger_prepared(voices[slot], psram, sample, first_page, checkpoint_first_page, start, hermite);
        ramp[slot].restart = true;
        filter_state[slot] = FilterState{};
    }

    // Высота звучащего голоса готовым шагом Q16.16: считает её тик, здесь
    // только присвоение. На снятом голосе двигать нечего.
    void set_step(uint8_t slot, uint32_t step) {
        if (!voices[slot].active) return;
        voices[slot].step = step;
    }

    // Нота без сэмпла (.mid): новый голос слота не запустится, прежний гаснет
    // сейчас последним значением; идущий волновой хвост доигрывается той же
    // кривой с того же места.
    void fade_before_missing(uint8_t slot);

    // Увод в фон (NNA): голос слота from продолжает звучать в слоте to -
    // позиция, память и коэффициенты фильтра, сглаживание громкости. Прежний
    // голос слота to гаснет параллельно.
    void move(uint8_t from, uint8_t to);

    // --- Команды тика: список голосов этого тика ---
    uint8_t list_size() const { return active_count; }
    uint8_t list_at(uint8_t k) const { return active[k]; }
    void list_clear() { active_count = 0; }
    void list_push(uint8_t slot) { active[active_count++] = slot; }
    // Убрать k-й: на его место переезжает последний. Порядок не важен -
    // сведение ассоциативно.
    void list_remove_at(uint8_t k) {
        active[k] = active[active_count - 1];
        --active_count;
    }

    // Голоса списка, снятые с прошлого тика: последнее значение гаснет за
    // ramp_samples отсчётов; wave_tail (.mid) - гаснет продолжение волны.
    void fade_stopped(bool wave_tail);
    // Снятый голос: гашение последнего значения, если оно слышно; усиления -
    // в ноль.
    void fade(uint8_t slot) { fade_out(ramp[slot]); }
    bool fading(uint8_t slot) const { return ramp[slot].fading(); }
    bool wave_fading(uint8_t slot) const { return ramp[slot].wave_remaining != 0; }
    bool tail_fading(uint8_t slot) const { return ramp[slot].tail_remaining != 0; }

    // --- Команды тика: сведение голоса ---
    // Громкости L/R этого тика (масштаб шины).
    void set_gains(uint8_t slot, int32_t l, int32_t r) {
        gain_l_q24[slot] = l;
        gain_r_q24[slot] = r;
    }
    // Антиклик: новая громкость - не значение, а цель, до которой голос
    // доезжает за ramp_samples отсчётов. Перезапущенный голос - с тишины.
    void ramp_to_gains(uint8_t slot);
    bool filter_on(uint8_t slot) const { return filter_coeffs[slot].active; }
    void filter_off(uint8_t slot) { filter_coeffs[slot].active = false; }
    void set_filter(uint8_t slot, const FilterCoeffs& c) { filter_coeffs[slot] = c; }
    void set_route(uint8_t slot, bool to_discard, uint8_t send) {
        discard[slot] = to_discard;
        reverb_send[slot] = send;
    }

    // Гашение последнего выданного значения до тишины за ramp_samples отсчётов.
    void start_tail(VoiceRamp& r, int32_t from_l, int32_t from_r) const;
    // Снятый голос: гашение последнего значения, если оно слышно; усиления -
    // в ноль.
    void fade_out(VoiceRamp& r) const;

    // Цена звучащих голосов списка: оценка загрузки там, где часов нет.
    uint64_t list_cost_ns() const {
        uint64_t sum = 0;
        for (uint8_t k = 0; k < active_count; ++k) {
            const uint8_t idx = active[k];
            if (voices[idx].active) sum += voice_cost_ns(voices[idx].resident_encoding, voices[idx].step, filter_coeffs[idx].active);
        }
        // Волновое гашение стоит как живой голос: оно читает сэмпл тем же
        // декодером. Замороженное - одно умножение, в счёт не идёт.
        for (uint8_t k = 0; k < tail_count; ++k) {
            const uint8_t idx = tails[k];
            if (ramp[idx].wave_remaining != 0) {
                sum += voice_cost_ns(voices[idx].resident_encoding, voices[idx].step, filter_coeffs[idx].active);
            }
        }
        return sum;
    }

    // Догашиваемые слоты, которых нет в списке тика. Кто гаснет, знает
    // звуковая сторона: гашение начинают её же правила, и длится оно
    // ramp_samples отсчётов.
    void collect_tails();

    // Свести batch отсчётов голосов списка в mix_l/mix_r начиная с отсчёта
    // at; посыл - в reverb_bus не дальше reverb_frames. Внутри батча
    // границы тика нет.
    void mix(memory::PsramStore& psram, int32_t* mix_l, int32_t* mix_r, uint32_t at, uint32_t batch,
             uint32_t reverb_frames);
};

} // namespace soundsinth::engine
