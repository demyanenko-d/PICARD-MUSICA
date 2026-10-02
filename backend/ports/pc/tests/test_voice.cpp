// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "core/codec/dpcm8.h"
#include "core/engine/engine_defs.h" // kSampleRateHz
#include "core/engine/voice.h"
#include "core/model/instrument.h"
#include "core/model/song.h" // FrequencyModel
#include "core/memory/psram_store.h"
#include "core/codec/sample_pack.h"

// Синтетика для engine::Voice без реального файла и секвенсора (тот
// сценарий покрыт в test_sequencer_libxmp.cpp). Здесь проверяется сам
// голос: резидентный декод (Dpcm8/Raw8) из PSRAM-цепочки, линейная
// интерполяция, остановка и зацикливание на конце сэмпла, обе частотные
// модели. Большинство тестов используют Dpcm8 (синтетические исходные
// значения выходят за диапазон [-128,127], которым ограничен Raw8),
// отдельный тест на Raw8 - в конце файла.

namespace {

using namespace soundsinth;
using soundsinth::model::FrequencyModel;
using soundsinth::model::ResidentEncoding;

// Кладёт сэмпл в PSRAM тем же путём, каким загрузчики форматов наполняют
// кэш (sample_pack::SamplePacker, Mode::Dpcm8); Voice читает только уже
// загруженный кэш.
uint16_t repack_into_psram_dpcm8(memory::PsramStore& psram, const int16_t* native, uint32_t count) {
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Dpcm8);
    CHECK(packer.add_samples(native, count));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    CHECK_EQ(result.total_samples, count);
    return result.first_page;
}

// То же, но возвращает и checkpoint_first_page - для тестов быстрого пути
// voice_trigger через персистентные чекпоинты (а не отката на линейный
// decode-and-discard).
sample_pack::PackResult repack_into_psram_dpcm8_with_checkpoints(memory::PsramStore& psram, const int16_t* native, uint32_t count) {
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Dpcm8);
    CHECK(packer.add_samples(native, count));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    CHECK_EQ(result.total_samples, count);
    CHECK(result.checkpoint_first_page != memory::kPageChainEnd);
    return result;
}

// То, что голос прочитает из Dpcm8: кодек с потерями, оракулу - распакованное.
std::vector<int16_t> dpcm8_decoded(const std::vector<int16_t>& native) {
    const uint32_t n = static_cast<uint32_t>(native.size());
    std::vector<int8_t> bytes(n);
    std::vector<dpcm8::Dpcm8Checkpoint> cps(n / dpcm8::kCheckpointIntervalSamples + 2);
    dpcm8::Dpcm8State st{};
    dpcm8::encode_block(native.data(), n, 0, st, bytes.data(), cps.data(), static_cast<uint32_t>(cps.size()));
    std::vector<int16_t> out(n);
    dpcm8::decode_block(bytes.data(), n, dpcm8::Dpcm8State{}, out.data());
    return out;
}

// Последовательный оракул линейной интерполяции: окно (v[start], следующий),
// дальше prev = next и заворот перед чтением следующего - порядок рендера, без
// прыжков, пропусков и точек петли. Незацикленный кончается, когда читать
// нечего.
std::vector<int16_t> linear_oracle(const std::vector<int16_t>& v, bool loop, uint32_t loop_start, uint32_t loop_end, const std::vector<uint32_t>& steps,
                                   uint32_t start = 0) {
    std::vector<int16_t> out;
    const uint32_t end = loop ? loop_end : static_cast<uint32_t>(v.size());
    uint32_t decoded   = start + 1;
    int16_t prev       = v[start];
    if (loop && decoded >= end) decoded = loop_start;
    int16_t next  = v[decoded++];
    uint32_t frac = 0;
    for (const uint32_t s : steps) {
        const int32_t d = static_cast<int32_t>(next) - static_cast<int32_t>(prev);
        out.push_back(static_cast<int16_t>(prev + static_cast<int32_t>((static_cast<int64_t>(d) * frac) >> 16)));
        frac += s;
        while (frac >= 0x10000u) {
            frac -= 0x10000u;
            prev  = next;
            if (decoded >= end) {
                if (!loop) return out;
                decoded = loop_start;
            }
            next = v[decoded++];
        }
    }
    return out;
}

void test_voice_render_matches_independent_dpcm8_decode_1to1() {
    std::printf("test_voice_render_matches_independent_dpcm8_decode_1to1\n");

    constexpr uint32_t kN    = 8;
    const int16_t native[kN] = {100, 200, 300, 400, 500, 600, 700, 800};

    // Независимый оракул ожидаемых значений: те же dpcm8::encode_block и
    // decode_block, что проверены в test_dpcm8.cpp. Кодек с потерями, вручную
    // квантование точно не воспроизвести.
    int8_t dpcm_bytes[kN];
    dpcm8::Dpcm8Checkpoint oracle_checkpoints[4];
    uint32_t oracle_checkpoint_count = 0;
    dpcm8::Dpcm8State encode_state;
    oracle_checkpoint_count = dpcm8::encode_block(native, kN, 0, encode_state, dpcm_bytes, oracle_checkpoints, 4);

    int16_t expected[kN];
    dpcm8::decode_block(dpcm_bytes, kN, dpcm8::Dpcm8State{}, expected);

    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8(psram, native, kN);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz; // см. voice.cpp: на note=48 (period==C4Period) даёт step==0x10000 (1:1)
    sample.default_volume    = 64;

    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Amiga);
    CHECK(voice.active);
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x10000));

    int16_t out[kN]         = {};
    const uint32_t produced = engine::voice_render(voice, psram, out, kN);

    // При step 1:1 голос на первом выходном фрейме отдаёт ещё предыдущий
    // prev_sample (исходный сэмпл 0) и продвигается только после этого.
    // Последний исходный сэмпл (индекс kN-1) не успевает стать prev_sample:
    // раньше decoded_count достигает native_len, и голос останавливается.
    // Поэтому produced == kN-1, а не kN (см. voice.cpp).
    CHECK_EQ(produced, kN - 1);
    CHECK(!voice.active); // незацикленный сэмпл - дошёл до конца и остановился

    for (uint32_t i = 0; i < produced; ++i) {
        CHECK_EQ(out[i], expected[i]);
    }

    // Повторный рендер после конца сэмпла - 0 кадров, без падений.
    CHECK_EQ(engine::voice_render(voice, psram, out, kN), 0u);

    memory::psram_destroy(psram);
}

void test_voice_trigger_empty_sample_stays_inactive() {
    std::printf("test_voice_trigger_empty_sample_stays_inactive\n");

    memory::PsramStore psram{};
    memory::psram_create(psram);

    soundsinth::model::SampleDescriptor sample; // length_samples == 0 по умолчанию - пустой слот
    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, memory::kPageChainEnd, /*note=*/48, FrequencyModel::Amiga);
    CHECK(!voice.active);

    int16_t out[4];
    CHECK_EQ(engine::voice_render(voice, psram, out, 4), 0u);

    memory::psram_destroy(psram);
}

// Зацикливание: прямая петля через точку позиции декодера на loop_start.
// Весь выход - против последовательного оракула: точка, пойманная на
// отсчёт позже, тоже даёт период loop_len, только из других отсчётов, и
// периодичностью не ловится.
void test_voice_loop_repeats_via_checkpoint() {
    std::printf("test_voice_loop_repeats_via_checkpoint\n");

    constexpr uint32_t kN         = 12;
    const int16_t native[kN]      = {100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100, 1200};
    constexpr uint32_t kLoopStart = 4;
    constexpr uint32_t kLoopEnd   = 10;
    constexpr uint32_t kLoopLen   = kLoopEnd - kLoopStart;

    int8_t dpcm_bytes[kN];
    dpcm8::Dpcm8Checkpoint oracle_checkpoints[4];
    uint32_t oracle_checkpoint_count = 0;
    dpcm8::Dpcm8State encode_state;
    oracle_checkpoint_count = dpcm8::encode_block(native, kN, 0, encode_state, dpcm_bytes, oracle_checkpoints, 4);
    int16_t expected[kN];
    dpcm8::decode_block(dpcm_bytes, kN, dpcm8::Dpcm8State{}, expected);

    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8(psram, native, kN);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = kN;
    sample.loop_enabled      = true;
    sample.loop_start        = kLoopStart;
    sample.loop_end          = kLoopEnd;
    sample.c5_speed          = engine::kSampleRateHz; // step==0x10000 (1:1), см. тест выше

    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Amiga);
    CHECK(voice.active);
    CHECK(voice.loop_enabled);

    constexpr uint32_t kTotalFrames = kLoopEnd + 4 * kLoopLen; // интро + несколько полных витков петли
    int16_t out[kTotalFrames]       = {};
    const uint32_t produced         = engine::voice_render(voice, psram, out, kTotalFrames);

    CHECK_EQ(produced, kTotalFrames); // зацикленный сэмпл не останавливается
    CHECK(voice.active);

    const std::vector<int16_t> oracle =
        linear_oracle(std::vector<int16_t>(expected, expected + kN), true, kLoopStart, kLoopEnd, std::vector<uint32_t>(kTotalFrames, 0x10000u));
    for (uint32_t i = 0; i < kTotalFrames; ++i)
        CHECK_EQ(out[i], oracle[i]);

    memory::psram_destroy(psram);
}

