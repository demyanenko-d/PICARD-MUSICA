// SPDX-License-Identifier: MIT
#pragma once

// Связь управляющей части со звуковой: единственное место, через которое тик
// командует голосами. Команды складываются в кольцо и применяются звуковой
// частью перед сведением батча (flush). Связь односторонняя: обратно не едет
// ничего. Всё, что тику нужно знать о голосах, он считает сам - звучание по
// шагу и границам сэмпла, гашение по его длине, фильтр и перезапуск как эхо
// собственных команд. Поэтому стороны разводятся по задачам и ядрам, а
// секвенсор волен считать тики вперёд.
//
// Высоту считает тик: звуковой уходит готовый шаг, кэш входов питча - здесь.

#include <cstdint>

#include "core/config.h"
#include "core/engine/voice_mixer.h"
#include "core/engine/voice_queue.h"

namespace soundsinth::engine {

class VoiceLink {
public:
    // mixer и psram живут не меньше связи; ramp_samples у микшера уже задан.
    void init(VoiceMixer* mixer, memory::PsramStore* psram) {
        mixer_        = mixer;
        psram_        = psram;
        ramp_samples_ = mixer->ramp_samples;
        for (uint16_t s = 0; s < SOUNDSINTH_MAX_SLOTS; ++s) {
            restart_[s]       = true; // VoiceRamp нового слота ждёт разгона с тишины
            filter_on_[s]     = false;
            gain_l_[s]        = 0;
            gain_r_[s]        = 0;
            step_[s]          = 0;
            enc_[s]           = soundsinth::model::ResidentEncoding::Raw8;
            pitch_memo_[s]    = 0;
            pitch_memo_c5_[s] = 0;
        }
    }

    // Начало тика: кадры, сведённые с прошлого тика, двигают модели. То, что
    // случилось само - конец сэмпла, доигранное гашение, - тик отсюда и
    // узнаёт, не спрашивая звуковую сторону.
    void begin_tick(uint32_t frames_since_last_tick) {
        advance_tails(frames_since_last_tick);
        advance_sounding(frames_since_last_tick);
    }

    // Конец тика: недописанная пачка номеров списка - в кольцо. После этого
    // тик к кольцу не прикасается.
    void end_tick() { flush_pending_list(); }

    // Звуковая сторона: применить накопленные команды. Зовётся перед сведением
    // батча, то есть после тика.
    void flush();

    // Наибольшее заполнение кольца за трек.
    uint32_t queue_peak() const { return queue_.peak(); }
    uint32_t queue_full() const { return queue_full_; }

    // --- Что тик знает о голосах: всё это он считает сам ---
    // Звучит ли голос: позиция растёт на шаг за кадр, и, дойдя до конца без
    // петли, голос молкнет. Сверено с микшером на всей коллекции эталонов -
    // ноль расхождений.
    bool playing(uint8_t slot) const { return sounding_[slot]; }
    // Гаснет ли слот - по модели тика: гашение длится ровно ramp_samples
    // отсчётов, и при исчерпании данных волновое переходит в замороженное с тем
    // же остатком. Модель завышает - тик считает хвост начавшимся всегда, - и
    // завышение безопасно: слот подольше не отдаётся новой ноте.
    bool fading(uint8_t slot) const { return tail_frames_[slot] != 0; }
    // Сколько слотов догашивается волной: голоса в них нет, а процессор они
    // едят как живые - тот же voice_render с распаковкой, и потолок полифонии
    // считает их наравне со звучащими. Замороженные хвосты сюда не входят. У
    // трекеров волновых хвостов нет вовсе.
    uint8_t fading_count() const {
        uint8_t n = 0;
        for (uint16_t s = 0; s < SOUNDSINTH_MAX_SLOTS; ++s) {
            if (tail_wave_[s] && !sounding_[s]) ++n;
        }
        return n;
    }
    // Фильтр включает и снимает тик; перезапуск он ставит на запуске ноты и
    // снимает первым же сглаживанием - ровно там, где это делает микшер.
    bool filter_on(uint8_t slot) const { return filter_on_[slot]; }
    bool restart(uint8_t slot) const { return restart_[slot]; }

