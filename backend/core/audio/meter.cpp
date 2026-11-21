// SPDX-License-Identifier: MIT
#include "core/audio/meter.h"

#include <array>
#include <cstring>

#include "platform/hot_path.h"

#if defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
#endif

namespace soundsinth::mixbus {

namespace {

constexpr uint32_t kN       = Meter::kFft;
constexpr uint32_t kHalf    = kN / 2;
constexpr uint32_t kQuarter = kN / 4;

// Четверть периода синуса в Q15: sin(pi/2 * i / kQuarter), рядом Тейлора.
constexpr std::array<int16_t, kQuarter + 1> kSine = [] {
    std::array<int16_t, kQuarter + 1> t{};
    for (uint32_t i = 0; i <= kQuarter; i++) {
        const double x = 1.5707963267948966 * i / kQuarter, x2 = x * x;
        const double s = x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0))));
        t[i]           = static_cast<int16_t>(s * 32767.0 + 0.5);
    }
    return t;
}();

// sin(2 pi k / kN), Q15.
constexpr int32_t sine(uint32_t k) {
    k                &= kN - 1u;
    const uint32_t r  = k & (kQuarter - 1u);
    const int32_t v   = ((k / kQuarter) & 1u) != 0 ? kSine[kQuarter - r] : kSine[r];
    return k >= kN / 2 ? -v : v;
}
constexpr int32_t cosine(uint32_t k) {
    return sine(k + kQuarter);
}

// Комплексное число - слово: действительная часть в младших 16 битах,
// мнимая в старших. На M33 - команды DSP, на ПК - то же по половинам.
constexpr int32_t lo(uint32_t v) {
    return static_cast<int16_t>(v & 0xffffu);
}
constexpr int32_t hi(uint32_t v) {
    return static_cast<int16_t>(v >> 16);
}
constexpr uint32_t pack(int32_t re, int32_t im) {
    return (static_cast<uint32_t>(re) & 0xffffu) | (static_cast<uint32_t>(im) << 16);
}
#if defined(__ARM_FEATURE_SIMD32)
// Действительная и мнимая часть w * b, Q15 до сдвига.
inline int32_t mul_re(uint32_t w, uint32_t b) {
    return __smusd(static_cast<int16x2_t>(w), static_cast<int16x2_t>(b));
}
inline int32_t mul_im(uint32_t w, uint32_t b) {
    return __smuadx(static_cast<int16x2_t>(w), static_cast<int16x2_t>(b));
}
// (a + b) / 2 и (a - b) / 2 по половинам.
inline uint32_t half_add(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>(__shadd16(static_cast<int16x2_t>(a), static_cast<int16x2_t>(b)));
}
inline uint32_t half_sub(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>(__shsub16(static_cast<int16x2_t>(a), static_cast<int16x2_t>(b)));
}
inline uint32_t rev8(uint32_t i) {
    return __rbit(i) >> 24;
}
#else
inline int32_t mul_re(uint32_t w, uint32_t b) {
    return lo(w) * lo(b) - hi(w) * hi(b);
}
inline int32_t mul_im(uint32_t w, uint32_t b) {
    return lo(w) * hi(b) + hi(w) * lo(b);
}
inline uint32_t half_add(uint32_t a, uint32_t b) {
    return pack((lo(a) + lo(b)) >> 1, (hi(a) + hi(b)) >> 1);
}
inline uint32_t half_sub(uint32_t a, uint32_t b) {
    return pack((lo(a) - lo(b)) >> 1, (hi(a) - hi(b)) >> 1);
}
inline uint32_t rev8(uint32_t i) {
    uint32_t r = 0;
    for (uint32_t b = 0; b < 8; b++) {
        r = (r << 1) | ((i >> b) & 1u);
    }
    return r;
}
#endif

// Окно Ханна в Q15, отсчёты 0..kN/2: окно симметрично, w(kN - i) = w(i).
constexpr std::array<int16_t, kHalf + 1> kHann = [] {
    std::array<int16_t, kHalf + 1> t{};
    for (uint32_t i = 0; i <= kHalf; i++) {
        t[i] = static_cast<int16_t>(16384 - cosine(i) / 2);
    }
    return t;
}();