// Ping-pong (loop_bidirectional) - не настоящий реверс (Dpcm8-декодер
// однонаправленный, см. voice.h/.cpp), но голос больше не должен
// замолкать на границе петли, как раньше, когда loop_bidirectional
// откатывался на "loop_enabled=false": играется как обычная прямая петля.
// Сэмпл и петля те же, что в тесте выше, только loop_bidirectional=true.
void test_voice_bidirectional_loop_plays_forward_instead_of_stopping() {
    std::printf("test_voice_bidirectional_loop_plays_forward_instead_of_stopping\n");

    constexpr uint32_t kN         = 12;
    const int16_t native[kN]      = {100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100, 1200};
    constexpr uint32_t kLoopStart = 4;
    constexpr uint32_t kLoopEnd   = 10;
    constexpr uint32_t kLoopLen   = kLoopEnd - kLoopStart;

    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8(psram, native, kN);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding  = ResidentEncoding::Dpcm8;
    sample.length_samples     = kN;
    sample.loop_enabled       = true;
    sample.loop_bidirectional = true; // ping-pong - раньше полностью отключал зацикливание
    sample.loop_start         = kLoopStart;
    sample.loop_end           = kLoopEnd;
    sample.c5_speed           = engine::kSampleRateHz;

    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Amiga);
    CHECK(voice.active);
    CHECK(voice.loop_enabled); // не false, как было бы раньше при loop_bidirectional==true

    constexpr uint32_t kTotalFrames = kLoopEnd + 4 * kLoopLen; // заведомо за пределы петли - раньше здесь voice.active стал бы false
    int16_t out[kTotalFrames] = {};
    const uint32_t produced   = engine::voice_render(voice, psram, out, kTotalFrames);
    CHECK_EQ(produced, kTotalFrames); // не остановился на границе петли
    CHECK(voice.active);
    // Играется как прямая петля (разворот - дело упаковщика).
    const std::vector<int16_t> oracle =
        linear_oracle(dpcm8_decoded(std::vector<int16_t>(native, native + kN)), true, kLoopStart, kLoopEnd, std::vector<uint32_t>(kTotalFrames, 0x10000u));
    for (uint32_t i = 0; i < kTotalFrames; ++i)
        CHECK_EQ(out[i], oracle[i]);

    memory::psram_destroy(psram);
}

// Linear-модель частоты (XM и современный IT, Song::frequency_model).
// Сверка на инварианте note=48 ("C-4")/finetune=0/relative_note=0 ->
// step==0x10000 (частота == c5_speed, та же точка отсчёта, что у
// Amiga-модели, см. voice.cpp), плюс октава вверх/вниз (степень двойки,
// точные целые значения без погрешности плавающей точки) и relative_note
// (XM: смещение сэмпла).
void test_voice_linear_frequency_model_octaves_and_relative_note() {
    std::printf("test_voice_linear_frequency_model_octaves_and_relative_note\n");

    const int16_t native[2] = {0, 0};
    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8(psram, native, 2);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = 2;
    sample.c5_speed          = engine::kSampleRateHz;

    engine::Voice voice;

    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Linear);
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x10000)); // референс

    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/60, FrequencyModel::Linear); // на октаву выше
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x20000));

    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/36, FrequencyModel::Linear); // на октаву ниже
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x8000));

    sample.relative_note = 12; // компенсирует note=36 обратно до эффективной 48
    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/36, FrequencyModel::Linear);
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x10000));

    memory::psram_destroy(psram);
}

// IT (флаг линейных слайдов, kQuirkItLinearC5Reference): C5Speed определён
// форматом относительно ноты C-5 (60, а не 48, как у XM). Найдено сверкой
// с OpenMPT soundlib/Snd_fx.cpp GetPeriodFromNote/GetFreqFromPeriod (см.
// voice.cpp kItLinearReferenceNote). Это объясняет ошибку питча на
// октаву, на которую жаловался пользователь (00009.it, флаг линейных
// слайдов включён).
void test_voice_linear_frequency_model_it_c5_reference_is_note_60() {
    std::printf("test_voice_linear_frequency_model_it_c5_reference_is_note_60\n");

    const int16_t native[2] = {0, 0};
    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8(psram, native, 2);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = 2;
    sample.c5_speed          = engine::kSampleRateHz;

    engine::Voice voice;
    const soundsinth::model::QuirkFlags it_linear = soundsinth::model::kQuirkItLinearC5Reference;

    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/60, FrequencyModel::Linear, 0, it_linear);
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x10000)); // референс - на C-5, а не C-4

    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/72, FrequencyModel::Linear, 0, it_linear); // на октаву выше
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x20000));

    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Linear, 0, it_linear); // на октаву ниже C-5
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x8000));

    // Без квирка (XM-путь) на той же note=48 - референс, а не октава ниже.
    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Linear);
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x10000));

    memory::psram_destroy(psram);
}

// SampleOffset (Effect::SampleOffset): voice_trigger(..., start_offset)
// декодирует и отбрасывает start_offset исходных сэмплов, прежде чем
// отдавать звук. Сверка с тем же независимым оракулом
// dpcm8::decode_block, что в тесте без смещения выше: первый выходной
// кадр должен совпасть с expected[start_offset], а не expected[0].
void test_voice_trigger_with_sample_offset_skips_native_samples() {
    std::printf("test_voice_trigger_with_sample_offset_skips_native_samples\n");

    constexpr uint32_t kN    = 8;
    const int16_t native[kN] = {100, 200, 300, 400, 500, 600, 700, 800};

    int8_t dpcm_bytes[kN];
    dpcm8::Dpcm8Checkpoint oracle_checkpoints[4];
    uint32_t oracle_checkpoint_count = 0;
    dpcm8::Dpcm8State encode_state;
    oracle_checkpoint_count = dpcm8::encode_block(native, kN, 0, encode_state, dpcm_bytes, oracle_checkpoints, 4);
    int16_t expected[kN];
    dpcm8::decode_block(dpcm_bytes, kN, dpcm8::Dpcm8State{}, expected);

    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8(psram, native, kN);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz; // step==0x10000 (1:1), см. тесты выше

    engine::Voice voice;
    constexpr uint32_t kOffset = 3;
    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Amiga, kOffset);
    CHECK(voice.active);

    int16_t out[kN] = {};
    // При step 1:1 тот же сдвиг "на кадр позади", что и в тесте без
    // смещения: produced == (kN-kOffset)-1, кадры совпадают с
    // expected[kOffset..kN-2].
    const uint32_t produced = engine::voice_render(voice, psram, out, kN);
    CHECK_EQ(produced, kN - kOffset - 1);
    for (uint32_t i = 0; i < produced; ++i) {
        CHECK_EQ(out[i], expected[kOffset + i]);
    }

    memory::psram_destroy(psram);
}