    // Слышимость голоса на этом тике: |gain_l| + |gain_r| (модуль - из-за
    // surround), в ней все множители и панорама.
    int64_t loudness(uint8_t slot) const {
        const int32_t gl = gain_l_[slot];
        const int32_t gr = gain_r_[slot];
        return static_cast<int64_t>(gl < 0 ? -gl : gl) + static_cast<int64_t>(gr < 0 ? -gr : gr);
    }

    // Цена голоса: кодек, шаг и включённость фильтра.
    uint32_t cost_ns(uint8_t slot) const { return voice_cost_ns(enc_[slot], step_[slot], filter_on(slot)); }

    // --- Список голосов этого тика ---
    uint8_t list_size() const { return list_count_; }
    uint8_t list_at(uint8_t k) const { return list_[k]; }
    void list_clear() {
        list_count_ = 0;
        send(VoiceOp::ListClear, 0);
    }
    // Номера копятся и уходят пачкой: подряд их бывает под сотню, и запись на
    // каждый съедала бы кольцо. Пачка уходит перед любой другой командой -
    // порядок в кольце тот же, что у вызовов.
    void list_push(uint8_t slot) {
        list_[list_count_++]                 = slot;
        pending_list_[pending_list_count_++] = slot;
        if (pending_list_count_ == kListPerCommand) flush_pending_list();
    }
    // Убрать k-й: на его место переезжает последний. Порядок не важен -
    // сведение ассоциативно.
    void list_remove_at(uint8_t k) {
        list_[k] = list_[list_count_ - 1];
        --list_count_;
        VoiceCommand cmd{VoiceOp::ListRemoveAt, 0, k, 0, {}};
        push(cmd);
    }

    // --- Команды голосу ---
    // Гашения здесь нет: микшер на снятии тоже только гасит признак, а хвост
    // заводит пересборка списка.
    void stop(uint8_t slot) {
        sounding_[slot] = false;
        send(VoiceOp::Stop, slot);
    }

    void trigger(uint8_t slot, const soundsinth::model::SampleDescriptor& sample, uint16_t first_page, uint8_t note,
                 soundsinth::model::FrequencyModel frequency_model, uint32_t start_offset, soundsinth::model::QuirkFlags quirks,
                 uint16_t checkpoint_first_page);

    // Высота звучащего голоса: шаг считает тик, вниз уходит готовое число. Тот
    // же вход - выход сразу (кэш питча): иначе каждый голос каждый тик стоил бы
    // 64-битного деления, а у модели Linear - pow.
    void set_pitch_amiga(uint8_t slot, uint16_t period, uint32_t c5_speed) {
        if (!playing(slot)) return; // нечего двигать
        if (pitch_memo_[slot] == period && pitch_memo_c5_[slot] == c5_speed) return;
        pitch_memo_[slot]    = period;
        pitch_memo_c5_[slot] = c5_speed;
        step_[slot]          = voice_step_amiga(period, c5_speed);
        send_step(slot);
    }
    void set_pitch_linear(uint8_t slot, int32_t amount_units, uint32_t c5_speed) {
        if (!playing(slot)) return;
        if (pitch_memo_[slot] == amount_units && pitch_memo_c5_[slot] == c5_speed) return;
        pitch_memo_[slot]    = amount_units;
        pitch_memo_c5_[slot] = c5_speed;
        step_[slot]          = voice_step_linear(amount_units, c5_speed);
        send_step(slot);
    }

    void fade_before_missing(uint8_t slot);

    void move(uint8_t from, uint8_t to);

    // Снятый голос: гашение последнего значения, если оно слышно; усиления - в ноль.
    void fade(uint8_t slot) {
        model_tail(slot);
        send(VoiceOp::Fade, slot);
    }

    // Голоса списка, снятые с прошлого тика: последнее значение гаснет;
    // wave_tail (.mid) - гаснет продолжение волны.
    void fade_stopped(bool wave_tail);

