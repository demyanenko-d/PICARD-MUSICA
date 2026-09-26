#include "core/engine/reverb.h"

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "core/config.h"
#include "core/audio/sound_source.h"
#include "core/audio/saturate.h"

namespace soundsinth::engine {
namespace {

// Константы Freeverb в Q15 подобраны замером возврата против эталонного
// рендера на паузе между фразами, где сухой сигнал у нас и у эталона
// затухает близко (87 и 79 дБ/с) и разница - только возврат ревербератора.
//
// Обратная связь гребёнки задаёт время затухания. 0.93 при этих длинах -
// RT60 около двух секунд, как у эталона (возврат в паузе спадает на
// 27 дБ/с у эталона, 28 у нас).
//
// Демпфера в обратной связи нет: возврат и без него темнее эталонного
// (2.4..11 кГц на 8..12 дБ ниже).
constexpr int32_t kFeedback = 30474; // 0.93
constexpr int32_t kAllpassGain = 16384; // 0.5
constexpr int32_t kQ15One = 32768;

// Вход /16 перед гребёнками: восемь гребёнок с обратной связью 0.93 дают
// усиление около 114, иначе линии int16 переполняются. Против /32 на самых
// громких файлах разница -46..-49 дБ - шум округления.
constexpr int32_t kInputShift = 4; // деление на 16

// Обратно на выходе. Делить надо только на входе (иначе переполнятся
// линии int16), здесь - сколько нужно по уровню; общий уровень возврата -
// SOUNDSINTH_REVERB_WET_Q15, он подогнан по эталону.
constexpr int32_t kOutputShift = 1;

// Раскладка линий в Reverb::lines: гребёнки подряд, за ними
// всепропускающие. Считается при сборке, поэтому в цикле смещения и длины -
// непосредственные значения, а не чтения из объекта.
struct Layout {
    uint32_t comb_off[kReverbCombCount];
    uint32_t comb_len[kReverbCombCount];
    uint32_t allpass_off[kReverbAllpassCount];
    uint32_t allpass_len[kReverbAllpassCount];
};

constexpr Layout make_layout() {
    Layout l{};
    uint32_t off = 0;
    for (uint32_t i = 0; i < kReverbCombCount; ++i) {
        l.comb_len[i] = kReverbCombLen44k[i] / kReverbRateDiv;
        l.comb_off[i] = off;
        off += l.comb_len[i];
    }
    for (uint32_t i = 0; i < kReverbAllpassCount; ++i) {
        l.allpass_len[i] = kReverbAllpassLen44k[i] / kReverbRateDiv;
        l.allpass_off[i] = off;
        off += l.allpass_len[i];
    }
    return l;
}

constexpr Layout kLayout = make_layout();
static_assert(kLayout.allpass_off[kReverbAllpassCount - 1] + kLayout.allpass_len[kReverbAllpassCount - 1] ==
                  kReverbSampleCount,
              "раскладка линий разошлась с kReverbSampleCount");

// Q15-умножение с округлением к нулю, а не сдвигом.
//
// У знакового >> округление к минус бесконечности, и у отрицательной
// величины есть неподвижная точка: -1 * 0.93 = -0.93, а -30474 >> 15 -
// опять -1. Значение -1 в линии задержки не затухает никогда: после конца
// музыки остаётся постоянная составляющая, на .mid - до -73 LSB (-53 дБ).
//
// Деление на степень двойки компилятор разворачивает в сдвиг с поправкой
// знака - две команды на умножение, меньше процента цены ревербератора.
int32_t mul_q15(int32_t a, int32_t b) {
    return (a * b) / kQ15One;
}

// Всепропускающее звено a: sig через его линию.
SOUNDSINTH_ALWAYS_INLINE int32_t allpass_step(Reverb& rv, uint32_t a, int32_t sig) {
    int16_t* line = rv.lines + kLayout.allpass_off[a];
    uint32_t pos = rv.allpass_pos[a];
    const int32_t buf = line[pos];
    line[pos] = static_cast<int16_t>(sat_s16(sig + mul_q15(buf, kAllpassGain)));
    if (++pos >= kLayout.allpass_len[a]) pos = 0;
    rv.allpass_pos[a] = static_cast<uint16_t>(pos);
    return buf - sig;
}

} // namespace

// Горячий путь, на каждый блок рендера.
void SOUNDSINTH_HOT_PATH(reverb_process)(Reverb& rv, const int32_t* bus, int32_t* mix_l, int32_t* mix_r,
                                         uint32_t n_frames) {
    // Децимация входа: kReverbRateDiv отсчётов выхода дают один внутренний.
    // Сумма, а не выборка: иначе верх завернулся бы вниз и хвост звенел
    // бы на посторонних частотах. Полная группа делится на константу (сдвиг с
    // поправкой знака), неполная бывает только в конце блока нечётной длины.
    constexpr int32_t kGroupDiv = static_cast<int32_t>(kReverbRateDiv) << kInputShift;
    for (uint32_t i = 0; i < n_frames; i += kReverbRateDiv) {
        const uint32_t taken = (n_frames - i < kReverbRateDiv) ? n_frames - i : kReverbRateDiv;
        int32_t in = 0;
        for (uint32_t k = 0; k < taken; ++k) {
            in += bus[i + k];
        }
        in = (taken == kReverbRateDiv) ? in / kGroupDiv : in / (static_cast<int32_t>(taken) << kInputShift);

        // Гребёнки параллельно, суммой. Позиция звена - в локальной: запись в
        // линию int16_t по правилам языка может задеть uint16_t позиции, и без
        // локальной компилятор перечитывает её после каждой записи.
        int32_t acc = 0;
        for (uint32_t c = 0; c < kReverbCombCount; ++c) {
            int16_t* line = rv.lines + kLayout.comb_off[c];
            uint32_t pos = rv.comb_pos[c];
            const int32_t out = line[pos];
            line[pos] = static_cast<int16_t>(sat_s16(in + mul_q15(out, kFeedback)));
            if (++pos >= kLayout.comb_len[c]) pos = 0;
            rv.comb_pos[c] = static_cast<uint16_t>(pos);
            acc += out;
        }

        // Всепропускающие последовательно, разными парами: левый - звенья 0 и
        // 2, правый - 1 и 3. Полноценный стерео потребовал бы второго
        // комплекта гребёнок и вдвое больше памяти, а расхождение каналов
        // даёт и это.
        static_assert(kReverbAllpassCount == 4, "звенья 0, 2 - левый канал, 1, 3 - правый");
        const int32_t l0 = allpass_step(rv, 0, acc);
        const int32_t r0 = allpass_step(rv, 1, acc);
        const int32_t l = allpass_step(rv, 2, l0);
        const int32_t r = allpass_step(rv, 3, r0);
        const int32_t wl = mul_q15(l / (1 << kOutputShift), SOUNDSINTH_REVERB_WET_Q15);
        const int32_t wr = mul_q15(r / (1 << kOutputShift), SOUNDSINTH_REVERB_WET_Q15);

        // Обратно на выходную частоту линейной интерполяцией: ступенька даёт
        // зеркало вокруг 22050 (18..22 кГц на 45 дБ выше эталона), у среднего
        // двух точек ноль ровно на 22050.
        //
        // В шину - в её масштабе Q24.8 (kMixFracBits): дробь интерполяции
        // между внутренними отсчётами остаётся в накопителе, а не
        // отбрасывается. Возврат не больше +-76 тыс., после сдвига около
        // 19.5 млн.
        constexpr int32_t kOne = 1 << mixbus::kMixFracBits;
        const int32_t pl = rv.prev_l * kOne;
        const int32_t dl = (wl - rv.prev_l) * kOne;
        const int32_t pr = rv.prev_r * kOne;
        const int32_t dr = (wr - rv.prev_r) * kOne;
        for (uint32_t k = 0; k < taken; ++k) {
            const int32_t w = static_cast<int32_t>(k + 1);
            mix_l[i + k] += pl + dl * w / static_cast<int32_t>(kReverbRateDiv);
            mix_r[i + k] += pr + dr * w / static_cast<int32_t>(kReverbRateDiv);
        }
        rv.prev_l = wl;
        rv.prev_r = wr;
    }
}

} // namespace soundsinth::engine