// SampleOffset через персистентные чекпоинты (voice_trigger(...,
// checkpoint_first_page)). Быстрый путь
// (dpcm8::locate_block, O(число страниц), без лишних
// decode_and_advance) должен дать тот же результат, что линейный
// decode-and-discard (см. тест выше); сверка с тем же независимым
// оракулом dpcm8::decode_block. Сэмпл больше одного блока
// (kN=600 > kCheckpointIntervalSamples*2), offset на границе второго блока
// (256, кратно kCheckpointIntervalSamples, как гарантирует
// effect_dispatch.cpp для реальных SampleOffset/HighOffset).
void test_voice_trigger_with_checkpoint_aligned_offset_matches_oracle() {
    std::printf("test_voice_trigger_with_checkpoint_aligned_offset_matches_oracle\n");

    constexpr uint32_t kN = 600;
    std::vector<int16_t> native(kN);
    for (uint32_t i = 0; i < kN; ++i)
        native[i] = static_cast<int16_t>(((i * 4099) % 30000) - 15000);

    std::vector<int8_t> dpcm_bytes(kN);
    dpcm8::Dpcm8Checkpoint oracle_checkpoints[8];
    uint32_t oracle_checkpoint_count = 0;
    dpcm8::Dpcm8State encode_state;
    oracle_checkpoint_count = dpcm8::encode_block(native.data(), kN, 0, encode_state, dpcm_bytes.data(), oracle_checkpoints, 8);
    std::vector<int16_t> expected(kN);
    dpcm8::decode_block(dpcm_bytes.data(), kN, dpcm8::Dpcm8State{}, expected.data());

    memory::PsramStore psram{};
    memory::psram_create(psram);
    const sample_pack::PackResult result = repack_into_psram_dpcm8_with_checkpoints(psram, native.data(), kN);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz; // step==0x10000 (1:1)

    engine::Voice voice;
    constexpr uint32_t kOffset = dpcm8::kCheckpointIntervalSamples; // граница второго блока
    engine::voice_trigger(voice, psram, sample, result.first_page, /*note=*/48, FrequencyModel::Amiga, kOffset,
                          /*quirks=*/0, result.checkpoint_first_page);
    CHECK(voice.active);

    std::vector<int16_t> out(kN);
    const uint32_t produced = engine::voice_render(voice, psram, out.data(), kN);
    CHECK_EQ(produced, kN - kOffset - 1); // тот же сдвиг "на кадр позади", см. тесты выше
    for (uint32_t i = 0; i < produced; ++i) {
        CHECK_EQ(out[i], expected[kOffset + i]);
    }

    memory::psram_destroy(psram);
}

// Тот же быстрый путь, но с петлёй, у которой loop_start лежит до
// смещения чекпоинта. voice_trigger должен отдельно поймать чекпоинт петли
// (см. .cpp: обычный проход decode_and_advance здесь перепрыгнут), иначе
// после первого достижения loop_end зацикливание сломается или выйдет за
// границы. Выход - против последовательного оракула от смещения.
void test_voice_trigger_checkpoint_offset_past_loop_start_still_loops_correctly() {
    std::printf("test_voice_trigger_checkpoint_offset_past_loop_start_still_loops_correctly\n");

    constexpr uint32_t kN = 600;
    std::vector<int16_t> native(kN);
    for (uint32_t i = 0; i < kN; ++i)
        native[i] = static_cast<int16_t>(((i * 4099) % 30000) - 15000);

    memory::PsramStore psram{};
    memory::psram_create(psram);
    const sample_pack::PackResult result = repack_into_psram_dpcm8_with_checkpoints(psram, native.data(), kN);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = kN;
    sample.loop_enabled      = true;
    sample.loop_start        = 100; // < kOffset ниже - start_offset перепрыгивает loop_start
    sample.loop_end          = 500;
    sample.c5_speed          = engine::kSampleRateHz;
    const uint32_t loop_len  = sample.loop_end - sample.loop_start;

    engine::Voice voice;
    constexpr uint32_t kOffset = dpcm8::kCheckpointIntervalSamples; // 256 > loop_start(100)
    engine::voice_trigger(voice, psram, sample, result.first_page, /*note=*/48, FrequencyModel::Amiga, kOffset,
                          /*quirks=*/0, result.checkpoint_first_page);
    CHECK(voice.active);
    CHECK(voice.loop_checkpoint_captured); // пойман отдельно, а не естественным проходом

    const uint32_t total_frames = sample.loop_end + 4 * loop_len;
    std::vector<int16_t> out(total_frames);
    const uint32_t produced = engine::voice_render(voice, psram, out.data(), total_frames);
    CHECK_EQ(produced, total_frames);
    CHECK(voice.active);
    const std::vector<int16_t> oracle =
        linear_oracle(dpcm8_decoded(native), true, sample.loop_start, sample.loop_end, std::vector<uint32_t>(total_frames, 0x10000u), kOffset);
    for (uint32_t i = 0; i < total_frames; ++i)
        CHECK_EQ(out[i], oracle[i]);

    memory::psram_destroy(psram);
}

// Прореженный сэмпл (decimated) хранится вдвое короче, а SampleOffset
// приходит в отсчётах исходного файла: смещение 6 обязано встать на
// отсчёт 3 хранимого потока, а смещение 12 при хранимой длине 8 - ещё не
// за концом (в исходных отсчётах сэмпл длиной 16).
void test_voice_trigger_decimated_sample_offset_is_halved() {
    std::printf("test_voice_trigger_decimated_sample_offset_is_halved\n");

    constexpr uint32_t kN    = 8;
    const int16_t native[kN] = {100, 200, 300, 400, 500, 600, 700, 800};

    int8_t dpcm_bytes[kN];
    dpcm8::Dpcm8Checkpoint oracle_checkpoints[4];
    dpcm8::Dpcm8State encode_state;
    dpcm8::encode_block(native, kN, 0, encode_state, dpcm_bytes, oracle_checkpoints, 4);
    int16_t expected[kN];
    dpcm8::decode_block(dpcm_bytes, kN, dpcm8::Dpcm8State{}, expected);

    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8(psram, native, kN);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding     = ResidentEncoding::Dpcm8;
    sample.length_samples        = kN;
    sample.source_length_samples = kN * 2;
    sample.decimated             = true;
    sample.c5_speed              = engine::kSampleRateHz;

    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Amiga, /*start_offset=*/6);
    CHECK(voice.active);
    int16_t out[kN]         = {};
    const uint32_t produced = engine::voice_render(voice, psram, out, kN);
    CHECK_EQ(produced, kN - 3 - 1);
    for (uint32_t i = 0; i < produced; ++i) {
        CHECK_EQ(out[i], expected[3 + i]);
    }

    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Amiga, /*start_offset=*/12);
    CHECK(voice.active);

    memory::psram_destroy(psram);
}

void test_voice_trigger_with_offset_past_end_stays_inactive() {
    std::printf("test_voice_trigger_with_offset_past_end_stays_inactive\n");

    constexpr uint32_t kN    = 8;
    const int16_t native[kN] = {100, 200, 300, 400, 500, 600, 700, 800};

    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8(psram, native, kN);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz;

    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, first_page, /*note=*/48, FrequencyModel::Amiga, /*start_offset=*/kN);
    CHECK(!voice.active); // offset == length_samples - играть нечего, та же трактовка, что пустой сэмпл

    memory::psram_destroy(psram);
}

// SampleOffset за концом петли: S3M заворачивает внутрь петли, MOD играет
// с loop_start; у битой петли заворота нет - нота не звучит и не падает.
void test_voice_trigger_offset_past_loop_end_by_format() {
    std::printf("test_voice_trigger_offset_past_loop_end_by_format\n");

    constexpr uint32_t kN = 16;
    int16_t native[kN];
    for (uint32_t i = 0; i < kN; ++i)
        native[i] = static_cast<int16_t>(i * 4);

    memory::PsramStore psram{};
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
    CHECK(packer.add_samples(native, kN));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Raw8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz;
    sample.loop_enabled      = true;
    sample.loop_start        = 4;
    sample.loop_end          = 12;

    auto first_value = [&](uint32_t offset, soundsinth::model::QuirkFlags quirks, bool* active) {
        engine::Voice voice;
        engine::voice_trigger(voice, psram, sample, result.first_page, /*note=*/48, FrequencyModel::Amiga, offset, quirks);
        *active        = voice.active;
        int16_t out[2] = {};
        if (voice.active) engine::voice_render(voice, psram, out, 2);
        return out[0];
    };

    bool active = false;
    // S3M: (23 - 4) % 8 + 4 = 7.
    CHECK_EQ(first_value(23, soundsinth::model::kQuirkS3mOffsetWrapInLoop, &active), native[7] * 256);
    CHECK(active);
    // MOD: с loop_start.
    CHECK_EQ(first_value(20, soundsinth::model::kQuirkModOffsetPastLoopEnd, &active), native[4] * 256);
    CHECK(active);
    // XM (без квирков): нота не звучит.
    first_value(20, 0, &active);
    CHECK(!active);
    // Битая петля: заворота нет, нота не звучит.
    sample.loop_start = 12;
    sample.loop_end   = 4;
    first_value(14, soundsinth::model::kQuirkS3mOffsetWrapInLoop, &active);
    CHECK(!active);

    memory::psram_destroy(psram);
}

