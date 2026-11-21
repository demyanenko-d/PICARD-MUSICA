// SPDX-License-Identifier: MIT
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
    soft_knee_            = knee;
    const uint32_t span_u = static_cast<uint32_t>(32767 - knee);
    soft_span_sq_         = span_u * span_u; // до 32766^2, в uint32 влезает
}

inline int32_t MixBus::soft_clip(int32_t x) const {
    const int32_t a = x < 0 ? -x : x; // |x| до 8.4 млн (запас шины), переполнения нет
    if (a <= soft_knee_) return x;
    const uint32_t over = static_cast<uint32_t>(a - soft_knee_);
    // Частное усекается вниз, выход - вверх меньше чем на единицу; не выше
    // 32767: при частном 0 ровно порог + запас. С ростом входа частное не
    // растёт - монотонно.
    const uint32_t span = static_cast<uint32_t>(32767 - soft_knee_);
    const int32_t y     = 32767 - static_cast<int32_t>(soft_span_sq_ / (span + over));
    return x < 0 ? -y : y;
}

bool MixBus::add_source(SoundSource* source) {
    if (source_count_ >= kMaxSources) {
        return false;
    }
    sources_[source_count_++] = source;
    return true;
}

bool MixBus::add_machine_source(SoundSource* source) {
    if (machine_count_ >= kMaxMachineSources) return false;
    machine_[machine_count_++] = source;
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
        const int32_t al   = l < 0 ? ~l : l;
        const int32_t ar   = r < 0 ? ~r : r;
        const int32_t peak = al > ar ? al : ar;

        // Компрессор: поджимает всё громче порога на долю AMOUNT. Полку
        // не гарантирует, за это отвечает лимитер ниже.
        if (peak > comp_threshold_) {
            const int32_t q      = static_cast<int32_t>((static_cast<uint32_t>(comp_threshold_) << 15) / static_cast<uint32_t>(peak));
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
                gain_ = static_cast<int32_t>((static_cast<uint32_t>(SOUNDSINTH_MIDI_LIMITER_CEILING) << 15) / static_cast<uint32_t>(peak));
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
            const int32_t lack  = kUnityGain - gain_;
            const int32_t step  = lack >> SOUNDSINTH_MIDI_LIMITER_RELEASE_SHIFT;
            gain_              += step > 0 ? step : 1;
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
    const int32_t d  = al > ar ? al : ar;
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
    int32_t prev_l      = prev_l_;
    int32_t prev_r      = prev_r_;
    uint32_t run        = run_;
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
            over  = static_cast<uint32_t>(l + 32768) > 65535u || static_cast<uint32_t>(r + 32768) > 65535u;
            out_l = l;
            out_r = r;
            if (static_cast<uint32_t>(l) + knee > 2u * knee || static_cast<uint32_t>(r) + knee > 2u * knee) {
                soft_clip_frame(l, r, out_l, out_r);
            }
        } else {
            out_l = sat_s16(l);
            out_r = sat_s16(r);
            over  = ((out_l ^ l) | (out_r ^ r)) != 0;
        }
        // Полка считается по кадру: важно, сколько подряд звук стоит на шкале
        // (одиночный отсчёт не слышен, полка в миллисекунды слышна).
        if (over) {
            ++clipped_;
            if (++run > longest_run_) longest_run_ = run;
        } else {
            run = 0;
        }
        const int32_t dl         = out_l - prev_l;
        const int32_t dr         = out_r - prev_r;
        constexpr uint32_t kJump = static_cast<uint32_t>(kJumpThreshold);
        if (static_cast<uint32_t>(dl) + kJump > 2u * kJump || static_cast<uint32_t>(dr) + kJump > 2u * kJump) {
            count_jump(dl, dr);
        }
        prev_l = out_l;
        prev_r = out_r;
        // Кадр одной записью слова.
        const uint32_t frame = static_cast<uint16_t>(out_l) | (static_cast<uint32_t>(static_cast<uint16_t>(out_r)) << 16);
        std::memcpy(dst_interleaved + i * 2, &frame, sizeof(frame));
    }
    prev_l_ = prev_l;
    prev_r_ = prev_r;
    run_    = run;
}