// Повороты exp(-2 pi i k / kN), k < kN / 2: и для БПФ на kN / 2 точек (через
// один), и для разделения его итога.
constexpr std::array<uint32_t, kHalf> kTwiddle = [] {
    std::array<uint32_t, kHalf> t{};
    for (uint32_t k = 0; k < kHalf; k++) {
        t[k] = pack(cosine(k), -sine(k));
    }
    return t;
}();

// Полоса b - отсчёты спектра [kEdge[b], kEdge[b + 1]); отсчёт - 86 Гц.
constexpr uint8_t kEdge[kSpectrumBands + 1] = {1, 2, 3, 4, 5, 6, 8, 10, 13, 17, 23, 31, 43, 59, 82, 114, 186};

// Квадрат полной шкалы в шкале уровня, log2 в 1/16: пик - 32768^2; полоса -
// синус в полную шкалу после окна Ханна и деления БПФ на длину
// (8192^2 * 1.5 по трём отсчётам главного лепестка).
constexpr int32_t kPeakFull = 30 * 16;
constexpr int32_t kBandFull = 425;

uint8_t level_of_power(uint32_t power, int32_t full) {
    const int32_t v = 255 + log2_fixed(power, 4) - full;
    return power == 0 || v < 0 ? uint8_t{0} : v > 255 ? uint8_t{255} : static_cast<uint8_t>(v);
}

} // namespace

int32_t log2_fixed(uint32_t v, uint32_t frac_bits) {
    if (v == 0) return 0;
    uint32_t e = 31;
    while ((v >> e) == 0) {
        e--;
    }
    // Мантисса после старшего разряда - дробная часть, линейно.
    const uint32_t frac = (e >= frac_bits ? v >> (e - frac_bits) : v << (frac_bits - e)) & ((1u << frac_bits) - 1u);
    return static_cast<int32_t>((e << frac_bits) | frac);
}

uint8_t level_of(uint32_t amplitude) {
    if (amplitude > 32768u) amplitude = 32768u;
    return level_of_power(amplitude * amplitude, kPeakFull);
}

void Meter::feed(const int16_t* interleaved, uint32_t n_frames) {
    uint32_t pl = peak_l_, pr = peak_r_, bp = 0, w = written_;
    const bool ring = !hold_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n_frames; i++) {
        const int32_t l = interleaved[2 * i], r = interleaved[2 * i + 1];
        const uint32_t al = static_cast<uint32_t>(l < 0 ? -l : l), ar = static_cast<uint32_t>(r < 0 ? -r : r);
        if (al > pl) pl = al;
        if (ar > pr) pr = ar;
        if (al > bp) bp = al;
        if (ar > bp) bp = ar;
        if (ring) ring_[w++ & (kRing - 1u)] = static_cast<int16_t>((l + r) / 2);
    }
    peak_l_ = static_cast<uint16_t>(pl > 32767u ? 32767u : pl);
    peak_r_ = static_cast<uint16_t>(pr > 32767u ? 32767u : pr);
    if (ring) {
        // Атака: пик буфера в полтора раза выше прошлого и не тише kOnsetFloor.
        // Отметка - середина буфера: по ней счёт возьмёт окно с атакой в
        // центре, где окно Ханна её не гасит.
        if (!onset_pending_ && bp > kOnsetFloor && 2u * bp > 3u * last_peak_) {
            onset_at_      = written_ + n_frames / 2u;
            onset_pending_ = true;
        }
        written_ = w;
    }
    last_peak_ = static_cast<uint16_t>(bp > 0xffffu ? 0xffffu : bp);
}

void Meter::take_vu(uint8_t& left, uint8_t& right) {
    left    = level_of(peak_l_);
    right   = level_of(peak_r_);
    peak_l_ = 0;
    peak_r_ = 0;
}

bool Meter::analyze() {
    const uint32_t now = written_;
    bool any           = false;
    // Окно с атакой в центре - когда после неё прошло полокна и она ещё в круге.
    if (onset_pending_ && now - onset_at_ >= kN / 2u) {
        if (now - onset_at_ <= kRing - kN / 2u) {
            count(onset_at_ + kN / 2u);
            any = true;
        }
        onset_pending_ = false;
    }
    if (now != counted_at_) {
        count(now);
        counted_at_ = now;
        any         = true;
    }
    return any;
}