// Raw8 - резидентная 8-битная шкала без декодера (см. voice.h/.cpp).
// Голос должен раскрывать прочитанный байт до полной 16-битной шкалы
// (x256) при каждом чтении, а произвольный доступ (start_offset) должен
// работать совсем без персистентных чекпоинтов (checkpoint_first_page по
// умолчанию kPageChainEnd), в отличие от Dpcm8, где они обязательны.
void test_voice_render_raw8_expands_to_full_scale_and_seeks_without_checkpoints() {
    std::printf("test_voice_render_raw8_expands_to_full_scale_and_seeks_without_checkpoints\n");

    constexpr uint32_t kN    = 6;
    const int16_t native[kN] = {10, -10, 20, -20, 30, -40}; // натуральная 8-битная шкала, см. sample_pack.h

    memory::PsramStore psram{};
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
    CHECK(packer.add_samples(native, kN));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    CHECK(result.checkpoint_first_page == memory::kPageChainEnd); // Raw8 не пишет чекпоинты

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Raw8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz; // step==0x10000 (1:1)

    engine::Voice voice;
    constexpr uint32_t kOffset = 2; // проверяет seek_to_sample без персистентных чекпоинтов (checkpoint_first_page не передан)
    engine::voice_trigger(voice, psram, sample, result.first_page, /*note=*/48, FrequencyModel::Amiga, kOffset);
    CHECK(voice.active);

    int16_t out[kN]         = {};
    const uint32_t produced = engine::voice_render(voice, psram, out, kN);
    CHECK_EQ(produced, kN - kOffset - 1); // тот же сдвиг "на кадр позади", что и у Dpcm8-тестов

    // Ожидается x256-расширение исходных 8-битных значений, начиная с
    // native[kOffset] (voice_render на первом фрейме отдаёт prev_sample, то
    // есть native[kOffset], см. комментарий про сдвиг выше).
    for (uint32_t i = 0; i < produced; ++i) {
        const int16_t expected = static_cast<int16_t>(static_cast<int32_t>(native[kOffset + i]) * 256);
        CHECK_EQ(out[i], expected);
    }

    memory::psram_destroy(psram);
}

// Raw8-прыжок (voice.cpp, порог n_steps>=3) заменяет последовательное
// декодирование пачки прямым пересчётом позиции по индексу. Самый
// рискованный случай - короткая зацикленная область при огромном шаге,
// когда за одну пачку проходит несколько витков петли. Оракул ниже -
// независимая буквальная копия старой последовательной семантики
// (побайтно, с тем же порядком prev=next/decode/wrap-check), без
// прыжкового кода: если пути расходятся, тест это поймает, а не
// продублирует один и тот же баг.
void test_voice_render_raw8_high_step_multi_wrap_loop_matches_sequential_oracle() {
    std::printf("test_voice_render_raw8_high_step_multi_wrap_loop_matches_sequential_oracle\n");

    constexpr uint32_t kN    = 8;
    const int16_t native[kN] = {10, 20, 30, 40, 50, 60, 70, 80}; // натуральная 8-битная шкала

    memory::PsramStore psram{};
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
    CHECK(packer.add_samples(native, kN));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Raw8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz;
    sample.loop_enabled      = true;
    sample.loop_start        = 2;
    sample.loop_end = 6; // петля длиной 4 - короче шага ниже, гарантирует несколько витков за одну пачку

    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, result.first_page, /*note=*/48, FrequencyModel::Amiga);
    CHECK(voice.active);

    // 10 исходных сэмплов на один выходной: при петле длиной 4 это два полных
    // витка плюс остаток за одну пачку, выше порога прыжка (>=3).
    voice.step = 10u << 16;

    constexpr uint32_t kFrames = 5;
    int16_t oracle_out[kFrames];
    {
        uint32_t decoded = 2; // см. voice_trigger: prev=native[0],next=native[1],decoded_count=2 после триггера
        uint32_t frac    = 0;
        int16_t prev     = static_cast<int16_t>(native[0] * 256);
        int16_t next     = static_cast<int16_t>(native[1] * 256);
        for (uint32_t f = 0; f < kFrames; ++f) {
            const int32_t delta  = int32_t(next) - int32_t(prev);
            oracle_out[f]        = static_cast<int16_t>(int32_t(prev) + static_cast<int32_t>((int64_t(delta) * int64_t(frac)) >> 16));
            frac                += 10u << 16;
            while (frac >= 0x10000u) {
                frac -= 0x10000u;
                prev  = next;
                if (decoded >= sample.loop_end) decoded = sample.loop_start;
                next = static_cast<int16_t>(native[decoded] * 256);
                ++decoded;
            }
        }
    }

    int16_t actual_out[kFrames] = {};
    const uint32_t produced     = engine::voice_render(voice, psram, actual_out, kFrames);
    CHECK_EQ(produced, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        CHECK_EQ(actual_out[i], oracle_out[i]);
    }

    memory::psram_destroy(psram);
}

// Смещение на конце петли (loop_end - 1) и за концом сэмпла (старые эффекты
// IT берут последний отсчёт): после триггера следующий отсчёт - с
// loop_start, дальше рендер совпадает с последовательным оракулом на любом
// шаге - и у прыжка Raw8, и у пропуска Dpcm8.
void test_voice_trigger_offset_at_loop_end_wraps_like_sequential() {
    std::printf("test_voice_trigger_offset_at_loop_end_wraps_like_sequential\n");
    constexpr uint32_t kN  = 12;
    constexpr uint32_t kLs = 4, kLe = 8;
    int16_t native8[kN], native16[kN];
    for (uint32_t i = 0; i < kN; ++i) {
        native8[i]  = static_cast<int16_t>(5 + 7 * i);
        native16[i] = static_cast<int16_t>(native8[i] * 256);
    }
    struct Case {
        uint32_t offset;
        soundsinth::model::QuirkFlags quirks;
    };
    const Case cases[] = {
        {kLe - 1, 0},
        {kN + 5, soundsinth::model::kQuirkItOffsetPastEndRestarts | soundsinth::model::kQuirkItOldEffects},
    };
    for (const ResidentEncoding enc : {ResidentEncoding::Raw8, ResidentEncoding::Dpcm8}) {
        memory::PsramStore psram{};
        memory::psram_create(psram);
        sample_pack::SamplePacker packer(psram, enc);
        CHECK(packer.add_samples(enc == ResidentEncoding::Raw8 ? native8 : native16, kN));
        const sample_pack::PackResult r = packer.finish();
        CHECK(r.ok);
        // Значения, как их отдаёт голос: Raw8 раскрыт до 16 бит, Dpcm8 - распакован.
        int16_t val[kN];
        dpcm8::Dpcm8State st;
        for (uint32_t i = 0; i < kN; ++i) {
            if (enc == ResidentEncoding::Raw8) {
                val[i] = native16[i];
            } else {
                const uint16_t page = memory::psram_page_advance(psram, r.first_page, i / memory::kPsramPageBytes);
                val[i]              = dpcm8::decode_delta(memory::psram_page_ptr(psram, page)[i % memory::kPsramPageBytes], st);
            }
        }
        soundsinth::model::SampleDescriptor sd;
        sd.resident_encoding = enc;
        sd.length_samples    = kN;
        sd.c5_speed          = engine::kSampleRateHz;
        sd.loop_enabled      = true;
        sd.loop_start        = kLs;
        sd.loop_end          = kLe;
        for (const Case& c : cases) {
            for (const uint32_t step : {1u, 3u, 7u}) {
                engine::Voice v;
                engine::voice_trigger(v, psram, sd, r.first_page, 48, FrequencyModel::Amiga, c.offset, c.quirks, r.checkpoint_first_page);
                CHECK(v.active);
                v.step = step << 16;
                // Оракул: первый отсчёт по смещению, дальше заворот перед каждым
                // следующим, как в рендере.
                const uint32_t start = c.offset >= kN ? kN - 1 : c.offset;
                uint32_t decoded     = start + 1;
                int16_t prev         = val[start];
                if (decoded >= kLe) decoded = kLs;
                int16_t next               = val[decoded++];
                uint32_t frac              = 0;
                constexpr uint32_t kFrames = 12;
                int16_t want[kFrames], got[kFrames] = {};
                for (uint32_t f = 0; f < kFrames; ++f) {
                    const int32_t delta  = int32_t(next) - int32_t(prev);
                    want[f]              = static_cast<int16_t>(int32_t(prev) + static_cast<int32_t>((int64_t(delta) * frac) >> 16));
                    frac                += step << 16;
                    while (frac >= 0x10000u) {
                        frac -= 0x10000u;
                        prev  = next;
                        if (decoded >= kLe) decoded = kLs;
                        next = val[decoded++];
                    }
                }
                CHECK_EQ(engine::voice_render(v, psram, got, kFrames), kFrames);
                for (uint32_t f = 0; f < kFrames; ++f)
                    CHECK_EQ(got[f], want[f]);
            }
        }
        memory::psram_destroy(psram);
    }
}

