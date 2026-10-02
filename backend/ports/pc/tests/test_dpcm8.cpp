// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "core/codec/dpcm8.h"

using namespace soundsinth::dpcm8;

namespace {

void test_roundtrip_bounded_error() {
    std::printf("test_dpcm8_roundtrip_bounded_error\n");

    // Период 2000 сэмплов, амплитуда 8000: перепад между соседними сэмплами
    // (амплитуда*2*pi/период ~ 25) укладывается в 1-2 нижних масштаба
    // (kScaleTable={1,3,9,...}), ошибка ожидается очень маленькой
    // (компандированная схема даёт точность до 1 на малых дельтах, см.
    // dpcm8.h).
    constexpr uint32_t kN = 4000;
    std::vector<int16_t> samples(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        samples[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * i / 2000.0));
    }

    std::vector<int8_t> dpcm(kN);
    std::vector<Dpcm8Checkpoint> checkpoints((kN + kCheckpointIntervalSamples - 1) / kCheckpointIntervalSamples);
    uint32_t checkpoint_count = 0;
    Dpcm8State encode_state;
    checkpoint_count = encode_block(samples.data(), kN, 0, encode_state, dpcm.data(), checkpoints.data(), static_cast<uint32_t>(checkpoints.size()));
    CHECK_EQ(checkpoint_count, static_cast<uint32_t>(checkpoints.size()));

    std::vector<int16_t> decoded(kN);
    decode_block(dpcm.data(), kN, Dpcm8State{}, decoded.data());

    double sum_abs_err = 0;
    for (uint32_t i = 0; i < kN; ++i) {
        sum_abs_err += std::abs(static_cast<int>(decoded[i]) - static_cast<int>(samples[i]));
    }
    const double avg_abs_err = sum_abs_err / kN;
    CHECK(avg_abs_err < 3.0);
}

void test_roundtrip_full_scale_high_frequency_much_better_than_flat_dpcm() {
    std::printf("test_dpcm8_roundtrip_full_scale_high_frequency_much_better_than_flat_dpcm\n");

    // Резкий сигнал из жалобы пользователя на старый плоский DPCM8 (+-127):
    // период 97 сэмплов, амплитуда 8000, пиковый перепад между соседними
    // сэмплами ~518 - заведомо за пределами старого диапазона +-127 (был бы
    // систематический slope overload на каждом шаге около нулей синусоиды).
    // Компандированная схема (верхний масштаб 2048) должна пройти этот сигнал
    // на порядок точнее.
    constexpr uint32_t kN = 2000;
    std::vector<int16_t> samples(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        samples[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * i / 97.0));
    }

    std::vector<int8_t> dpcm(kN);
    std::vector<Dpcm8Checkpoint> checkpoints((kN + kCheckpointIntervalSamples - 1) / kCheckpointIntervalSamples);
    uint32_t checkpoint_count = 0;
    Dpcm8State encode_state;
    checkpoint_count = encode_block(samples.data(), kN, 0, encode_state, dpcm.data(), checkpoints.data(), static_cast<uint32_t>(checkpoints.size()));

    std::vector<int16_t> decoded(kN);
    decode_block(dpcm.data(), kN, Dpcm8State{}, decoded.data());

    double sum_abs_err = 0;
    for (uint32_t i = 0; i < kN; ++i) {
        sum_abs_err += std::abs(static_cast<int>(decoded[i]) - static_cast<int>(samples[i]));
    }
    const double avg_abs_err = sum_abs_err / kN;
    // Старый плоский DPCM8 (+-127) на этом же сигнале (см. историю файла)
    // давал систематическую "лесенку", средняя ошибка была бы порядка сотен.
    // Порог на порядок меньше, с запасом.
    CHECK(avg_abs_err < 60.0);
}

void test_checkpoint_resume_matches_full_decode_exactly() {
    std::printf("test_dpcm8_checkpoint_resume_matches_full_decode_exactly\n");

    constexpr uint32_t kN = 1000;
    std::vector<int16_t> samples(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        // Резкие скачки, чтобы состояние (predictor) заметно менялось между
        // чекпоинтами и разные масштабы задействовались по очереди.
        samples[i] = static_cast<int16_t>(((i * 6151) % 20000) - 10000);
    }

    std::vector<int8_t> dpcm(kN);
    std::vector<Dpcm8Checkpoint> checkpoints((kN + kCheckpointIntervalSamples - 1) / kCheckpointIntervalSamples);
    uint32_t checkpoint_count = 0;
    Dpcm8State encode_state;
    checkpoint_count = encode_block(samples.data(), kN, 0, encode_state, dpcm.data(), checkpoints.data(), static_cast<uint32_t>(checkpoints.size()));

    std::vector<int16_t> full_decoded(kN);
    decode_block(dpcm.data(), kN, Dpcm8State{}, full_decoded.data());

    // Чекпоинт k=2 (позиция 512) - состояние перед декодированием сэмпла 512.
    // Декодирование с этой точки должно точно (оба пути идут через один и тот
    // же код декодера) совпасть с соответствующим срезом полного
    // декодирования с нуля.
    const uint32_t k            = 2;
    const uint32_t start_sample = k * kCheckpointIntervalSamples;
    CHECK(start_sample < kN);
    CHECK(k < checkpoint_count);

    const Dpcm8State resume_state{checkpoints[k].predictor};
    const uint32_t resume_count = kN - start_sample;
    std::vector<int16_t> resumed(resume_count);
    decode_block(dpcm.data() + start_sample, resume_count, resume_state, resumed.data());

    for (uint32_t i = 0; i < resume_count; ++i) {
        CHECK_EQ(resumed[i], full_decoded[start_sample + i]);
    }

    // Чекпоинт 0 - канонический старт потока {predictor=0}, не особый случай.
    CHECK_EQ(checkpoints[0].predictor, static_cast<int16_t>(0));
}