// В SRAM: счёт идёт около 100 раз в секунду, а кэш флеша делит PSRAM.
SOUNDSINTH_HOT_PATH_ATTR("meter_count")
void Meter::count(uint32_t end) {
    // kN отсчётов круга, кончая отсчётом end - 1, с окном - в рабочий
    // буфер; на время копии круг стоит. Шкала - половина: пара отсчётов
    // как комплексное число по модулю до корня из двух от отсчёта, и
    // бабочки без этого вышли бы за int16.
    hold_.store(true, std::memory_order_release);
    for (uint32_t i = 0; i < kN; i++) {
        work_[i] = static_cast<int16_t>((ring_[(end - kN + i) & (kRing - 1u)] * kHann[i <= kHalf ? i : kN - i]) >> 16);
    }
    hold_.store(false, std::memory_order_release);
    // Вещественные отсчёты 2n и 2n + 1 - комплексное z[n]: БПФ на kHalf точек
    // на месте, потом разделение. Слово читается через memcpy - work_
    // выровнен на 4.
    auto at = [this](uint32_t n) {
        uint32_t v;
        std::memcpy(&v, work_ + 2 * n, sizeof(v));
        return v;
    };
    auto put = [this](uint32_t n, uint32_t v) { std::memcpy(work_ + 2 * n, &v, sizeof(v)); };
    for (uint32_t i = 1; i < kHalf; i++) {
        const uint32_t j = rev8(i);
        if (i < j) {
            const uint32_t t = at(i);
            put(i, at(j));
            put(j, t);
        }
    }
    // Бабочки с делением на два на каждом из восьми проходов: вместе с
    // половиной шкалы окна итог - ДПФ, делённое на kN, как у БПФ на kN точек.
    // Первые два прохода без умножений: поворот 1, во втором ещё -i, а
    // (-i)(re + i im) = im - i re.
    for (uint32_t i = 0; i < kHalf; i += 2) {
        const uint32_t a = at(i), b = at(i + 1);
        put(i, half_add(a, b));
        put(i + 1, half_sub(a, b));
    }
    for (uint32_t i = 0; i < kHalf; i += 4) {
        uint32_t a = at(i), b = at(i + 2);
        put(i, half_add(a, b));
        put(i + 2, half_sub(a, b));
        a                = at(i + 1);
        b                = at(i + 3);
        const uint32_t t = pack(hi(b), -lo(b));
        put(i + 1, half_add(a, t));
        put(i + 3, half_sub(a, t));
    }
    for (uint32_t half = 4; half < kHalf; half *= 2) {
        const uint32_t step = kHalf / half;
        for (uint32_t j = 0; j < half; j++) {
            const uint32_t w = kTwiddle[j * step];
            for (uint32_t i = j; i < kHalf; i += 2 * half) {
                const uint32_t a = at(i), b = at(i + half);
                const uint32_t t = pack(mul_re(w, b) >> 15, mul_im(w, b) >> 15);
                put(i, half_add(a, t));
                put(i + half, half_sub(a, t));
            }
        }
    }
    // Разделение: X[k] = (Z[k] + Z*[N - k]) / 2 + W^k (Z[k] - Z*[N - k]) / 2i,
    // N = kHalf, W = exp(-2 pi i / kN). Считается вдвое, делится в конце.
    for (uint32_t b = 0; b < kSpectrumBands; b++) {
        uint32_t power = 0;
        for (uint32_t k = kEdge[b]; k < kEdge[b + 1]; k++) {
            const uint32_t z = at(k), c = at(kHalf - k), w = kTwiddle[k];
            const int32_t er = lo(z) + lo(c), ei = hi(z) - hi(c);
            const int32_t dr = lo(z) - lo(c), di = hi(z) + hi(c);
            const int32_t xr  = (er + ((lo(w) * di + hi(w) * dr) >> 15)) >> 1;
            const int32_t xi  = (ei + ((hi(w) * di - lo(w) * dr) >> 15)) >> 1;
            power            += static_cast<uint32_t>(xr * xr + xi * xi);
        }
        const uint8_t level = level_of_power(power, kBandFull);
        if (level > max_[b]) max_[b] = level;
    }
    counted_ = true;
}

bool Meter::take_spectrum(uint8_t bands[kSpectrumBands]) {
    analyze();
    if (!counted_) return false;
    for (uint32_t b = 0; b < kSpectrumBands; b++) {
        bands[b] = max_[b];
        max_[b]  = 0;
    }
    counted_ = false;
    return true;
}

} // namespace soundsinth::mixbus