void MixBus::set_highpass_hz(uint32_t hz, uint32_t sample_rate_hz) {
    if (hz == 0 || sample_rate_hz == 0) {
        hp_a_q30_ = 0;
        return;
    }
    constexpr double kTwoPi = 6.283185307179586;
    const double a          = 1.0 / (1.0 + kTwoPi * static_cast<double>(hz) / static_cast<double>(sample_rate_hz));
    hp_a_q30_               = static_cast<int32_t>(a * static_cast<double>(1 << 30) + 0.5);
}

// Q24.8 на входе и выходе, коэффициент Q30, произведение в 64 битах.
// Остаток округления идёт в следующий шаг: без него спад замирает, когда шаг
// меньше младшего разряда, и на тишине стоит сдвиг до 2.7 единицы int16 при 5 Гц.
void MixBus::apply_highpass(Highpass& hp, uint32_t n_frames) {
    const int64_t a = hp_a_q30_;
    int32_t xl      = hp.x_l;
    int32_t xr      = hp.x_r;
    int32_t yl      = hp.y_l;
    int32_t yr      = hp.y_r;
    int64_t el      = hp.e_l;
    int64_t er      = hp.e_r;
    for (uint32_t i = 0; i < n_frames; ++i) {
        const int32_t l     = mix_l_[i];
        const int32_t r     = mix_r_[i];
        const int64_t acc_l = a * (static_cast<int64_t>(yl) + l - xl) + el;
        const int64_t acc_r = a * (static_cast<int64_t>(yr) + r - xr) + er;
        yl                  = static_cast<int32_t>(acc_l >> 30);
        yr                  = static_cast<int32_t>(acc_r >> 30);
        el                  = acc_l - (static_cast<int64_t>(yl) << 30);
        er                  = acc_r - (static_cast<int64_t>(yr) << 30);
        xl                  = l;
        xr                  = r;
        mix_l_[i]           = yl;
        mix_r_[i]           = yr;
    }
    hp.x_l = xl;
    hp.x_r = xr;
    hp.y_l = yl;
    hp.y_r = yr;
    hp.e_l = static_cast<int32_t>(el);
    hp.e_r = static_cast<int32_t>(er);
}

