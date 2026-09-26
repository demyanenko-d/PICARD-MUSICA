#include "core/engine/resonant_filter.h"

#include <cmath>

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "core/engine/engine_defs.h"
#include "core/audio/saturate.h"

namespace soundsinth::engine {

namespace {

constexpr int32_t kPrecision = 24; // Q24
constexpr int32_t kPreamp = 256; // усиление входа, чтобы состояние не теряло точность
constexpr int32_t kQ24Limit = 2147483000; // предел коэффициента Q24, с запасом до INT32_MAX
constexpr float kTwoPi = 6.283185307f;
constexpr float kMinCutoffHz = 120.0f;
constexpr float kMaxCutoffHz = 20000.0f;
// Срез с огибающей на шкале 0..254 (co * (env + 256) / 256): 254 и выше без
// резонанса - фильтра нет.
constexpr int32_t kCutoffOpen = 254;
// Верх шкалы SF2 (13500 центов): выше него без резонанса фильтр не считается,
// как у эталонного синтезатора. У .mid шкала 16 делений на октаву, и срезы
// банка сплошь выше этой границы: на SWARS.MID с SGM фильтр работал на всех
// голосах при срезе 19-24 кГц, то есть выше Найквиста, за 156 нс на голос.
constexpr float kOpenCutoffHz = 19912.0f;

// Срез в герцы: 110 * 2^(0.25 + raw / (U * 512)), raw = cutoff * (env + 256).
// Одна запись на таблицы нейтрали и прямой расчёт: они совпадают побитово.
float cutoff_to_hz(float raw, float units_per_octave) {
    return 110.0f * std::pow(2.0f, 0.25f + raw / (units_per_octave * 512.0f));
}

// Состояние ограничивается вдвое шире входного диапазона: на резонансе
// фильтр может разогнаться, и без ограничения обратная связь уходит
// вразнос.
constexpr int32_t kStateClip = 32768 * 2 * kPreamp;

// Таблицы для filter_compute: pow() на Cortex-M33 программный, сотни
// тактов, а нужен на каждый голос с фильтром каждый тик. Заполняются тем же
// pow и теми же выражениями, что прямой расчёт, а не готовыми константами:
// значения совпадают побитово. Резонанс - при старте, до main(); срез при
// нейтральной огибающей - один на шкалу песни, filter_prepare.
constexpr uint32_t kFilterParamSteps = 128; // срез и резонанс 0..127

struct FilterTables {
    float dmpfac[kFilterParamSteps]; // по резонансу
    float freq_neutral[kFilterParamSteps]; // по срезу при нейтральной огибающей, шкала units
    uint8_t units = 0; // 0 - таблица среза не заполнена
    FilterTables() {
        for (uint32_t i = 0; i < kFilterParamSteps; ++i) {
            dmpfac[i] = std::pow(10.0f, -static_cast<float>(i) * ((24.0f / 128.0f) / 20.0f));
        }
    }
};
FilterTables g_filter_tables;

int32_t to_q24(float v) {
    const float scaled = v * static_cast<float>(1 << kPrecision);
    if (scaled > static_cast<float>(kQ24Limit)) return kQ24Limit;
    if (scaled < -static_cast<float>(kQ24Limit)) return -kQ24Limit;
    return static_cast<int32_t>(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

} // namespace

void filter_prepare(uint8_t units_per_octave) {
    if (units_per_octave == 0 || g_filter_tables.units == units_per_octave) return;
    g_filter_tables.units = 0;
    for (uint32_t i = 0; i < kFilterParamSteps; ++i) {
        // Нейтраль - 256, а не 0: env_modifier = raw_env * 8 - 256, и
        // "огибающей нет" даёт +256. Поэтому co * (256 + 256).
        const float raw = static_cast<float>(i) * 512.0f;
        g_filter_tables.freq_neutral[i] = cutoff_to_hz(raw, static_cast<float>(units_per_octave));
    }
    g_filter_tables.units = units_per_octave;
}

FilterCoeffs filter_compute(uint8_t cutoff, uint8_t resonance, int32_t env_modifier, uint8_t units_per_octave,
                            bool sf2_response) {
    FilterCoeffs out;

    int32_t co = cutoff;
    if (co > 127) co = 127;
    int32_t res = resonance & 0x7fu;
    if (env_modifier < -256) env_modifier = -256;
    if (env_modifier > 256) env_modifier = 256;

    // Фильтр выключен, когда срез открыт и резонанса нет. Шкала здесь другая,
    // чем у перевода в герцы: делится на 256, диапазон 0..254.
    const int32_t computed_cutoff = co * (env_modifier + 256) / 256;
    if (res == 0 && computed_cutoff >= kCutoffOpen) {
        return out; // active == false
    }

    // Резонанс в децибелах: 0..127 -> 0..-24 дБ демпфирования. По таблице
    // (FilterTables).
    const float dmpfac = g_filter_tables.dmpfac[res];

    // Срез в герцы. Обычный диапазон IT (делитель 24*512), потолок около
    // 5 кГц - так в формате, это не ошибка масштаба.
    const float raw = static_cast<float>(co) * static_cast<float>(env_modifier + 256);
    // Огибающей фильтра у большинства инструментов нет, env_modifier
    // держит нейтраль 256, и raw зависит только от co (0..127) - таблица
    // шкалы песни покрывает этот случай точно. С огибающей или с другой
    // шкалой - pow.
    float freq;
    if (env_modifier == 256 && units_per_octave == g_filter_tables.units) {
        freq = g_filter_tables.freq_neutral[co];
    } else {
        freq = cutoff_to_hz(raw, static_cast<float>(units_per_octave));
    }
    if (res == 0 && freq >= kOpenCutoffHz) {
        return out; // active == false: выше верха шкалы SF2 фильтр не слышен
    }
    if (freq < kMinCutoffHz) freq = kMinCutoffHz;
    if (freq > kMaxCutoffHz) freq = kMaxCutoffHz;
    const float nyquist = static_cast<float>(kSampleRateHz) * 0.5f;
    if (freq > nyquist) freq = nyquist;

    // Биквад ФНЧ RBJ (.mid). У фильтра IT при нулевом резонансе завал
    // начинается за октаву до среза (на срезе -5 дБ), у SF2 до среза ровно: на
    // срезах выше 5 кГц это до 6 дБ темнее эталона. Полюса RBJ в рекурсии IT,
    // на входе числитель [1 2 1]/4, a0 - усиление 1 на нулевой частоте.
    if (sf2_response) {
        const float q_cb = static_cast<float>(res) * (240.0f / 128.0f);
        const float q = std::pow(10.0f, (q_cb - 3.01f) / 200.0f);
        const float w = kTwoPi * freq / static_cast<float>(kSampleRateHz);
        const float cw = std::cos(w);
        const float alpha = std::sin(w) / (2.0f * q);
        const float inv = 1.0f / (1.0f + alpha);
        const float rbj_a1 = -2.0f * cw * inv;
        const float rbj_a2 = (1.0f - alpha) * inv;
        out.a0 = to_q24(1.0f + rbj_a1 + rbj_a2);
        out.b0 = to_q24(-rbj_a1);
        out.b1 = to_q24(-rbj_a2);
        if (out.a0 == 0) out.a0 = 1;
        out.fir121 = true;
        out.active = true;
        return out;
    }

    // Родная ветка IT: r = частота_микса / fc (у ModPlug наоборот, это
    // другой на слух фильтр).
    const float fc = freq * kTwoPi;
    const float r = static_cast<float>(kSampleRateHz) / fc;
    const float d = dmpfac * r + dmpfac - 1.0f;
    const float e = r * r;

    const float denom = 1.0f + d + e;
    const float fg = 1.0f / denom;
    const float fb0 = (d + e + e) / denom;
    const float fb1 = -e / denom;

    out.b0 = to_q24(fb0);
    out.b1 = to_q24(fb1);
    out.a0 = to_q24(fg);
    // На очень низком срезе fg округляется в ноль и голос замолкает: ставится
    // единица.
    if (out.a0 == 0) out.a0 = 1;
    out.active = true;
    return out;
}

namespace {

// Одна рекурсия на оба фильтра. kFir121 - биквад RBJ (.mid): на входе
// числитель [1 2 1]/4, вход умножается на 64 вместо 256 - это и есть деление
// на 4, без потери разрядов.
template <bool kFir121>
SOUNDSINTH_ALWAYS_INLINE void filter_loop(int16_t* samples, uint32_t n, const FilterCoeffs& c, FilterState& s) {
    // Коэффициенты в локальных переменных: константны весь тик, компилятору
    // проще держать их в регистрах, чем перечитывать из структуры.
    const int32_t a0 = c.a0;
    const int32_t b0 = c.b0;
    const int32_t b1 = c.b1;
    int32_t y0 = s.y0;
    int32_t y1 = s.y1;
    int32_t x1 = s.x1;
    int32_t x2 = s.x2;
    // Насыщение y1 в цикле не считается - это тождество: в конце итерации
    // y1 = y0, поэтому y1 на шаге i - это y0 шага i-1, его насыщение уже
    // посчитано. Готовое значение переносится вперёд.
    int32_t y1c = (y1 > kStateClip) ? kStateClip : ((y1 < -kStateClip) ? -kStateClip : y1);

    // По указателю, а не по индексу: у варианта kFir121 иначе не хватает
    // регистров, и a0, b1 перечитываются со стека на каждом отсчёте.
    for (int16_t* const end = samples + n; samples != end; ++samples) {
        int32_t in;
        if constexpr (kFir121) {
            const int32_t x = *samples;
            in = (x + 2 * x1 + x2) * (kPreamp / 4);
            x2 = x1;
            x1 = x;
        } else {
            // Вход умножается на 256: без этого на тихом сигнале с низким срезом
            // состояние теряет точность и фильтр шумит.
            in = static_cast<int32_t>(*samples) * kPreamp;
        }

        const int32_t y0c = (y0 > kStateClip) ? kStateClip : ((y0 < -kStateClip) ? -kStateClip : y0);

        // Сначала округление и слагаемые, не зависящие от прошлого выхода,
        // y0c * b0 - последним: на цепочке рекурсии остаётся одно умножение с
        // накоплением. Сумма в int64 точна при любом порядке.
        const int64_t acc = (static_cast<int64_t>(1) << (kPrecision - 1)) + static_cast<int64_t>(in) * a0 +
                            static_cast<int64_t>(y1c) * b1 + static_cast<int64_t>(y0c) * b0;
        const int32_t val = static_cast<int32_t>(acc >> kPrecision);

        y1 = y0;
        y1c = y0c; // насыщение y1 на следующем шаге
        y0 = val;

        // Ограничение int16: буфер голоса 16-битный, на резонансе уровень выше
        // входного, и значение иначе заворачивается в другой знак. Прижим - от
        // уже поделённого: деление усекает к нулю, а сдвиг без поправки знака
        // дал бы у отрицательных на единицу меньше.
        *samples = static_cast<int16_t>(sat_s16(val / kPreamp));
    }

    s.y0 = y0;
    s.y1 = y1;
    if constexpr (kFir121) {
        s.x1 = static_cast<int16_t>(x1);
        s.x2 = static_cast<int16_t>(x2);
    }
}

} // namespace

void SOUNDSINTH_HOT_PATH(filter_apply)(int16_t* samples, uint32_t n, const FilterCoeffs& c, FilterState& s) {
    if (c.fir121) {
        filter_loop<true>(samples, n, c, s);
    } else {
        filter_loop<false>(samples, n, c, s);
    }
}

} // namespace soundsinth::engine
