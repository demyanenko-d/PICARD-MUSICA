#pragma once

#include <cstdint>

#include "core/config.h"
#include "core/audio/sound_source.h"

namespace soundsinth::mixbus {

// Шина источников звука: суммирует активные SoundSource в общий
// аккумулятор int32 (Q24.8, kMixFracBits), один раз округляет до целых и
// насыщает до int16. Без нормализации; лимитер и мягкое насыщение
// включает формат.
class MixBus {
public:
    static constexpr uint32_t kMaxSources = 8;

    // Порог мягкого насыщения по умолчанию - 0.75 шкалы, выбран на слух.
    static constexpr int32_t kSoftKneeDefault = 24576;

    bool add_source(SoundSource* source);
    void remove_source(SoundSource* source);

    // Рендерит ровно n_frames (<= SOUNDSINTH_AUDIO_BUFFER_FRAMES) в dst
    // (стерео int16, чередование).
    void render(int16_t* dst_interleaved, uint32_t n_frames);

    // Граница трека: счётчики шины, прошлый кадр для счёта разрывов и
    // состояние лимитера - с нуля. Звать при снятом рендере, рядом с
    // add_source(). Иначе сверка "плата против ПК тем же файлом" идёт
    // накопленным за всю работу против одного трека.
    void start_track() {
        rendered_frames_ = 0;
        end_frame_ = 0;
        fade_pos_ = kFadeOff;
        clipped_ = 0;
        longest_run_ = 0;
        run_ = 0;
        jumps_ = 0;
        max_jump_ = 0;
        prev_l_ = 0;
        prev_r_ = 0;
        soft_clipped_ = 0;
        gain_ = kUnityGain;
        limited_frames_ = 0;
        min_gain_ = kUnityGain;
    }

    // Кадр конца трека (0 - нет): за буфер до него выход гаснет, дальше -
    // нули. Начало второго прохода песни не звучит.
    void set_end_frame(uint32_t frame) { end_frame_ = frame; }
    // Перемотка: движок ушёл вперёд на frames кадров, шине их не отдав.
    // Считать конец трека по одному только своему счётчику после этого
    // нельзя - он отстаёт ровно на прыжок, затухание не успевает, и начало
    // второго прохода слышно поверх конца.
    void skip_frames(uint32_t frames) { rendered_frames_ += frames; }
    // Погасить выход за kFadeFrames кадров, дальше - нули (источники
    // по-прежнему зовутся: движок считает кадры до конца трека). Вместо
    // ступеньки в ноль при сносе трека посреди звука.
    void fade_out() {
        fade_dir_ = +1;
        if (fade_pos_ == kFadeOff) fade_pos_ = 0;
    }
    // Затухание кончилось: выход - нули.
    bool faded() const { return fade_pos_ == kFadeFrames; }

    // Пауза. Вход - тем же затуханием, что у конца трека: обрыв на полной
    // громкости щёлкает. Пока выход погашен, источники не вызываются, и
    // позиция стоит - движок её ведёт сам. Выход из паузы - нарастание по
    // той же кривой.
    void set_paused(bool on) {
        if (on == paused_) return;
        paused_ = on;
        if (on) {
            fade_out();
            return;
        }
        // Трек кончился, пока стояла пауза (перемотка в самый конец):
        // возвращать нечего, выход остаётся погашенным.
        if (end_frame_ != 0 && rendered_frames_ + kFadeFrames > end_frame_) {
            fade_dir_ = +1;
            fade_pos_ = kFadeFrames;
            return;
        }
        fade_dir_ = -1;
        if (fade_pos_ == kFadeOff) fade_pos_ = kFadeFrames;
    }
    bool paused() const { return paused_; }
    // Сколько раз render() ушёл по короткому пути паузы, не тронув
    // источники. По изменению этого счётчика видно, что рендер уже в
    // тишине: значит движок можно трогать снаружи - перематывать.
    // Одного изменения довольно: рендер один, проходы идут по очереди, и
    // пока пауза стоит, следующий уйдёт туда же.
    uint32_t silent_renders() const { return silent_renders_; }
    static constexpr uint32_t kFadeFrames = SOUNDSINTH_AUDIO_BUFFER_FRAMES;

    // Сколько кадров трека упёрлись в шкалу хотя бы одним каналом (до
    // мягкого насыщения), и самая длинная полка подряд. Отличает перегруз от
    // заминки рендера: на слух одинаково, лечатся противоположно.
    uint32_t clipped_frames() const { return clipped_; }
    uint32_t longest_clip_run() const { return longest_run_; }