// Усиление блока целиком: авторегулировка и длинное затухание перед концом
// трека. Одно на блок - шаг между блоками меньше 0.1 дБ.
void MixBus::apply_level(uint32_t n_frames) {
    constexpr int32_t kUnity = 1 << 12;
    // 2^(i/16) в Q12.
    static constexpr uint16_t kExp[17] = {4096, 4277, 4467, 4664, 4871, 5087, 5312, 5547, 5793, 6049, 6317, 6597, 6889, 7194, 7512, 7845, 8192};
    // Тише -50 дБ от шкалы уровень не меряется: на паузе усиление стоит.
    constexpr uint32_t kGateMs = 10737u;
    // Сглаживание среднего квадрата: первые 3 с трека - около 0.2 с, дальше
    // около 3 с (доля 1/2^сдвиг на блок).
    constexpr uint32_t kFastFrames = 3u * 44100u, kFastShift = 5, kSlowShift = 9;
    constexpr int32_t kMinLog = -2 * 65536, kMaxLog = 3 * 65536; // -12 и +18 дБ
    constexpr int32_t kUpPerFrame = 1, kDownPerFrame = 6;        // 4 и 24 дБ/с

    int64_t gain = kUnity;
    if (auto_volume_) {
        // Мера уровня: к сигналу прибавлено 37/64 того, что выше ФНЧ первого
        // порядка на 1.5 кГц (доля 3/16 на отсчёт) - верх тяжелее на 4 дБ.
        uint64_t sum = 0;
        int32_t lpl = level_lp_l_, lpr = level_lp_r_;
        for (uint32_t i = 0; i < n_frames; ++i) {
            const int32_t l = mix_l_[i] >> kMixFracBits, r = mix_r_[i] >> kMixFracBits;
            lpl              += ((l - lpl) * 3) >> 4;
            lpr              += ((r - lpr) * 3) >> 4;
            const int64_t wl = l + (((l - lpl) * 37) >> 6), wr = r + (((r - lpr) * 37) >> 6);
            sum += static_cast<uint64_t>(wl * wl + wr * wr);
        }
        level_lp_l_       = lpl;
        level_lp_r_       = lpr;
        const uint64_t ms = sum / (2u * n_frames);
        if (ms >= kGateMs) {
            const bool fast  = rendered_frames_ < kFastFrames;
            const int64_t in = static_cast<int64_t>(ms > 0xffffffffu ? 0xffffffffu : ms);
            // Первый звучащий блок задаёт уровень целиком.
            const int64_t level = level_ms_ == 0 ? in : static_cast<int64_t>(level_ms_) + ((in - level_ms_) >> (fast ? kFastShift : kSlowShift));
            level_ms_           = static_cast<uint32_t>(level < 1 ? 1 : level);
            // Усиление - половина разности логарифмов квадратов.
            int32_t want     = (log2_fixed(kAutoTargetMs, 8) - log2_fixed(level_ms_, 8)) * 128;
            want             = want < kMinLog ? kMinLog : want > kMaxLog ? kMaxLog : want;
            const int32_t up = kUpPerFrame * static_cast<int32_t>(n_frames), down = kDownPerFrame * static_cast<int32_t>(n_frames);
            if (fast) {
                auto_log_ = want;
            } else {
                auto_log_ += want > auto_log_ ? (want - auto_log_ < up ? want - auto_log_ : up) : -(auto_log_ - want < down ? auto_log_ - want : down);
            }
        }
        // 2^(октавы): целая часть сдвигом, дробная - по таблице с шагом 1/16.
        const int32_t oct = auto_log_ >> 16, frac = auto_log_ & 0xffff;
        const int32_t i = frac >> 12, f = frac & 0xfff;
        const int32_t m = kExp[i] + (((kExp[i + 1] - kExp[i]) * f) >> 12);
        gain            = oct >= 0 ? static_cast<int64_t>(m) << oct : m >> -oct;
    }
    if (end_fade_ != 0 && end_frame_ != 0) {
        // rendered_frames_ уже включает этот блок.
        const uint32_t at = rendered_frames_ - n_frames, left = end_frame_ > at ? end_frame_ - at : 0;
        if (left < end_fade_) {
            const int64_t f = (static_cast<int64_t>(left) << 15) / end_fade_;
            gain            = (gain * ((f * f) >> 15)) >> 15;
        }
    }
    gain = (gain * track_gain_) >> 12;
    if (gain == kUnity) return;
    for (uint32_t i = 0; i < n_frames; ++i) {
        mix_l_[i] = static_cast<int32_t>((mix_l_[i] * gain) >> 12);
        mix_r_[i] = static_cast<int32_t>((mix_r_[i] * gain) >> 12);
    }
}