// Прыжок Raw8 перескакивает loop_start (2 -> 5), точка петли не поставлена;
// затем шаг падает ниже порога прыжка, и последовательный путь доходит до
// loop_end. Точку ставит заворот (direct_loop_checkpoint), без неё - чтение
// kPageChainEnd и падение (final_fantasy.xm на плате и на ПК).
void test_voice_render_raw8_jump_skips_loop_start_then_sequential_wrap_matches_oracle() {
    std::printf("test_voice_render_raw8_jump_skips_loop_start_then_sequential_wrap_matches_oracle\n");

    constexpr uint32_t kN    = 10;
    const int16_t native[kN] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

    memory::PsramStore psram{};
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
    CHECK(packer.add_samples(native, kN));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Raw8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz;
    sample.loop_enabled      = true;
    sample.loop_start        = 4;
    sample.loop_end          = 8;

    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, result.first_page, /*note=*/48, FrequencyModel::Amiga);
    CHECK(voice.active);
    CHECK(!voice.loop_checkpoint_captured); // ещё не входили в петлю

    // Независимый оракул: тот же побайтный последовательный проход, что и в
    // предыдущем тесте, на все 5 кадров (1 на высоком шаге + 4 на низком),
    // без кода прыжка и чекпоинта.
    const uint32_t steps_q16[5] = {3u << 16, 1u << 16, 1u << 16, 1u << 16, 1u << 16};
    int16_t oracle_out[5];
    {
        uint32_t decoded = 2;
        uint32_t frac    = 0;
        int16_t prev     = static_cast<int16_t>(native[0] * 256);
        int16_t next     = static_cast<int16_t>(native[1] * 256);
        for (uint32_t f = 0; f < 5; ++f) {
            const int32_t delta  = int32_t(next) - int32_t(prev);
            oracle_out[f]        = static_cast<int16_t>(int32_t(prev) + static_cast<int32_t>((int64_t(delta) * int64_t(frac)) >> 16));
            frac                += steps_q16[f];
            while (frac >= 0x10000u) {
                frac -= 0x10000u;
                prev  = next;
                if (decoded >= sample.loop_end) decoded = sample.loop_start;
                next = static_cast<int16_t>(native[decoded] * 256);
                ++decoded;
            }
        }
    }

    // Кадр 0 - высокий шаг, прыжок 2->5 (минует loop_start=4 без
    // decode_and_advance на нём).
    voice.step            = 3u << 16;
    int16_t actual_out[5] = {};
    CHECK_EQ(engine::voice_render(voice, psram, actual_out, 1), 1u);
    CHECK(!voice.loop_checkpoint_captured); // прыжок не захватил чекпоинт - это и есть условие бага

    // Кадры 1..4 - низкий шаг, обычный последовательный путь; кадр,
    // на котором decoded_count достигает loop_end(8), раньше падал
    // через невалидный чекпоинт.
    voice.step = 1u << 16;
    CHECK_EQ(engine::voice_render(voice, psram, actual_out + 1, 4), 4u);

    for (uint32_t i = 0; i < 5; ++i) {
        CHECK_EQ(actual_out[i], oracle_out[i]);
    }

    memory::psram_destroy(psram);
}

// Заворот и прыжок прямых кодеков на петле далеко от начала сэмпла: до
// loop_start десятки страниц, петля короткая. Точка петли ставится один раз
// (direct_loop_checkpoint), витки и прыжки через конец петли идут от неё.
// Оракул - индекс в исходном массиве с тем же порядком prev/next/заворот, без
// кода прыжка и точек петли.
void check_direct_far_loop(ResidentEncoding enc, uint32_t step_q16) {
    constexpr uint32_t kN = 40000;
    std::vector<int16_t> native(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        // Без периода 256: сдвиг позиции на страницу не должен давать то же значение.
        const uint32_t h = i * 2654435761u;
        native[i]        = enc == ResidentEncoding::Raw8 ? static_cast<int16_t>(static_cast<int32_t>(h >> 24) - 128) : static_cast<int16_t>(h >> 16);
    }
    auto value = [&](uint32_t i) -> int16_t { return enc == ResidentEncoding::Raw8 ? static_cast<int16_t>(native[i] * 256) : native[i]; };

    memory::PsramStore psram{};
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, enc);
    CHECK(packer.add_samples(native.data(), kN));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = enc;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz;
    sample.loop_enabled      = true;
    sample.loop_start        = 39000;
    sample.loop_end          = 39037; // 37 - короче и не кратно шагам ниже: витки ложатся в разные места

    engine::voice_reset_debug_counters();
    memory::g_page_next_walks = 0;
    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, result.first_page, /*note=*/48, FrequencyModel::Amiga);
    CHECK(voice.active);
    voice.step = step_q16;

    // До петли и ещё около двух тысяч витков.
    const uint32_t frames = static_cast<uint32_t>((uint64_t(sample.loop_end + 37u * 2000u) << 16) / step_q16);
    std::vector<int16_t> actual(frames), oracle(frames);
    for (uint32_t done = 0; done < frames;) {
        const uint32_t n = frames - done < 256u ? frames - done : 256u;
        CHECK_EQ(engine::voice_render(voice, psram, actual.data() + done, n), n);
        done += n;
    }
    {
        uint32_t decoded = 2, frac = 0;
        int16_t prev = value(0), next = value(1);
        for (uint32_t f = 0; f < frames; ++f) {
            const int32_t delta  = int32_t(next) - int32_t(prev);
            oracle[f]            = static_cast<int16_t>(int32_t(prev) + static_cast<int32_t>((int64_t(delta) * int64_t(frac)) >> 16));
            frac                += step_q16;
            while (frac >= 0x10000u) {
                frac -= 0x10000u;
                prev  = next;
                if (decoded >= sample.loop_end) decoded = sample.loop_start;
                next = value(decoded);
                ++decoded;
            }
        }
    }
    uint32_t mismatches = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        if (actual[f] != oracle[f]) ++mismatches;
    }
    CHECK_EQ(mismatches, 0u);
    CHECK(voice.loop_checkpoint_captured);
    // Цена поиска: страницы до петли проходятся раз последовательно и раз при
    // постановке точки, каждый из трёх поисков прыжка пересекает не больше
    // одной границы. Поиск от first_page за заворотом дал бы десятки обходов
    // на поиск - порядка миллиона на прогон.
    const uint32_t stride        = soundsinth::model::resident_bytes_per_sample(enc);
    const uint32_t pages_to_loop = sample.loop_start * stride / memory::kPsramPageBytes + 1;
    CHECK(memory::g_page_next_walks <= 2 * pages_to_loop + 3 * engine::voice_debug_counters().direct_jumps);
    memory::psram_destroy(psram);
}