    // --- Команды сведения ---
    void set_gains(uint8_t slot, int32_t l, int32_t r) {
        gain_l_[slot] = l;
        gain_r_[slot] = r;
        VoiceCommand cmd{VoiceOp::SetGains, slot, 0, 0, {}};
        cmd.u.val.x = l;
        cmd.u.val.y = r;
        push(cmd);
    }
    // Усиления этого тика и сглаживание к ним: их всегда ставят вместе, и
    // одна запись в кольце вместо двух.
    void set_gains_ramp(uint8_t slot, int32_t l, int32_t r) {
        gain_l_[slot] = l;
        gain_r_[slot] = r;
        if (restart_[slot]) {
            // Перезапущенный голос стартует с тишины: прежнее значение гаснет
            // параллельно разгону новой ноты, волновой хвост продолжать нечем.
            model_tail(slot); // прежняя нота гаснет параллельно разгону новой
            restart_[slot] = false;
        }
        VoiceCommand cmd{VoiceOp::SetGainsRamp, slot, 0, 0, {}};
        cmd.u.val.x = l;
        cmd.u.val.y = r;
        push(cmd);
    }
    // Фильтр уже снят - команды нет: у трекерных файлов это почти все голоса
    // каждый тик.
    void filter_off(uint8_t slot) {
        if (!filter_on(slot)) return;
        filter_on_[slot] = false;
        send(VoiceOp::FilterOff, slot);
    }
    void set_filter(uint8_t slot, const FilterCoeffs& c);
    // Маршрут держится в голосе между тиками: та же пара - команды нет. У
    // трекерных файлов посыла нет вовсе, и после первого тика маршрут молчит.
    void set_route(uint8_t slot, bool to_discard, uint8_t send_level) {
        const uint8_t packed = static_cast<uint8_t>(send_level | (to_discard ? 0x80u : 0u));
        if (route_known_[slot] && route_[slot] == packed) return;
        route_known_[slot] = true;
        route_[slot]       = packed;
        VoiceCommand cmd{VoiceOp::SetRoute, slot, send_level, static_cast<uint8_t>(to_discard ? 1 : 0), {}};
        push(cmd);
    }

private:
    void flush_pending_list() {
        if (pending_list_count_ == 0) return;
        VoiceCommand cmd{VoiceOp::ListPush, 0, pending_list_count_, 0, {}};
        for (uint8_t k = 0; k < pending_list_count_; ++k) {
            cmd.u.slots[k] = pending_list_[k];
        }
        pending_list_count_ = 0;
        push_raw(cmd);
    }

    // Команда без данных.
    void send(VoiceOp op, uint8_t slot) {
        VoiceCommand cmd{op, slot, 0, 0, {}};
        push(cmd);
    }
    void send_step(uint8_t slot) {
        VoiceCommand cmd{VoiceOp::SetStep, slot, 0, 0, {}};
        cmd.u.val.z = step_[slot];
        push(cmd);
    }
    // Кольцо полно - разобрать прямо здесь и положить снова. На одном ядре это
    // просто более ранний разбор, порядок команд тот же; счётчик показывает,
    // что ёмкости не хватило.
    void push(const VoiceCommand& cmd) {
        flush_pending_list(); // порядок команд - как у вызовов
        push_raw(cmd);
    }
    void push_raw(const VoiceCommand& cmd) {
        if (queue_.push(cmd)) return;
        ++queue_full_;
        flush();
        queue_.push(cmd);
    }

    VoiceMixer* mixer_         = nullptr;
    memory::PsramStore* psram_ = nullptr;
    uint32_t ramp_samples_     = 0;
    VoiceQueue queue_;
    uint32_t queue_full_ = 0;

    // Фильтр ведёт сам тик: он его и включает, и снимает.
    bool filter_on_[SOUNDSINTH_MAX_SLOTS] = {};
    bool restart_[SOUNDSINTH_MAX_SLOTS]   = {};

    // Модель звучания голоса: тик считает её сам, чтобы не спрашивать микшер.
    // Позиция растёт на шаг за кадр, целая часть - декодированные отсчёты;
    // дойдя до конца без петли, голос молкнет. Всё это тик знает: шаг задаёт
    // он, сэмпл и петлю видит при запуске, число кадров в тике - тоже.
    uint32_t sounding_frac_[SOUNDSINTH_MAX_SLOTS]       = {}; // Q16.16
    uint32_t sounding_decoded_[SOUNDSINTH_MAX_SLOTS]    = {};
    uint32_t sounding_loop_start_[SOUNDSINTH_MAX_SLOTS] = {};
    uint32_t sounding_loop_end_[SOUNDSINTH_MAX_SLOTS]   = {};
    bool sounding_loop_[SOUNDSINTH_MAX_SLOTS]           = {};
    bool sounding_[SOUNDSINTH_MAX_SLOTS]                = {};

