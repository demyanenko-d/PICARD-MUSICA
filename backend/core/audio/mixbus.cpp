#include "core/audio/mixbus.h"

#include <cstring>

#include "platform/compiler.h"
#include "core/audio/saturate.h"

namespace soundsinth::mixbus {

// Мягкое насыщение: до порога сигнал не трогается, выше
// y = порог + запас * u/(1+u), u = (|x| - порог) / запас, запас = шкала -
// порог; шкалу выход не переходит. Точно, одно деление UDIV на отсчёт выше
// порога.
void MixBus::set_soft_knee(int32_t knee) {
    if (knee < 1) knee = 1;
    if (knee > 32000) knee = 32000;
    soft_knee_ = knee;
    const uint32_t span_u = static_cast<uint32_t>(32767 - knee);
    soft_span_sq_ = span_u * span_u;   // до 32766^2, в uint32 влезает
}

inline int32_t MixBus::soft_clip(int32_t x) const {
    const int32_t a = x < 0 ? -x : x;   // |x| до 8.4 млн (запас шины), переполнения нет
    if (a <= soft_knee_) return x;
    const uint32_t over = static_cast<uint32_t>(a - soft_knee_);
    // Частное усекается вниз, выход - вверх меньше чем на единицу; не выше
    // 32767: при частном 0 ровно порог + запас. С ростом входа частное не
    // растёт - монотонно.
    const uint32_t span = static_cast<uint32_t>(32767 - soft_knee_);
    const int32_t y = 32767 - static_cast<int32_t>(soft_span_sq_ / (span + over));
    return x < 0 ? -y : y;
}

bool MixBus::add_source(SoundSource* source) {
    if (source_count_ >= kMaxSources) {
        return false;
    }
    sources_[source_count_++] = source;
    return true;
}

void MixBus::remove_source(SoundSource* source) {
    for (uint32_t i = 0; i < source_count_; ++i) {
        if (sources_[i] == source) {
            sources_[i] = sources_[--source_count_];
            return;
        }
    }
}

// Лимитер прямого хода: без буфера, задержки и обратной связи. Пик кадра
// известен до записи кадра, ужатие ложится на тот же кадр, перелёта нет,
// потолок соблюдается точно без просмотра вперёд.
//
// Отдельным проходом, а не веткой основного цикла: без лимитера цена - одно
// сравнение на буфер, и трекерные форматы совпадают с эталонными плеерами.
// Деление - только на кадре выше порога компрессора или потолка, умножения
// пропускаются, пока усиление ровно единица.
void MixBus::apply_limiter(uint32_t n_frames) {
    for (uint32_t i = 0; i < n_frames; ++i) {
        const int32_t l = mix_l_[i];
        const int32_t r = mix_r_[i];
        // Модуль относительно своей границы: шкала несимметрична, -32768
        // законен, +32768 нет. ~x = -x-1 даёт ровно это одной командой.
        // Иначе законный -32768 считался бы перегрузом, и лимитер трогал
        // бы неклипующие файлы (на GeneralUser - шесть рендеров из 32).
        const int32_t al = l < 0 ? ~l : l;
        const int32_t ar = r < 0 ? ~r : r;
        const int32_t peak = al > ar ? al : ar;

        // Компрессор: поджимает всё громче порога на долю AMOUNT. Полку
        // не гарантирует, за это отвечает лимитер ниже.
        if (peak > comp_threshold_) {
            const int32_t q = static_cast<int32_t>(
                (static_cast<uint32_t>(comp_threshold_) << 15) / static_cast<uint32_t>(peak));
            const int32_t target = kUnityGain - (((kUnityGain - q) * comp_amount_) >> 15);
            if (target < gain_) gain_ = target;
        }
        // Лимитер. Порог - сама шкала: на неклипующем материале лимитер не
        // срабатывает; при выключенном компрессоре выход побайтово прежний.
        // Внешнее сравнение с порогом убирает умножение из обычного кадра.
        if (peak > SOUNDSINTH_MIDI_LIMITER_CEILING) {
            const int32_t after = static_cast<int32_t>((static_cast<int64_t>(peak) * gain_) >> 15);
            if (after > SOUNDSINTH_MIDI_LIMITER_CEILING) {
                // Деление 32-битное беззнаковое: потолок<<15 = 1073709056
                // влезает в uint32. С int64 компилятор вызывает
                // __aeabi_ldivmod вместо одной UDIV.
                gain_ = static_cast<int32_t>(
                    (static_cast<uint32_t>(SOUNDSINTH_MIDI_LIMITER_CEILING) << 15) /
                    static_cast<uint32_t>(peak));
            }
        }

        if (gain_ < kUnityGain) {
            mix_l_[i] = static_cast<int32_t>((static_cast<int64_t>(mix_l_[i]) * gain_) >> 15);
            mix_r_[i] = static_cast<int32_t>((static_cast<int64_t>(mix_r_[i]) * gain_) >> 15);
            ++limited_frames_;
            if (static_cast<uint32_t>(gain_) < min_gain_) min_gain_ = static_cast<uint32_t>(gain_);

            // Восстановление: за кадр добирается 1/2^N недостающего.
            // Единица снизу обязательна: при недостаче меньше 2^N сдвиг
            // даёт ноль, усиление навсегда остаётся ниже единицы, каждый
            // кадр умножается и попадает в limited_frames.
            const int32_t lack = kUnityGain - gain_;
            const int32_t step = lack >> SOUNDSINTH_MIDI_LIMITER_RELEASE_SHIFT;
            gain_ += step > 0 ? step : 1;
        }
    }
}

// Из Q24.8 в целые единицы int16 с округлением к ближайшему - один раз на
// всю сумму. Сдвиг знакового вправо арифметический (GCC, MSVC): -1.5 -> -1,
// 1.5 -> 2, смещения нуля нет.
constexpr int32_t round_q24_8(int32_t x) {
    return (x + (1 << (kMixFracBits - 1))) >> kMixFracBits;
}

// Разрыв: скачок хотя бы одного канала больше kJumpThreshold. Редкая ветка.
SOUNDSINTH_NOINLINE void MixBus::count_jump(int32_t dl, int32_t dr) {
    const int32_t al = dl < 0 ? -dl : dl;
    const int32_t ar = dr < 0 ? -dr : dr;
    const int32_t d = al > ar ? al : ar;
    ++jumps_;
    if (static_cast<uint32_t>(d) > max_jump_) max_jump_ = static_cast<uint32_t>(d);
}

// Кадр выше порога мягкого насыщения хотя бы в одном канале. Редкая ветка.
SOUNDSINTH_NOINLINE void MixBus::soft_clip_frame(int32_t l, int32_t r, int32_t& out_l, int32_t& out_r) {
    out_l = soft_clip(l);
    out_r = soft_clip(r);
    if (out_l != l || out_r != r) ++soft_clipped_;
}

// Один проход по кадру: округление (если лимитера нет), насыщение, счёт
// полки и разрывов, запись кадра. Прошлый кадр и серия полки - в локальных,
// в члены - после цикла.
template <bool kRound, bool kSoft>
void MixBus::write_frames(int16_t* dst_interleaved, uint32_t n_frames) {
    const uint32_t knee = static_cast<uint32_t>(soft_knee_);
    int32_t prev_l = prev_l_;
    int32_t prev_r = prev_r_;
    uint32_t run = run_;
    for (uint32_t i = 0; i < n_frames; ++i) {
        int32_t l = mix_l_[i];
        int32_t r = mix_r_[i];
        if constexpr (kRound) {
            l = round_q24_8(l);
            r = round_q24_8(r);
        }
        int32_t out_l;
        int32_t out_r;
        bool over;
        if constexpr (kSoft) {
            // Мягкое насыщение - вместо полки (set_soft_clip). Полка считается
            // до него: сколько раз сумма уходила за шкалу.
            over = static_cast<uint32_t>(l + 32768) > 65535u || static_cast<uint32_t>(r + 32768) > 65535u;
            out_l = l;
            out_r = r;
            if (static_cast<uint32_t>(l) + knee > 2u * knee || static_cast<uint32_t>(r) + knee > 2u * knee) {
                soft_clip_frame(l, r, out_l, out_r);
            }
        } else {
            out_l = sat_s16(l);
            out_r = sat_s16(r);
            over = ((out_l ^ l) | (out_r ^ r)) != 0;
        }
        // Полка считается по кадру: важно, сколько подряд звук стоит на шкале
        // (одиночный отсчёт не слышен, полка в миллисекунды слышна).
        if (over) {
            ++clipped_;
            if (++run > longest_run_) longest_run_ = run;
        } else {
            run = 0;
        }
        const int32_t dl = out_l - prev_l;
        const int32_t dr = out_r - prev_r;
        constexpr uint32_t kJump = static_cast<uint32_t>(kJumpThreshold);
        if (static_cast<uint32_t>(dl) + kJump > 2u * kJump || static_cast<uint32_t>(dr) + kJump > 2u * kJump) {
            count_jump(dl, dr);
        }
        prev_l = out_l;
        prev_r = out_r;
        // Кадр одной записью слова.
        const uint32_t frame =
            static_cast<uint16_t>(out_l) | (static_cast<uint32_t>(static_cast<uint16_t>(out_r)) << 16);
        std::memcpy(dst_interleaved + i * 2, &frame, sizeof(frame));
    }
    prev_l_ = prev_l;
    prev_r_ = prev_r;
    run_ = run;
}

void MixBus::render(int16_t* dst_interleaved, uint32_t n_frames) {
    // Пауза, затухание уже кончилось: источники не зовём - позиция стоит.
    // Счётчик кадров шины тоже: по нему считается конец трека.
    if (paused_ && fade_pos_ >= kFadeFrames) {
        // Отметка - до тишины, а не после: наблюдателю нужно знать, что этот
        // проход источники уже не тронет, а не что он закончился.
        ++silent_renders_;
        std::memset(dst_interleaved, 0, n_frames * 2 * sizeof(int16_t));
        return;
    }
    // Конец трека: затухание начинается так, чтобы кончиться к кадру конца.
    if (end_frame_ != 0 && fade_pos_ == kFadeOff && rendered_frames_ + n_frames + kFadeFrames > end_frame_) {
        fade_pos_ = 0;
    }
    rendered_frames_ += n_frames;
    std::memset(mix_l_, 0, n_frames * sizeof(int32_t));
    std::memset(mix_r_, 0, n_frames * sizeof(int32_t));

    for (uint32_t i = 0; i < source_count_; ++i) {
        sources_[i]->render_add(sources_[i]->self, mix_l_, mix_r_, n_frames);
    }

    if (limiter_) {
        // Лимитер работает в целых единицах: округление - до него, отдельным
        // проходом.
        for (uint32_t i = 0; i < n_frames; ++i) {
            mix_l_[i] = round_q24_8(mix_l_[i]);
            mix_r_[i] = round_q24_8(mix_r_[i]);
        }
        apply_limiter(n_frames);
        if (soft_clip_) {
            write_frames<false, true>(dst_interleaved, n_frames);
        } else {
            write_frames<false, false>(dst_interleaved, n_frames);
        }
    } else if (soft_clip_) {
        write_frames<true, true>(dst_interleaved, n_frames);
    } else {
        write_frames<true, false>(dst_interleaved, n_frames);
    }
    if (fade_pos_ != kFadeOff) apply_fade(dst_interleaved, n_frames);
}

// Кривая x*x*(3-2x) от единицы к нулю за kFadeFrames кадров, как гашение
// хвоста голоса: без изломов на концах. Счёт затухания и разрывов уже
// сделан по кадрам до неё.
void MixBus::apply_fade(int16_t* dst_interleaved, uint32_t n_frames) {
    // t^2 (3K - 2t) при t = K = 256 - это 2^24, сдвиг на 9 даёт Q15. При
    // другом K кривая начинается ступенькой или заворачивает int16.
    static_assert(kFadeFrames == 256, "сдвиг кривой затухания посчитан для 256 кадров");
    for (uint32_t i = 0; i < n_frames; ++i) {
        const int32_t t = fade_pos_ < kFadeFrames ? static_cast<int32_t>(kFadeFrames - fade_pos_) : 0; // 256..0
        const int32_t g = (t * t * (3 * static_cast<int32_t>(kFadeFrames) - 2 * t)) >> 9;             // Q15
        dst_interleaved[i * 2 + 0] = static_cast<int16_t>((dst_interleaved[i * 2 + 0] * g) >> 15);
        dst_interleaved[i * 2 + 1] = static_cast<int16_t>((dst_interleaved[i * 2 + 1] * g) >> 15);
        if (fade_dir_ > 0) {
            if (fade_pos_ < kFadeFrames) ++fade_pos_;
        } else if (fade_pos_ > 0) {
            --fade_pos_;
        }
    }
    // Нарастание дошло до полной громкости - затухания больше нет. Снимается
    // после буфера, а не в цикле: kFadeOff внутри дал бы нулевую кривую, и
    // остаток буфера ушёл бы в тишину.
    if (fade_dir_ < 0 && fade_pos_ == 0) {
        fade_pos_ = kFadeOff;
        fade_dir_ = +1;
    }
}

} // namespace soundsinth::mixbus