void test_voice_render_direct_far_loop_matches_oracle() {
    std::printf("test_voice_render_direct_far_loop_matches_oracle\n");
    for (const ResidentEncoding enc : {ResidentEncoding::Raw8, ResidentEncoding::Raw16}) {
        check_direct_far_loop(enc, 1u << 16);           // последовательный путь, заворот на loop_end
        check_direct_far_loop(enc, 4u << 16);           // прыжок, цели за концом петли
        check_direct_far_loop(enc, (4u << 16) + 24248); // прыжок с дробью шага
    }
}

// --- Эрмитова интерполяция (kQuirkHermiteInterpolation) ---

// Независимый последовательный проход окна из четырёх отсчётов: без
// прыжков, пропусков и контрольных точек, та же формула, что в voice.cpp.
// native - уже раскрытые до 16 бит значения; после триггера окно
// (0, 0, native[0], native[1]), decoded = 2.
std::vector<int16_t> hermite_oracle(const std::vector<int16_t>& native, bool loop, uint32_t loop_start, uint32_t loop_end, const std::vector<uint32_t>& steps) {
    std::vector<int16_t> out;
    int32_t xm1 = 0, x0 = 0, x1 = native[0], x2 = native.size() > 1 ? native[1] : native[0];
    uint32_t decoded = 2, frac = 0;
    const uint32_t end = loop ? loop_end : static_cast<uint32_t>(native.size());
    for (uint32_t step : steps) {
        const int32_t c2 = x1 - xm1, v2 = 2 * (x0 - x1), w2 = c2 + v2, a2 = w2 + v2 + (x2 - x0), b2 = w2 + a2;
        const int64_t t = frac;
        int64_t acc     = (int64_t(a2) * t) >> 16;
        acc             = ((acc - b2) * t) >> 16;
        acc             = ((acc + c2) * t) >> 16;
        int32_t y       = x0 + static_cast<int32_t>(acc >> 1);
        if (y > 32767) y = 32767;
        if (y < -32768) y = -32768;
        out.push_back(static_cast<int16_t>(y));
        frac += step;
        while (frac >= 0x10000u) {
            frac -= 0x10000u;
            xm1   = x0;
            x0    = x1;
            x1    = x2;
            if (decoded >= end) {
                if (!loop) return out;
                decoded = loop_start;
            }
            x2 = native[decoded++];
        }
    }
    return out;
}

void test_voice_hermite_passes_through_samples_with_one_sample_delay() {
    std::printf("test_voice_hermite_passes_through_samples_with_one_sample_delay\n");
    constexpr uint32_t kN = 16;
    int16_t native[kN];
    for (uint32_t i = 0; i < kN; ++i)
        native[i] = static_cast<int16_t>((i * 37) % 23 * 10 - 110); // 8-битная шкала

    memory::PsramStore psram{};
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
    CHECK(packer.add_samples(native, kN));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);

    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Raw8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz;
    engine::Voice voice;
    engine::voice_trigger(voice, psram, sample, result.first_page, /*note=*/48, FrequencyModel::Amiga, 0, soundsinth::model::kQuirkHermiteInterpolation);
    CHECK(voice.hermite);
    CHECK_EQ(voice.step, static_cast<uint32_t>(0x10000));

    int16_t out[kN]         = {};
    const uint32_t produced = engine::voice_render(voice, psram, out, kN);
    // Задержка на один отсчёт: out[0] - ноль перед сэмплом, out[i] = native[i-1].
    // Незацикленный сэмпл кончается на том же шаге, что у линейной: kN-1 кадров.
    CHECK_EQ(produced, kN - 1);
    CHECK_EQ(out[0], 0);
    for (uint32_t i = 1; i < produced; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>(native[i - 1] * 256));
    memory::psram_destroy(psram);
}

// Прыжок Raw8 и пропуск Dpcm8 обязаны оставлять окну четыре последних
// отсчёта: шаги до 10 родных на выходной, петля короче шага.
void test_voice_hermite_fast_paths_match_sequential_oracle() {
    std::printf("test_voice_hermite_fast_paths_match_sequential_oracle\n");
    const std::vector<uint32_t> steps = {0x0B000, 0x14000, 0x30000, 0x50000, 0x5C000, 0xA0000, 0x06000, 0x71000, 0x10000, 0x98000, 0x28000, 0xC4000};
    std::vector<uint32_t> many;
    for (int k = 0; k < 20; ++k)
        many.insert(many.end(), steps.begin(), steps.end());

    // Raw8, зацикленный: прыжок через петлю, в том числе с несколькими витками.
    {
        constexpr uint32_t kN = 40;
        std::vector<int16_t> native8(kN), native16(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            native8[i]  = static_cast<int16_t>((i * 53) % 41 * 6 - 120);
            native16[i] = static_cast<int16_t>(native8[i] * 256);
        }
        memory::PsramStore psram{};
        memory::psram_create(psram);
        sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
        CHECK(packer.add_samples(native8.data(), kN));
        const sample_pack::PackResult result = packer.finish();
        CHECK(result.ok);
        soundsinth::model::SampleDescriptor sample;
        sample.resident_encoding = ResidentEncoding::Raw8;
        sample.length_samples    = kN;
        sample.c5_speed          = engine::kSampleRateHz;
        sample.loop_enabled      = true;
        sample.loop_start        = 9;
        sample.loop_end          = 16; // петля 7 - короче шага 10
        engine::voice_reset_debug_counters();
        engine::Voice voice;
        engine::voice_trigger(voice, psram, sample, result.first_page, 48, FrequencyModel::Amiga, 0, soundsinth::model::kQuirkHermiteInterpolation);
        const std::vector<int16_t> oracle = hermite_oracle(native16, true, 9, 16, many);
        std::vector<int16_t> actual;
        for (uint32_t s : many) {
            voice.step = s;
            int16_t y  = 0;
            CHECK_EQ(engine::voice_render(voice, psram, &y, 1), 1u);
            actual.push_back(y);
        }
        CHECK_EQ(actual.size(), oracle.size());
        for (size_t i = 0; i < actual.size() && i < oracle.size(); ++i)
            CHECK_EQ(actual[i], oracle[i]);
        CHECK(engine::voice_debug_counters().direct_jumps > 0);
        memory::psram_destroy(psram);
    }

    // Dpcm8, длинный незацикленный: пропуск тесной петлёй.
    {
        constexpr uint32_t kN = 3000;
        std::vector<int16_t> native(kN);
        for (uint32_t i = 0; i < kN; ++i)
            native[i] = static_cast<int16_t>(12000.0 * std::sin(i * 0.37) + 3000.0 * std::sin(i * 2.9));
        memory::PsramStore psram{};
        memory::psram_create(psram);
        const uint16_t first_page = repack_into_psram_dpcm8_with_checkpoints(psram, native.data(), kN).first_page;
        // Оракулу - то, что реально лежит в PSRAM: кодек с потерями.
        std::vector<int8_t> bytes(kN);
        std::vector<dpcm8::Dpcm8Checkpoint> cps(kN / dpcm8::kCheckpointIntervalSamples + 2);
        dpcm8::Dpcm8State st{};
        dpcm8::encode_block(native.data(), kN, 0, st, bytes.data(), cps.data(), static_cast<uint32_t>(cps.size()));
        std::vector<int16_t> decoded(kN);
        dpcm8::decode_block(bytes.data(), kN, dpcm8::Dpcm8State{}, decoded.data());

        soundsinth::model::SampleDescriptor sample;
        sample.resident_encoding = ResidentEncoding::Dpcm8;
        sample.length_samples    = kN;
        sample.c5_speed          = engine::kSampleRateHz;
        engine::voice_reset_debug_counters();
        engine::Voice voice;
        engine::voice_trigger(voice, psram, sample, first_page, 48, FrequencyModel::Amiga, 0, soundsinth::model::kQuirkHermiteInterpolation);
        const std::vector<int16_t> oracle = hermite_oracle(decoded, false, 0, 0, many);
        std::vector<int16_t> actual;
        for (uint32_t s : many) {
            if (!voice.active) break;
            voice.step = s;
            int16_t y  = 0;
            if (engine::voice_render(voice, psram, &y, 1) == 1u) actual.push_back(y);
        }
        CHECK_EQ(actual.size(), oracle.size());
        for (size_t i = 0; i < actual.size() && i < oracle.size(); ++i)
            CHECK_EQ(actual[i], oracle[i]);
        CHECK(engine::voice_debug_counters().discarded_dpcm8 > 0); // пропуск действительно сработал
        memory::psram_destroy(psram);
    }
}

