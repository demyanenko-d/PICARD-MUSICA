#include "testing.h"

#include <cstdio>
#include <initializer_list>

#include "core/codec/loop_unroll.h"
#include "core/codec/resident_encoding_policy.h"
#include "core/model/song.h"

using namespace soundsinth::model;

namespace {

void test_8bit_always_raw8_but_still_subject_to_decimation() {
    std::printf("test_resident_encoding_policy_8bit_always_raw8_but_still_subject_to_decimation\n");

    // 8-битные исходники всегда Raw8 (mode), но правило децимации (>1 МБ и
    // >20 кГц) применяется к любому сэмплу независимо от битности.
    const auto huge = decide_resident_encoding(/*is16bit=*/false, /*length_samples=*/50'000'000, /*c5_speed=*/96000);
    CHECK(huge.mode == ResidentEncoding::Raw8);
    CHECK(huge.decimate); // большой и высокочастотный 8-бит - тоже децимируется

    const auto tiny = decide_resident_encoding(false, 10, 8363);
    CHECK(tiny.mode == ResidentEncoding::Raw8);
    CHECK(!tiny.decimate); // маленький - не подпадает под порог размера
}

void test_16bit_below_size_threshold_not_decimated() {
    std::printf("test_resident_encoding_policy_16bit_below_size_threshold_not_decimated\n");

    // DPCM8-размер = length_samples + чекпоинты*2. Берём заведомо меньше
    // 1 МБ даже с учётом чекпоинтов, на высокой частоте (>20 кГц), чтобы
    // изолированно проверить порог размера, а не частоты.
    const auto decision = decide_resident_encoding(/*is16bit=*/true, /*length_samples=*/500'000, /*c5_speed=*/44100);
    CHECK(decision.mode == ResidentEncoding::Dpcm8);
    CHECK(!decision.decimate);
}

void test_16bit_above_size_threshold_but_low_rate_not_decimated() {
    std::printf("test_resident_encoding_policy_16bit_above_size_but_low_rate_not_decimated\n");

    // >1 МБ по размеру, но частота ниже порога (20 кГц) - низкочастотный
    // сэмпл класса Amiga; децимация нарочно не применяется (не срезать и без
    // того скудные верха).
    const auto decision = decide_resident_encoding(/*is16bit=*/true, /*length_samples=*/2'000'000, /*c5_speed=*/8363);
    CHECK(decision.mode == ResidentEncoding::Dpcm8);
    CHECK(!decision.decimate);
}

void test_16bit_above_both_thresholds_decimated() {
    std::printf("test_resident_encoding_policy_16bit_above_both_thresholds_decimated\n");

    // >1 МБ по размеру и >20 кГц - оба условия выполнены, децимация должна
    // сработать.
    const auto decision = decide_resident_encoding(/*is16bit=*/true, /*length_samples=*/2'000'000, /*c5_speed=*/44100);
    CHECK(decision.mode == ResidentEncoding::Dpcm8);
    CHECK(decision.decimate);
}

void test_16bit_size_threshold_boundary() {
    std::printf("test_resident_encoding_policy_16bit_size_threshold_boundary\n");

    // Порог - строго "больше" 1 МБ (не "больше либо равно"); length_samples
    // подобран так, чтобы DPCM8-размер (length + чекпоинты*2) оказался на
    // границе и чуть выше неё.
    constexpr uint32_t kThresholdBytes = kForceDownsampleSizeThresholdBytes;
    // Подбираем length так, чтобы length + ceil(length/256)*2 == kThresholdBytes
    // приближённо (чекпоинт-надбавка мала относительно 1 МБ, ~0.78%) -
    // не требуется побитная точность границы, важно поведение по обе стороны.
    const uint32_t just_below = static_cast<uint32_t>(kThresholdBytes * 0.99);
    const uint32_t just_above = static_cast<uint32_t>(kThresholdBytes * 1.01);

    const auto below = decide_resident_encoding(true, just_below, 44100);
    CHECK(!below.decimate);

    const auto above = decide_resident_encoding(true, just_above, 44100);
    CHECK(above.decimate);
}

// Граница Raw16 по точному счёту страниц: 16-битный сэмпл 10240 отсчётов
// (Raw16 - 20 страниц) и 8-битный 3072 (3 страницы) - при 23 свободных Raw16,
// при 22 - Dpcm8; нерезидентный сэмпл в счёт не входит. С allow_raw16
// 16-битный сэмпл - Raw16 без прореживания при любой длине; 8-битный - Raw8
// по прежнему правилу. Порог прореживания - строго больше 1 МБ: 8 бит,
// 1048576 - нет, 1048577 - да.
void test_raw16_and_exact_size_boundaries() {
    std::printf("test_resident_encoding_policy_raw16_and_exact_size_boundaries\n");
    for (uint32_t free_pages : {23u, 22u}) {
        SampleDescriptor samples[3];
        samples[0].encoding = SampleEncoding::Pcm16;
        samples[0].length_samples = samples[0].source_length_samples = 10240;
        samples[1].encoding = SampleEncoding::Pcm8;
        samples[1].length_samples = samples[1].source_length_samples = 3072;
        samples[2].encoding = SampleEncoding::Pcm16;
        samples[2].length_samples = samples[2].source_length_samples = 100000;
        samples[2].unsupported_codec = true;
        Song song;
        song.samples = samples;
        song.sample_count = 3;
        const bool raw16 = choose_resident_encoding(song, free_pages);
        CHECK_EQ(raw16, free_pages == 23u);
        CHECK(samples[0].resident_encoding == (raw16 ? ResidentEncoding::Raw16 : ResidentEncoding::Dpcm8));
        CHECK(samples[1].resident_encoding == ResidentEncoding::Raw8);
    }
    const auto r16 = decide_resident_encoding(true, 50'000'000, 96000, /*allow_raw16=*/true);
    CHECK(r16.mode == ResidentEncoding::Raw16);
    CHECK(!r16.decimate);
    const auto r8 = decide_resident_encoding(false, 50'000'000, 96000, /*allow_raw16=*/true);
    CHECK(r8.mode == ResidentEncoding::Raw8);
    CHECK(r8.decimate);
    CHECK(!decide_resident_encoding(false, kForceDownsampleSizeThresholdBytes, 44100).decimate);
    CHECK(decide_resident_encoding(false, kForceDownsampleSizeThresholdBytes + 1, 44100).decimate);
}

// resolve_sample_index: диапазоны keymap, note_offset с прижимом к 119,
// kNoSample и индекс за песней - разные ответы out_unmapped, равные start_note -
// берётся последний, без диапазонов - default_sample_index.
void test_resolve_sample_index() {
    std::printf("test_resolve_sample_index\n");
    SampleDescriptor samples[2];
    samples[0].length_samples = samples[1].length_samples = 100;
    const KeymapRange map_a[4] = {{0, 0, 0}, {40, 1, 12}, {80, kNoSample, 0}, {100, 5, 0}};
    const KeymapRange map_b[2] = {{0, 0, 0}, {0, 1, 12}}; // одинаковый start_note
    Instrument ins[3];
    ins[0].note_to_sample_ranges = map_a;
    ins[0].note_to_sample_range_count = 4;
    ins[1].note_to_sample_ranges = map_b;
    ins[1].note_to_sample_range_count = 2;
    ins[2].default_sample_index = 1; // без диапазонов
    Song song;
    song.samples = samples;
    song.sample_count = 2;
    song.instruments = ins;
    song.instrument_count = 3;
    uint16_t idx = 0xFFFF;
    uint8_t note = 0;
    bool unmapped = false;
    CHECK(resolve_sample_index(song, 1, 39, &idx, &note, &unmapped));
    CHECK_EQ(idx, static_cast<uint16_t>(0));
    CHECK_EQ(note, static_cast<uint8_t>(39));
    CHECK(!unmapped);
    CHECK(resolve_sample_index(song, 1, 40, &idx, &note));
    CHECK_EQ(idx, static_cast<uint16_t>(1));
    CHECK_EQ(note, static_cast<uint8_t>(52));
    CHECK(!resolve_sample_index(song, 1, 80, &idx, &note, &unmapped)); // kNoSample
    CHECK(unmapped);
    CHECK(!resolve_sample_index(song, 1, 100, &idx, &note, &unmapped)); // индекс 5 за песней
    CHECK(!unmapped);
    CHECK(!resolve_sample_index(song, 0, 40, &idx, &note, &unmapped));
    CHECK(unmapped);
    CHECK(!resolve_sample_index(song, 4, 40, &idx, &note, &unmapped));
    CHECK(unmapped);
    CHECK(resolve_sample_index(song, 2, 5, &idx, &note)); // последний из одинаковых
    CHECK_EQ(idx, static_cast<uint16_t>(1));
    CHECK(resolve_sample_index(song, 2, 115, &idx, &note)); // 115 + 12 - прижим к 119
    CHECK_EQ(note, static_cast<uint8_t>(119));
    CHECK(resolve_sample_index(song, 3, 60, &idx, &note));
    CHECK_EQ(idx, static_cast<uint16_t>(1));
    CHECK_EQ(note, static_cast<uint8_t>(60));
}

} // namespace

void run_resident_encoding_policy_tests() {
    test_raw16_and_exact_size_boundaries();
    test_resolve_sample_index();
    test_8bit_always_raw8_but_still_subject_to_decimation();
    test_16bit_below_size_threshold_not_decimated();
    test_16bit_above_size_threshold_but_low_rate_not_decimated();
    test_16bit_above_both_thresholds_decimated();
    test_16bit_size_threshold_boundary();
}