    // Разрывы потока: кадры со скачком больше kJumpThreshold и самый большой
    // скачок. Законный скачок .mid на GeneralUser GS - до 15709, на SGM - до
    // 35402: счёт сверяется с рендером того же файла и банка на ПК. Молчит
    // при слышимых щелчках - порча после шины: DMA, PIO, провода, ЦАП.
    static constexpr int32_t kJumpThreshold = 24576;
    uint32_t jumps() const { return jumps_; }
    uint32_t max_jump() const { return max_jump_; }

    // Порог и доля ужатия компрессора меняются на лету - подбор кривой
    // замером без пересборки. Порог выше шкалы выключает компрессор.
    // Состояние лимитера не сбрасывает.
    void set_compressor(int32_t threshold, int32_t amount) {
        comp_threshold_ = threshold;
        comp_amount_ = amount;
    }

    // Лимитер. Звать рядом с add_source() при смене трека, после
    // start_track().
    void set_limiter(bool on) { limiter_ = on; }

    // Мягкое насыщение вместо полки: до порога сигнал не трогается, выше
    // загибается к шкале. Состояния нет, звать можно в любой момент.
    void set_soft_clip(bool on) { soft_clip_ = on; }

    // Порог мягкого насыщения в единицах int16 (1..32000).
    void set_soft_knee(int32_t knee);
    // Сколько кадров прошло через кривую насыщения.
    uint32_t soft_clipped_frames() const { return soft_clipped_; }

    // Сколько кадров лимитер ужимал и до какого усиления опускался (Q15,
    // 32768 - не трогал). Чтобы видеть, работает он или молчит, не на слух.
    uint32_t limited_frames() const { return limited_frames_; }
    uint32_t min_gain_q15() const { return min_gain_; }

private:
    static constexpr int32_t kUnityGain = 32768;   // Q15
    static constexpr uint32_t kFadeOff = 0xFFFFFFFFu;

    void apply_limiter(uint32_t n_frames);
    void apply_fade(int16_t* dst_interleaved, uint32_t n_frames);
    int32_t soft_clip(int32_t x) const;
    // Итоговый проход: kRound - округлить Q24.8 здесь же (без лимитера),
    // kSoft - мягкое насыщение вместо полки.
    template <bool kRound, bool kSoft>
    void write_frames(int16_t* dst_interleaved, uint32_t n_frames);
    void count_jump(int32_t dl, int32_t dr);
    void soft_clip_frame(int32_t l, int32_t r, int32_t& out_l, int32_t& out_r);

    SoundSource* sources_[kMaxSources] = {};
    uint32_t source_count_ = 0;
    uint32_t rendered_frames_ = 0;
    uint32_t end_frame_ = 0;
    uint32_t fade_pos_ = kFadeOff; // кадр затухания; kFadeFrames и больше - нули
    int8_t fade_dir_ = +1;         // +1 гасим, -1 возвращаем из паузы
    bool paused_ = false;
    uint32_t silent_renders_ = 0;
    int32_t mix_l_[SOUNDSINTH_AUDIO_BUFFER_FRAMES] = {};
    int32_t mix_r_[SOUNDSINTH_AUDIO_BUFFER_FRAMES] = {};
    uint32_t clipped_ = 0;
    uint32_t longest_run_ = 0;
    uint32_t run_ = 0;
    uint32_t jumps_ = 0;
    uint32_t max_jump_ = 0;
    int32_t prev_l_ = 0;
    int32_t prev_r_ = 0;
    bool limiter_ = false;
    bool soft_clip_ = false;
    uint32_t soft_clipped_ = 0;
    int32_t soft_knee_ = kSoftKneeDefault;
    uint32_t soft_span_sq_ = static_cast<uint32_t>(32767 - kSoftKneeDefault) * (32767 - kSoftKneeDefault);
    int32_t comp_threshold_ = SOUNDSINTH_MIDI_COMPRESSOR_THRESHOLD;
    int32_t comp_amount_ = SOUNDSINTH_MIDI_COMPRESSOR_AMOUNT;
    int32_t gain_ = kUnityGain;
    uint32_t limited_frames_ = 0;
    uint32_t min_gain_ = kUnityGain;
};

} // namespace soundsinth::mixbus