// Ради чего всё: синус на 1/8 частоты сэмпла, проигранный вдвое медленнее
// (сэмпл 22050 Гц на выходе 44100). Зеркало - на частоте сэмпла минус
// частота синуса. Эрмитова давит его сильнее линейной, сам тон не трогает.
void test_voice_hermite_suppresses_images() {
    std::printf("test_voice_hermite_suppresses_images\n");
    constexpr uint32_t kN = 4096; // 512 периодов по 8 отсчётов - петля без стыка
    std::vector<int16_t> native(kN);
    for (uint32_t i = 0; i < kN; ++i)
        native[i] = static_cast<int16_t>(16000.0 * std::sin(2.0 * 3.14159265358979 * i / 8.0));
    memory::PsramStore psram{};
    memory::psram_create(psram);
    const uint16_t first_page = repack_into_psram_dpcm8_with_checkpoints(psram, native.data(), kN).first_page;
    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = kN;
    sample.c5_speed          = engine::kSampleRateHz / 2; // шаг 0.5
    sample.loop_enabled      = true;
    sample.loop_start        = 0;
    sample.loop_end          = kN;

    auto level_db = [](const std::vector<int16_t>& x, double hz) {
        double re = 0, im = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double w  = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / x.size());
            re             += x[i] * w * std::cos(2.0 * 3.14159265358979 * hz * i / engine::kSampleRateHz);
            im             += x[i] * w * std::sin(2.0 * 3.14159265358979 * hz * i / engine::kSampleRateHz);
        }
        return 10.0 * std::log10(re * re + im * im + 1e-9);
    };
    const double tone_hz  = engine::kSampleRateHz / 2.0 / 8.0;     // 2756.25
    const double image_hz = engine::kSampleRateHz / 2.0 - tone_hz; // 19293.75
    double tone[2], image[2];
    for (int h = 0; h < 2; ++h) {
        engine::Voice voice;
        engine::voice_trigger(voice, psram, sample, first_page, 48, FrequencyModel::Amiga, 0, h ? soundsinth::model::kQuirkHermiteInterpolation : 0);
        CHECK_EQ(voice.step, static_cast<uint32_t>(0x8000));
        std::vector<int16_t> out(8192);
        CHECK_EQ(engine::voice_render(voice, psram, out.data(), 8192), 8192u);
        tone[h]  = level_db(out, tone_hz);
        image[h] = level_db(out, image_hz);
    }
    const double rel_lin = image[0] - tone[0], rel_her = image[1] - tone[1];
    std::printf("  mirror against the tone: linear %.1f dB, hermite %.1f dB; tone %.2f dB of difference\n", rel_lin, rel_her, tone[1] - tone[0]);
    CHECK(rel_her < rel_lin - 10.0);
    CHECK(std::fabs(tone[1] - tone[0]) < 0.5);
    memory::psram_destroy(psram);
}

// Пропуск Dpcm8 тесной петлёй на зацикленном сэмпле: петля не с начала и не
// кратна странице, шаг 0.5..12 родных на выходной, обе интерполяции.
void test_voice_dpcm8_loop_high_step_matches_oracle() {
    std::printf("test_voice_dpcm8_loop_high_step_matches_oracle\n");
    constexpr uint32_t kN = 3000, kLs = 1234, kLe = 2345;
    std::vector<int16_t> native(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        native[i] = static_cast<int16_t>(12000.0 * std::sin(i * 0.37) + 3000.0 * std::sin(i * 2.9));
    }
    const std::vector<int16_t> decoded = dpcm8_decoded(native);
    std::vector<uint32_t> steps;
    uint32_t r = 12345;
    for (int i = 0; i < 20000; ++i) {
        r = r * 1103515245u + 12345u;
        steps.push_back(0x8000u + (r >> 8) % 0xB8000u);
    }
    memory::PsramStore psram{};
    memory::psram_create(psram);
    const sample_pack::PackResult pr = repack_into_psram_dpcm8_with_checkpoints(psram, native.data(), kN);
    soundsinth::model::SampleDescriptor sample;
    sample.resident_encoding = ResidentEncoding::Dpcm8;
    sample.length_samples    = kN;
    sample.loop_enabled      = true;
    sample.loop_start        = kLs;
    sample.loop_end          = kLe;
    sample.c5_speed          = engine::kSampleRateHz;
    for (const bool hermite : {false, true}) {
        engine::voice_reset_debug_counters();
        engine::Voice voice;
        engine::voice_trigger(voice, psram, sample, pr.first_page, 48, FrequencyModel::Amiga, 0, hermite ? soundsinth::model::kQuirkHermiteInterpolation : 0,
                              pr.checkpoint_first_page);
        const std::vector<int16_t> oracle = hermite ? hermite_oracle(decoded, true, kLs, kLe, steps) : linear_oracle(decoded, true, kLs, kLe, steps);
        uint32_t mismatches               = 0;
        for (size_t i = 0; i < steps.size(); ++i) {
            voice.step = steps[i];
            int16_t y  = 0;
            CHECK_EQ(engine::voice_render(voice, psram, &y, 1), 1u);
            if (y != oracle[i]) ++mismatches;
        }
        CHECK_EQ(mismatches, 0u);
        CHECK(engine::voice_debug_counters().discarded_dpcm8 > 0); // пропуск сработал
    }
    memory::psram_destroy(psram);
}