void MixBus::render(int16_t* dst_interleaved, uint32_t n_frames) {
    // Пауза или конец трека, затухание уже кончилось, или выход погашен
    // целиком: источники трека не зовём - позиция стоит. Счётчик кадров шины
    // тоже: по нему считается конец трека.
    const bool mute_done = muted_ && mute_pos_ == kFadeFrames;
    if (mute_done || ((paused_ || finished_) && fade_pos_ >= kFadeFrames)) {
        // Отметка - до тишины, а не после: наблюдателю нужно знать, что этот
        // проход источники уже не тронет, а не что он закончился.
        ++silent_renders_;
        // Под глушением затухание трека не исполняется - его просьба
        // (пауза, конец, снос) считается исполненной: иначе после снятия
        // глушения трек на миг зазвучит.
        if (mute_done && fade_dir_ > 0 && fade_pos_ != kFadeOff) fade_pos_ = kFadeFrames;
        std::memset(dst_interleaved, 0, n_frames * 2 * sizeof(int16_t));
        if (!mute_done && machine_count_ != 0) {
            add_machine(dst_interleaved, n_frames);
            if (mute_pos_ != kFadeOff) apply_fade(dst_interleaved, n_frames, mute_pos_, mute_dir_);
        }
        // Измерителю - тишина: иначе на экране стоит спектр последнего звука.
        if (meter_ != nullptr && meter_->enabled()) meter_->feed(dst_interleaved, n_frames);
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
    if (hp_a_q30_ != 0) apply_highpass(hp_, n_frames);
    if (auto_volume_ || end_fade_ != 0 || track_gain_ != 4096u) apply_level(n_frames);

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
    if (fade_pos_ != kFadeOff) apply_fade(dst_interleaved, n_frames, fade_pos_, fade_dir_);
    if (machine_count_ != 0) add_machine(dst_interleaved, n_frames);
    if (mute_pos_ != kFadeOff) apply_fade(dst_interleaved, n_frames, mute_pos_, mute_dir_);
    if (meter_ != nullptr && meter_->enabled()) meter_->feed(dst_interleaved, n_frames);
}

// Источники машины - поверх готового выхода трека, в тех же буферах сложения:
// трек их уже отдал. Своё состояние ФВЧ - живой AY однополярный. Ни звука, ни
// хвоста фильтра - проход сложения пропускается.
void MixBus::add_machine(int16_t* dst_interleaved, uint32_t n_frames) {
    std::memset(mix_l_, 0, n_frames * sizeof(int32_t));
    std::memset(mix_r_, 0, n_frames * sizeof(int32_t));
    for (uint32_t i = 0; i < machine_count_; ++i) {
        machine_[i]->render_add(machine_[i]->self, mix_l_, mix_r_, n_frames);
    }
    int32_t any = machine_hp_.x_l | machine_hp_.x_r | machine_hp_.y_l | machine_hp_.y_r;
    for (uint32_t i = 0; i < n_frames; ++i) {
        any |= mix_l_[i] | mix_r_[i];
    }
    if (any == 0) return;
    if (hp_a_q30_ != 0) apply_highpass(machine_hp_, n_frames);
    for (uint32_t i = 0; i < n_frames; ++i) {
        dst_interleaved[i * 2 + 0] = static_cast<int16_t>(sat_s16(dst_interleaved[i * 2 + 0] + round_q24_8(mix_l_[i])));
        dst_interleaved[i * 2 + 1] = static_cast<int16_t>(sat_s16(dst_interleaved[i * 2 + 1] + round_q24_8(mix_r_[i])));
    }
}

// Кривая x*x*(3-2x) от единицы к нулю за kFadeFrames кадров, как гашение
// хвоста голоса: без изломов на концах. Счёт затухания и разрывов уже
// сделан по кадрам до неё.
void MixBus::apply_fade(int16_t* dst_interleaved, uint32_t n_frames, uint32_t& pos, int8_t& dir) {
    // t^2 (3K - 2t) при t = K равно K^3, и чтобы это дало ровно Q15, делить
    // надо на K^3/32768. Деление, а не сдвиг: K больше не степень двойки.
    // Считается вне цикла отсчётов и только на кадрах затухания.
    constexpr int32_t kFadeDiv = static_cast<int32_t>(static_cast<uint64_t>(kFadeFrames) * kFadeFrames * kFadeFrames / 32768u);
    static_assert(static_cast<uint64_t>(kFadeDiv) * 32768u == static_cast<uint64_t>(kFadeFrames) * kFadeFrames * kFadeFrames,
                  "the cube of the fade length must divide by 2^15, otherwise the curve never reaches one");
    uint32_t p     = pos;
    const int8_t d = dir;
    for (uint32_t i = 0; i < n_frames; ++i) {
        const int32_t t            = p < kFadeFrames ? static_cast<int32_t>(kFadeFrames - p) : 0;          // K..0
        const int32_t g            = (t * t * (3 * static_cast<int32_t>(kFadeFrames) - 2 * t)) / kFadeDiv; // Q15
        dst_interleaved[i * 2 + 0] = static_cast<int16_t>((dst_interleaved[i * 2 + 0] * g) >> 15);
        dst_interleaved[i * 2 + 1] = static_cast<int16_t>((dst_interleaved[i * 2 + 1] * g) >> 15);
        if (d > 0) {
            if (p < kFadeFrames) ++p;
        } else if (p > 0) {
            --p;
        }
    }
    // Нарастание дошло до полной громкости - затухания больше нет. Снимается
    // после буфера, а не в цикле: kFadeOff внутри дал бы нулевую кривую, и
    // остаток буфера ушёл бы в тишину.
    if (d < 0 && p == 0) {
        p   = kFadeOff;
        dir = +1;
    }
    pos = p;
}

} // namespace soundsinth::mixbus