    // Модель гашения: сколько отсчётов слоту ещё гаснуть и каким хвостом.
    // Замороженный - одно умножение на отсчёт; волновой (.mid) продолжает
    // читать сэмпл и стоит как живой голос, поэтому считается отдельно.
    uint32_t tail_frames_[SOUNDSINTH_MAX_SLOTS] = {};
    bool tail_wave_[SOUNDSINTH_MAX_SLOTS]       = {};
    void model_tail(uint8_t slot) {
        tail_frames_[slot] = ramp_samples_;
        tail_wave_[slot]   = false;
    }
    void model_wave_tail(uint8_t slot) {
        tail_frames_[slot] = ramp_samples_;
        tail_wave_[slot]   = true;
    }
    void advance_tails(uint32_t frames) {
        for (uint16_t s = 0; s < SOUNDSINTH_MAX_SLOTS; ++s) {
            if (tail_frames_[s] == 0) continue;
            tail_frames_[s] = (tail_frames_[s] > frames) ? (tail_frames_[s] - frames) : 0;
            if (tail_frames_[s] == 0) tail_wave_[s] = false;
        }
    }

    // Кадры прошлого тика - по списку прошлого тика: голос, которого в нём не
    // было, микшер не рендерил, и позиция его не двигалась.
    void advance_sounding(uint32_t frames) {
        if (frames == 0) return;
        for (uint8_t k = 0; k < list_count_; ++k) {
            const uint8_t s = list_[k];
            if (!sounding_[s]) continue;
            const uint64_t total = static_cast<uint64_t>(sounding_frac_[s]) + static_cast<uint64_t>(frames) * static_cast<uint64_t>(step_[s]);
            uint32_t whole       = static_cast<uint32_t>(total >> 16);
            sounding_frac_[s]    = static_cast<uint32_t>(total) & 0xFFFFu;
            if (whole == 0) continue;
            if (!sounding_loop_[s]) {
                // Голос жив, пока успевает декодировать все нужные отсчёты:
                // микшер умирает не когда счётчик дошёл до конца, а когда
                // следующий отсчёт пришлось бы взять за концом.
                if (sounding_decoded_[s] + whole > sounding_loop_end_[s]) {
                    sounding_[s] = false; // конец незацикленного сэмпла
                    model_tail(s);        // обрыв на ненулевом отсчёте гасится
                    sounding_decoded_[s] = sounding_loop_end_[s];
                } else {
                    sounding_decoded_[s] += whole;
                }
                continue;
            }
            const uint32_t len = sounding_loop_end_[s] - sounding_loop_start_[s];
            if (len == 0) continue;
            uint32_t pos = sounding_decoded_[s] + whole;
            while (pos >= sounding_loop_end_[s]) {
                pos -= len;
            }
            sounding_decoded_[s] = pos;
        }
    }
    int32_t gain_l_[SOUNDSINTH_MAX_SLOTS] = {};
    int32_t gain_r_[SOUNDSINTH_MAX_SLOTS] = {};
    // Цена голоса: шаг и кодек резидентного сэмпла.
    uint32_t step_[SOUNDSINTH_MAX_SLOTS]                           = {};
    soundsinth::model::ResidentEncoding enc_[SOUNDSINTH_MAX_SLOTS] = {};
    // Кэш входов питча: по ним посчитан шаг.
    int32_t pitch_memo_[SOUNDSINTH_MAX_SLOTS]     = {};
    uint32_t pitch_memo_c5_[SOUNDSINTH_MAX_SLOTS] = {};

    // Последний посланный маршрут слота: посыл и признак discard в старшем бите.
    uint8_t route_[SOUNDSINTH_MAX_SLOTS]    = {};
    bool route_known_[SOUNDSINTH_MAX_SLOTS] = {};

    uint8_t list_[SOUNDSINTH_MAX_SLOTS] = {};
    uint8_t list_count_                 = 0;
    // Недописанная пачка номеров для кольца.
    uint8_t pending_list_[kListPerCommand] = {};
    uint8_t pending_list_count_            = 0;
};

} // namespace soundsinth::engine