// Смещение за концом по квиркам IT, битые границы петли, смещение за
// loop_start у прямых кодеков (точку петли ставит voice_trigger), прореженный
// сэмпл с квирком.
void test_voice_trigger_offset_quirks() {
    std::printf("test_voice_trigger_offset_quirks\n");
    using soundsinth::model::kQuirkItOffsetPastEndRestarts;
    using soundsinth::model::kQuirkItOldEffects;
    memory::PsramStore psram{};
    memory::psram_create(psram);

    // Сэмпл в 20 отсчётов и Oxx до 0xFE, как пишут музыку на IT.
    {
        constexpr uint32_t kN = 20;
        int16_t native[kN];
        for (uint32_t i = 0; i < kN; ++i)
            native[i] = static_cast<int16_t>(3 * i - 25);
        sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
        CHECK(packer.add_samples(native, kN));
        const sample_pack::PackResult r = packer.finish();
        CHECK(r.ok);
        soundsinth::model::SampleDescriptor sd;
        sd.resident_encoding = ResidentEncoding::Raw8;
        sd.length_samples    = kN;
        sd.c5_speed          = engine::kSampleRateHz;
        engine::Voice v;
        int16_t out[4] = {};
        engine::voice_trigger(v, psram, sd, r.first_page, 48, FrequencyModel::Amiga, 65024);
        CHECK(!v.active);
        engine::voice_trigger(v, psram, sd, r.first_page, 48, FrequencyModel::Amiga, 65024, kQuirkItOffsetPastEndRestarts);
        CHECK(v.active);
        CHECK_EQ(engine::voice_render(v, psram, out, 1), 1u);
        CHECK_EQ(out[0], static_cast<int16_t>(native[0] * 256));
        engine::voice_trigger(v, psram, sd, r.first_page, 48, FrequencyModel::Amiga, 65024, kQuirkItOffsetPastEndRestarts | kQuirkItOldEffects);
        CHECK(v.active);
        CHECK_EQ(v.prev_sample, static_cast<int16_t>(native[kN - 1] * 256));
        CHECK_EQ(engine::voice_render(v, psram, out, 4), 1u); // последний отсчёт и конец
        CHECK(!v.active);

        // Прореженный: хранится 10 из 20, смещение 30 - за концом, с квирком - с начала.
        sd.length_samples        = kN / 2;
        sd.source_length_samples = kN;
        sd.decimated             = true;
        engine::voice_trigger(v, psram, sd, r.first_page, 48, FrequencyModel::Amiga, 30, kQuirkItOffsetPastEndRestarts);
        CHECK(v.active);
        CHECK_EQ(engine::voice_render(v, psram, out, 1), 1u);
        CHECK_EQ(out[0], static_cast<int16_t>(native[0] * 256));
    }

    // Битые границы петли - без петли, голос доходит до конца сэмпла.
    {
        constexpr uint32_t kN = 100;
        std::vector<int16_t> native(kN);
        for (uint32_t i = 0; i < kN; ++i)
            native[i] = static_cast<int16_t>((i * 7) % 90 - 45);
        sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
        CHECK(packer.add_samples(native.data(), kN));
        const sample_pack::PackResult r = packer.finish();
        CHECK(r.ok);
        soundsinth::model::SampleDescriptor sd;
        sd.resident_encoding       = ResidentEncoding::Raw8;
        sd.length_samples          = kN;
        sd.c5_speed                = engine::kSampleRateHz;
        sd.loop_enabled            = true;
        const uint32_t bounds[][2] = {{50, 40}, {10, 120}};
        for (const auto& b : bounds) {
            sd.loop_start = b[0];
            sd.loop_end   = b[1];
            engine::Voice v;
            engine::voice_trigger(v, psram, sd, r.first_page, 48, FrequencyModel::Amiga);
            CHECK(v.active);
            CHECK(!v.loop_enabled);
            std::vector<int16_t> out(2 * kN);
            CHECK_EQ(engine::voice_render(v, psram, out.data(), 2 * kN), kN - 1);
            CHECK(!v.active);
        }
    }

    // Смещение за loop_start у прямых кодеков: точку петли ставит первый
    // заворот или прыжок; шаг 1 - заворот, шаг 3 - прыжок.
    for (const ResidentEncoding enc : {ResidentEncoding::Raw8, ResidentEncoding::Raw16}) {
        constexpr uint32_t kN = 5000, kLs = 3000, kLe = 3100, kOffset = 3050;
        std::vector<int16_t> native(kN), value(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            const uint32_t h = i * 2654435761u;
            native[i]        = enc == ResidentEncoding::Raw8 ? static_cast<int16_t>(static_cast<int32_t>(h >> 24) - 128) : static_cast<int16_t>(h >> 16);
            value[i]         = enc == ResidentEncoding::Raw8 ? static_cast<int16_t>(native[i] * 256) : native[i];
        }
        sample_pack::SamplePacker packer(psram, enc);
        CHECK(packer.add_samples(native.data(), kN));
        const sample_pack::PackResult r = packer.finish();
        CHECK(r.ok);
        soundsinth::model::SampleDescriptor sd;
        sd.resident_encoding = enc;
        sd.length_samples    = kN;
        sd.c5_speed          = engine::kSampleRateHz;
        sd.loop_enabled      = true;
        sd.loop_start        = kLs;
        sd.loop_end          = kLe;
        for (const uint32_t step : {1u << 16, 3u << 16}) {
            engine::Voice v;
            engine::voice_trigger(v, psram, sd, r.first_page, 48, FrequencyModel::Amiga, kOffset);
            CHECK(v.active);
            v.step                     = step;
            constexpr uint32_t kFrames = 1000;
            std::vector<int16_t> out(kFrames);
            CHECK_EQ(engine::voice_render(v, psram, out.data(), kFrames), kFrames);
            const std::vector<int16_t> oracle = linear_oracle(value, true, kLs, kLe, std::vector<uint32_t>(kFrames, step), kOffset);
            uint32_t mismatches               = 0;
            for (uint32_t f = 0; f < kFrames; ++f)
                mismatches += out[f] != oracle[f];
            CHECK_EQ(mismatches, 0u);
        }
    }
    memory::psram_destroy(psram);
}

// Потолок шага 0xFFFF0000: позиция питча на 40 октав выше даёт потолок и
// счётчик; прямые кодеки на нём совпадают с оракулом, Dpcm8 без петли гаснет.
void test_voice_step_ceiling() {
    std::printf("test_voice_step_ceiling\n");
    constexpr uint32_t kN = 200000, kLs = 1000, kLe = 150001;
    for (const ResidentEncoding enc : {ResidentEncoding::Raw8, ResidentEncoding::Raw16, ResidentEncoding::Dpcm8}) {
        std::vector<int16_t> native(kN), value(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            const uint32_t h = i * 2654435761u;
            native[i]        = enc == ResidentEncoding::Raw8 ? static_cast<int16_t>(static_cast<int32_t>(h >> 24) - 128) : static_cast<int16_t>(h >> 16);
            value[i]         = enc == ResidentEncoding::Raw8 ? static_cast<int16_t>(native[i] * 256) : native[i];
        }
        memory::PsramStore psram{};
        memory::psram_create(psram);
        sample_pack::SamplePacker packer(psram, enc);
        CHECK(packer.add_samples(native.data(), kN));
        const sample_pack::PackResult r = packer.finish();
        CHECK(r.ok);
        soundsinth::model::SampleDescriptor sd;
        sd.resident_encoding = enc;
        sd.length_samples    = kN;
        sd.c5_speed          = engine::kSampleRateHz;
        const bool loop      = enc != ResidentEncoding::Dpcm8;
        sd.loop_enabled      = loop;
        sd.loop_start        = loop ? kLs : 0;
        sd.loop_end          = loop ? kLe : 0;
        engine::voice_reset_debug_counters();
        engine::Voice v;
        engine::voice_trigger(v, psram, sd, r.first_page, 48, FrequencyModel::Linear, 0, 0, r.checkpoint_first_page);
        CHECK(v.active);
        engine::voice_recompute_linear_step(v, 64 * 12 * 40, sd.c5_speed);
        CHECK_EQ(v.step, 0xFFFF0000u);
        CHECK_EQ(engine::voice_debug_counters().step_clamps, 1u);
        constexpr uint32_t kFrames = 300;
        std::vector<int16_t> out(kFrames);
        const uint32_t produced = engine::voice_render(v, psram, out.data(), kFrames);
        if (loop) {
            CHECK_EQ(produced, kFrames);
            const std::vector<int16_t> oracle = linear_oracle(value, true, kLs, kLe, std::vector<uint32_t>(kFrames, v.step));
            uint32_t mismatches               = 0;
            for (uint32_t f = 0; f < kFrames; ++f)
                mismatches += out[f] != oracle[f];
            CHECK_EQ(mismatches, 0u);
            CHECK(engine::voice_debug_counters().direct_jumps > 0);
        } else {
            CHECK(produced < kFrames); // конец сэмпла, а не выход за цепочку
            CHECK(!v.active);
        }
        memory::psram_destroy(psram);
    }
}

} // namespace

void run_voice_tests() {
    test_voice_trigger_offset_at_loop_end_wraps_like_sequential();
    test_voice_hermite_passes_through_samples_with_one_sample_delay();
    test_voice_hermite_fast_paths_match_sequential_oracle();
    test_voice_hermite_suppresses_images();
    test_voice_render_matches_independent_dpcm8_decode_1to1();
    test_voice_trigger_empty_sample_stays_inactive();
    test_voice_loop_repeats_via_checkpoint();
    test_voice_bidirectional_loop_plays_forward_instead_of_stopping();
    test_voice_linear_frequency_model_octaves_and_relative_note();
    test_voice_linear_frequency_model_it_c5_reference_is_note_60();
    test_voice_trigger_with_sample_offset_skips_native_samples();
    test_voice_trigger_with_checkpoint_aligned_offset_matches_oracle();
    test_voice_trigger_checkpoint_offset_past_loop_start_still_loops_correctly();
    test_voice_trigger_with_offset_past_end_stays_inactive();
    test_voice_trigger_offset_past_loop_end_by_format();
    test_voice_trigger_decimated_sample_offset_is_halved();
    test_voice_render_raw8_expands_to_full_scale_and_seeks_without_checkpoints();
    test_voice_render_raw8_high_step_multi_wrap_loop_matches_sequential_oracle();
    test_voice_render_raw8_jump_skips_loop_start_then_sequential_wrap_matches_oracle();
    test_voice_render_direct_far_loop_matches_oracle();
    test_voice_dpcm8_loop_high_step_matches_oracle();
    test_voice_trigger_offset_quirks();
    test_voice_step_ceiling();
}