void test_small_residual_uses_finest_scale_lossless() {
    std::printf("test_dpcm8_small_residual_uses_finest_scale_lossless\n");

    // Остаток в пределах +-15 должен кодироваться на scale=0 (шаг 1) без
    // ошибки квантования (см. dpcm8.h: компандирование не должно ухудшать
    // точность на тихих и гладких участках).
    Dpcm8State state; // predictor=0
    for (int16_t target : {0, 5, -5, 15, -15}) {
        const uint8_t code = quantize_sample(target, state);
        CHECK_EQ(static_cast<uint32_t>(code >> 5), static_cast<uint32_t>(0)); // scale=0
        Dpcm8State tmp        = state;
        const int16_t decoded = decode_delta(code, tmp);
        CHECK_EQ(decoded, target); // без потерь
    }
}

void test_wide_dynamic_range_handles_near_full_scale_jump() {
    std::printf("test_dpcm8_wide_dynamic_range_handles_near_full_scale_jump\n");

    // Скачок с -20000 на +20000 (остаток на втором шаге 40480, почти весь
    // диапазон int16): квантователь должен насытиться на максимальном
    // масштабе/значении (scale=7, value=15 -> delta=30720), не переполниться и
    // не завернуться, и подойти к цели намного ближе, чем старый плоский DPCM8
    // (+-127): там шаг был бы 127 из требуемых ~40000, здесь 30720.
    const int16_t samples[2] = {-20000, 20000};
    Dpcm8State state;
    const uint8_t d0     = quantize_sample(samples[0], state);
    const int16_t after0 = decode_delta(d0, state);
    CHECK(after0 > -20700 && after0 < -19700); // остаток -20000 сам в пределах масштаба 7 (2048*-10=-20480) - почти точно

    const uint8_t d1     = quantize_sample(samples[1], state);
    const int16_t after1 = decode_delta(d1, state);
    CHECK(after1 > after0); // сдвинулся в сторону цели
    CHECK(after1 < samples[1]); // но не долетел за один шаг (сам остаток > максимально представимой дельты)
    CHECK(after1 > 10000); // главное: намного ближе к цели, чем дал бы старый диапазон +-127 (там after1 было бы ~-19556)
}

} // namespace

// Закреп кодировщика: байты encode_block на фиксированном сигнале и
// quantize_sample на всех остатках при трёх предикторах - одним хэшем.
// Любая правка кодировщика меняет байты Dpcm8 в PSRAM и в собранных банках;
// тест делает её видимой. Новое число - только вместе с решением о правке.
uint32_t fnv1a(uint32_t h, uint8_t b) {
    return (h ^ b) * 16777619u;
}

void test_encoder_fingerprint() {
    std::printf("test_dpcm8_encoder_fingerprint\n");
    constexpr uint32_t kN = 65536;
    std::vector<int16_t> src(kN);
    uint32_t x = 12345;
    for (uint32_t i = 0; i < kN; ++i) {
        x = x * 1664525u + 1013904223u;
        // Смесь плавного и скачков: работают все масштабы.
        src[i] = static_cast<int16_t>((i % 3000 < 1500 ? int32_t(x >> 20) - 2048 : int32_t(x >> 16) - 32768));
    }
    std::vector<int8_t> out(kN);
    std::vector<soundsinth::dpcm8::Dpcm8Checkpoint> cp(kN / soundsinth::dpcm8::kCheckpointIntervalSamples + 1);
    soundsinth::dpcm8::Dpcm8State st{};
    soundsinth::dpcm8::encode_block(src.data(), kN, 0, st, out.data(), cp.data(), static_cast<uint32_t>(cp.size()));
    uint32_t h = 2166136261u;
    for (int8_t b : out)
        h = fnv1a(h, static_cast<uint8_t>(b));
    for (const int16_t pred : {int16_t(0), int16_t(30000), int16_t(-30000)}) {
        soundsinth::dpcm8::Dpcm8State s{pred};
        for (int32_t r = -65535; r <= 65535; ++r) {
            const int32_t target = int32_t(pred) + r;
            if (target < -32768 || target > 32767) continue;
            h = fnv1a(h, soundsinth::dpcm8::quantize_sample(static_cast<int16_t>(target), s));
        }
    }
    std::printf("  hash 0x%08X\n", h);
    CHECK_EQ(h, 0xB7F20FC0u);
}

void run_dpcm8_tests() {
    test_encoder_fingerprint();
    test_roundtrip_bounded_error();
    test_roundtrip_full_scale_high_frequency_much_better_than_flat_dpcm();
    test_checkpoint_resume_matches_full_decode_exactly();
    test_small_residual_uses_finest_scale_lossless();
    test_wide_dynamic_range_handles_near_full_scale_jump();
}
