// SPDX-License-Identifier: MIT
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
// Обратная связь гребёнки задаёт время затухания. 0.93 даёт RT60 около
// двух секунд, как у эталона (возврат в паузе спадает на 27 дБ/с у эталона,
// 28 у нас); 0.95 - хвост длиннее эталонного, спад 14 дБ/с против 19.
//
// Демпфера в обратной связи нет: возврат и без него темнее эталонного
// (2.4..11 кГц на 8..12 дБ ниже).
constexpr int32_t kAllpassGain = 16384; // 0.5
constexpr int32_t kQ15One      = 32768;

// Вход /16 перед гребёнками: восемь гребёнок с обратной связью 0.95 дают
// усиление около 160, иначе линии int16 переполняются. Против /32 на самых
// громких файлах разница -46..-49 дБ - шум округления.
constexpr int32_t kInputShift = 4; // деление на 16

// Обратно на выходе. Делить надо только на входе (иначе переполнятся
// линии int16), здесь - сколько нужно по уровню; общий уровень возврата -
// SOUNDSINTH_REVERB_WET_Q15.
constexpr int32_t kOutputShift = 0;

// Вход всепропускающих звеньев - сумма восьми гребёнок, до 8 x 32767: в
// линии int16 он насыщается: на Timbres of Heaven у 0.85% записей при
// обратной связи 0.93 и 1.5% при 0.95. Делится на 4 перед звеньями и
// умножается обратно на выходе; остаётся 0.001%.
constexpr int32_t kAllpassInputShift = 2;

// Возврат звена в масштабе выхода с уровнем wet_q15 (Reverb::wet_q15). В 64
// битах: возврат звена до 98 тыс. на уровень до 60325 не помещается в int32.
// Деление, а не сдвиг: округление к нулю, как у mul_q15.
SOUNDSINTH_ALWAYS_INLINE int32_t wet_out(int32_t v, int32_t wet_q15) {
    constexpr int64_t kDiv = int64_t{kQ15One} << kOutputShift >> kAllpassInputShift;
    return static_cast<int32_t>(static_cast<int64_t>(v) * wet_q15 / kDiv);
}

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
        l.comb_len[i]  = reverb_line_len(kReverbCombLen44k[i]);
        l.comb_off[i]  = off;
        off           += l.comb_len[i];
    }
    for (uint32_t i = 0; i < kReverbAllpassCount; ++i) {
        l.allpass_len[i]  = reverb_line_len(kReverbAllpassLen44k[i]);
        l.allpass_off[i]  = off;
        off              += l.allpass_len[i];
    }
    return l;
}

constexpr Layout kLayout = make_layout();
static_assert(kLayout.allpass_off[kReverbAllpassCount - 1] + kLayout.allpass_len[kReverbAllpassCount - 1] == kReverbSampleCount,
              "the line layout diverged from kReverbSampleCount");

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
    int16_t* line     = rv.lines + kLayout.allpass_off[a];
    uint32_t pos      = rv.allpass_pos[a];
    const int32_t buf = line[pos];
    line[pos]         = static_cast<int16_t>(sat_s16(sig + mul_q15(buf, kAllpassGain)));
    if (++pos >= kLayout.allpass_len[a]) pos = 0;
    rv.allpass_pos[a] = static_cast<uint16_t>(pos);
    return buf - sig;
}

} // namespace

// Горячий путь, на каждый блок рендера.
void SOUNDSINTH_HOT_PATH(reverb_process)(Reverb& rv, const int32_t* bus, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames) {
    // Децимация входа: kReverbRateDiv отсчётов выхода дают один внутренний.
    // Сумма, а не выборка: иначе верх завернулся бы вниз и хвост звенел
    // бы на посторонних частотах. Полная группа делится на константу (сдвиг с
    // поправкой знака), неполная бывает только в конце блока нечётной длины.
    constexpr int32_t kGroupDiv = static_cast<int32_t>(kReverbRateDiv) << kInputShift;
    const int32_t feedback      = rv.feedback_q15;
    const int32_t wet           = rv.wet_q15;
    for (uint32_t i = 0; i < n_frames; i += kReverbRateDiv) {
        const uint32_t taken = (n_frames - i < kReverbRateDiv) ? n_frames - i : kReverbRateDiv;
        int32_t in           = 0;
        for (uint32_t k = 0; k < taken; ++k) {
            in += bus[i + k];
        }
        in = (taken == kReverbRateDiv) ? in / kGroupDiv : in / (static_cast<int32_t>(taken) << kInputShift);

        // Гребёнки параллельно, суммой. Позиция звена - в локальной: запись в
        // линию int16_t по правилам языка может задеть uint16_t позиции, и без
        // локальной компилятор перечитывает её после каждой записи.
        int32_t acc = 0;
        for (uint32_t c = 0; c < kReverbCombCount; ++c) {
            int16_t* line     = rv.lines + kLayout.comb_off[c];
            uint32_t pos      = rv.comb_pos[c];
            const int32_t out = line[pos];
            line[pos]         = static_cast<int16_t>(sat_s16(in + mul_q15(out, feedback)));
            if (++pos >= kLayout.comb_len[c]) pos = 0;
            rv.comb_pos[c]  = static_cast<uint16_t>(pos);
            acc            += out;
        }

        // Всепропускающие последовательно, разными парами: левый - звенья 0 и
        // 2, правый - 1 и 3. Полноценный стерео потребовал бы второго
        // комплекта гребёнок и вдвое больше памяти, а расхождение каналов
        // даёт и это.
        static_assert(kReverbAllpassCount == 4, "stages 0 and 2 are the left channel, 1 and 3 the right");
        const int32_t ap_in = acc / (1 << kAllpassInputShift);
        const int32_t l0    = allpass_step(rv, 0, ap_in);
        const int32_t r0    = allpass_step(rv, 1, ap_in);
        const int32_t l     = allpass_step(rv, 2, l0);
        const int32_t r     = allpass_step(rv, 3, r0);
        const int32_t wl    = wet_out(l, wet);
        const int32_t wr    = wet_out(r, wet);

        // Обратно на выходную частоту линейной интерполяцией: ступенька даёт
        // зеркало вокруг 22050 (18..22 кГц на 45 дБ выше эталона), у среднего
        // двух точек ноль ровно на 22050.
        //
        // В шину - в её масштабе Q24.8 (kMixFracBits): дробь интерполяции
        // между внутренними отсчётами остаётся в накопителе, а не
        // отбрасывается. Возврат не больше +-722 тыс., после сдвига около
        // 185 млн.
        constexpr int32_t kOne = 1 << mixbus::kMixFracBits;
        const int32_t pl       = rv.prev_l * kOne;
        const int32_t dl       = (wl - rv.prev_l) * kOne;
        const int32_t pr       = rv.prev_r * kOne;
        const int32_t dr       = (wr - rv.prev_r) * kOne;
        for (uint32_t k = 0; k < taken; ++k) {
            const int32_t w  = static_cast<int32_t>(k + 1);
            mix_l[i + k]    += pl + dl * w / static_cast<int32_t>(kReverbRateDiv);
            mix_r[i + k]    += pr + dr * w / static_cast<int32_t>(kReverbRateDiv);
        }
        rv.prev_l = wl;
        rv.prev_r = wr;
    }
}

} // namespace soundsinth::engine
