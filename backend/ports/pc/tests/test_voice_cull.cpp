#include "testing.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "core/config.h"
#include "core/codec/dpcm8.h"
#include "core/engine/engine_defs.h" // kSampleRateHz
#include "core/engine/tracker_engine.h"
#include "core/model/song.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_packer.h"
#include "core/codec/sample_pack.h"

// Сброс лишних голосов при перегрузке процессора.
//
// Приём наблюдения тот же, что в test_nna.cpp: константные сэмплы, pan=32
// (pan_l + pan_r == 1), огибающих нет, поэтому сумма левого и правого
// равна суммарной амплитуде всех звучащих в этот момент голосов. Сколько
// голосов живо, читается прямо по амплитуде.
//
// Загрузку рендера на PC не измерить (часов нет, platform/clock.h), поэтому
// она задаётся напрямую через set_overload_hint_pct() - тестовый ввод
// заведён для этого.

using namespace soundsinth;
using namespace soundsinth::model;

namespace {

// 1 тик/строка при tempo=125 - столько выходных сэмплов на строку.
constexpr uint32_t kSamplesPerRow = 882;

struct Fixture {
    memory::TrackMemory mem;
    std::vector<uint8_t> scratch = std::vector<uint8_t>(memory::kPatternPackBufferBytes);
    std::vector<SampleDescriptor> samples;
    std::vector<Instrument> instruments;
    std::vector<Pattern> patterns;
    std::vector<uint16_t> order;
    Song song;
    std::unique_ptr<engine::TrackerEngine> engine_ptr;

    Fixture() { memory::track_memory_create(mem); }
    ~Fixture() { memory::track_memory_destroy(mem); }

    uint16_t add_constant_sample(int16_t value, uint32_t length) {
        std::vector<int16_t> native(length, value);
        sample_pack::SamplePacker packer(mem.psram, sample_pack::ResidentEncoding::Dpcm8);
        CHECK(packer.add_samples(native.data(), length));
        const sample_pack::PackResult result = packer.finish();
        CHECK(result.ok);

        SampleDescriptor sd;
        sd.resident_encoding = ResidentEncoding::Dpcm8;
        sd.length_samples = length;
        sd.c5_speed = engine::kSampleRateHz;
        sd.default_volume = 64;
        // Петля обязательна: бюджету надо опуститься с 64 до числа каналов по
        // одному за тик, это десятки строк. Без петли сэмпл кончился бы сам
        // раньше, чем начнётся сброс, и тест мерил бы тишину и проходил впустую.
        sd.loop_enabled = true;
        sd.loop_start = 0;
        sd.loop_end = length;
        const uint16_t index = static_cast<uint16_t>(samples.size());
        samples.push_back(sd);
        CHECK(memory::sample_cache_alloc_slot(mem.sample_cache, index, result.first_page,
                                               result.checkpoint_first_page) != nullptr);
        return index;
    }

    uint16_t add_instrument(uint16_t sample_index, NewNoteAction nna = NewNoteAction::Cut) {
        Instrument ins;
        ins.nna = nna;
        ins.default_sample_index = sample_index;
        instruments.push_back(ins);
        return static_cast<uint16_t>(instruments.size()); // 1-based
    }

    void add_pattern(const std::vector<std::vector<PatternCell>>& rows, uint8_t channel_count) {
        const uint16_t row_count = static_cast<uint16_t>(rows.size());
        patterns::PatternPacker packer(scratch.data(), static_cast<uint32_t>(scratch.size()), row_count, channel_count);
        for (const auto& row : rows) {
            CHECK(row.size() == channel_count);
            CHECK(packer.add_row(row.data()));
        }
        CHECK(packer.ok());
        const uint32_t offset = packer.finish(mem.psram);
        CHECK(offset != memory::kPatternAllocFailed);

        Pattern p;
        p.row_count = row_count;
        p.channel_count = channel_count;
        p.psram_offset = offset;
        patterns.push_back(p);
    }

    engine::TrackerEngine& finalize(uint8_t channel_count) {
        song.samples = samples.data();
        song.sample_count = static_cast<uint16_t>(samples.size());
        song.instruments = instruments.data();
        song.instrument_count = static_cast<uint16_t>(instruments.size());
        song.patterns = patterns.data();
        song.pattern_count = static_cast<uint16_t>(patterns.size());
        order = {0, kOrderEnd};
        song.order = order.data();
        song.order_count = static_cast<uint16_t>(order.size());
        song.channel_count = channel_count;
        song.default_speed = 1;
        song.default_tempo = 125;
        song.frequency_model = FrequencyModel::Amiga;
        engine_ptr = std::make_unique<engine::TrackerEngine>(song, mem);
        return *engine_ptr;
    }
};

std::vector<float> render_sum(engine::TrackerEngine& engine, uint32_t n_frames) {
    std::vector<int32_t> mix_l(n_frames, 0), mix_r(n_frames, 0);
    mixbus::SoundSource* src = engine.as_sound_source();
    src->render_add(src->self, mix_l.data(), mix_r.data(), n_frames);
    std::vector<float> sum(n_frames);
    for (uint32_t i = 0; i < n_frames; ++i) sum[i] = std::fabs(static_cast<float>(mix_l[i] + mix_r[i]));
    return sum;
}

PatternCell note_cell(uint8_t note, uint16_t instrument, uint8_t volume) {
    PatternCell c;
    c.note = note;
    c.instrument = static_cast<uint8_t>(instrument);
    c.volume.type = VolumeColumnType::SetVolume;
    c.volume.param = volume;
    return c;
}

// Песня из n_channels каналов: на каждом одна и та же нота с первой строки
// и тишина дальше - все голоса звучат одновременно и одинаково.
engine::TrackerEngine& make_chorus(Fixture& fx, uint8_t n_channels, uint16_t rows) {
    const uint16_t smp = fx.add_constant_sample(1000, 8192);
    const uint16_t ins = fx.add_instrument(smp);
    std::vector<std::vector<PatternCell>> pattern;
    for (uint16_t r = 0; r < rows; ++r) {
        std::vector<PatternCell> row(n_channels);
        if (r == 0) {
            for (uint8_t ch = 0; ch < n_channels; ++ch) row[ch] = note_cell(48, ins, 64);
        }
        pattern.push_back(row);
    }
    fx.add_pattern(pattern, n_channels);
    return fx.finalize(n_channels);
}

// --- 1. Бюджет опускается под перегрузкой, голоса уходят ---
void test_cull_budget_falls_under_overload_and_voices_go() {
    Fixture fx;
    constexpr uint8_t kChannels = 24;
    engine::TrackerEngine& engine = make_chorus(fx, kChannels, 96);
    engine.set_voice_cull_enabled(true);
    engine.set_overload_hint_pct(100); // выше верхнего порога SOUNDSINTH_VOICE_CULL_HIGH_PCT

    const std::vector<float> first = render_sum(engine, kSamplesPerRow);
    const float before = first[800];

    // Каждый тик (== строка при speed=1) бюджет падает на единицу, а стартует
    // он с SOUNDSINTH_MAX_VOICES, так что до 24 каналов ему идти 40 тиков, и
    // только потом начнётся сброс. Отсюда запас.
    for (int row = 0; row < 60; ++row) render_sum(engine, kSamplesPerRow);
    const std::vector<float> later = render_sum(engine, kSamplesPerRow);
    const float after = later[800];

    std::printf("test_cull_budget_falls_under_overload_and_voices_go\n");
    std::printf("  before=%.1f after=%.1f budget=%u culled=%lu\n", before, after,
                static_cast<unsigned>(engine.voice_budget()), (unsigned long)engine.voices_culled());
    CHECK(engine.voice_budget() < kChannels);
    CHECK(engine.voices_culled() > 0);
    CHECK(after < before);
}

// Спуск бюджета соразмерен перегрузу: голос на каждые два процента сверх
// порога. Шагом в один голос путь с 64 до 40 занимал 24 тика, и всё это
// время рендер не успевал - на плате это стоило провала буфера (SWARS.MID,
// SGM: 56 голосов при потолке 41).
void test_cull_budget_step_matches_overload() {
    std::printf("test_cull_budget_step_matches_overload\n");
    using engine::pct_to_q8;
    using engine::voice_budget_after_overload;
    constexpr uint32_t kHigh = pct_to_q8(95), kStep = pct_to_q8(2);

    // 104% - перегруз 9 пунктов: четыре голоса плюс один.
    CHECK_EQ(voice_budget_after_overload(64, pct_to_q8(104), kHigh, 8, kStep, 8), 59);
    // Чуть выше порога - один голос, как раньше.
    CHECK_EQ(voice_budget_after_overload(64, pct_to_q8(96), kHigh, 8, kStep, 8), 63);
    // Всплеск замера не срезает полифонию целиком: шаг ограничен.
    CHECK_EQ(voice_budget_after_overload(64, pct_to_q8(300), kHigh, 8, kStep, 8), 56);
    // Ниже пола не опускается.
    CHECK_EQ(voice_budget_after_overload(10, pct_to_q8(300), kHigh, 8, kStep, 8), 8);
    // Под порогом бюджет не трогается.
    CHECK_EQ(voice_budget_after_overload(64, pct_to_q8(90), kHigh, 8, kStep, 8), 64);

    // На движке: с подсказкой 110% бюджет уходит вниз за единицы тиков.
    Fixture fx;
    engine::TrackerEngine& engine = make_chorus(fx, 24, 96);
    engine.set_voice_cull_enabled(true);
    engine.set_overload_hint_pct(110);
    for (int row = 0; row < 4; ++row) render_sum(engine, kSamplesPerRow);
    std::printf("  бюджет после четырёх тиков: %u\n", static_cast<unsigned>(engine.voice_budget()));
    CHECK(engine.voice_budget() <= 64 - 4 * 4);
}

// --- 2. Гасится самый тихий, громкий остаётся ---
void test_cull_picks_the_quietest_voice() {
    Fixture fx;
    // 16 каналов: 8 громких и 8 тихих. Число выбрано под пол бюджета
    // (SOUNDSINTH_VOICE_CULL_MIN_VOICES == 8): бюджет упрётся в 8, уйти должна
    // тихая половина, громкая - остаться. При числе каналов не больше пола
    // сброс не начался бы.
    constexpr uint8_t kChannels = 16;
    constexpr uint8_t kLoud = 8;
    const uint16_t smp = fx.add_constant_sample(1000, 8192);
    const uint16_t ins = fx.add_instrument(smp);
    std::vector<std::vector<PatternCell>> pattern;
    // Строк с запасом: песня не должна кончиться до замера, иначе ноты
    // перезапустятся и замер будет не о том.
    for (uint16_t r = 0; r < 96; ++r) {
        std::vector<PatternCell> row(kChannels);
        if (r == 0) {
            for (uint8_t ch = 0; ch < kChannels; ++ch) {
                row[ch] = note_cell(48, ins, ch < kLoud ? 64 : 16);
            }
        }
        pattern.push_back(row);
    }
    fx.add_pattern(pattern, kChannels);
    engine::TrackerEngine& engine = fx.finalize(kChannels);
    engine.set_voice_cull_enabled(true);

    const std::vector<float> all = render_sum(engine, kSamplesPerRow);
    const float sum_all = all[800];

    engine.set_overload_hint_pct(100);
    for (int row = 0; row < 70; ++row) render_sum(engine, kSamplesPerRow);
    const std::vector<float> rest = render_sum(engine, kSamplesPerRow);
    const float sum_rest = rest[800];

    std::printf("test_cull_picks_the_quietest_voice\n");
    std::printf("  all=%.1f rest=%.1f budget=%u culled=%lu\n", sum_all, sum_rest,
                static_cast<unsigned>(engine.voice_budget()), (unsigned long)engine.voices_culled());
    CHECK(engine.voice_budget() == SOUNDSINTH_VOICE_CULL_MIN_VOICES);
    // Громкая половина даёт 8*64, тихая 8*16, то есть 80% и 20% суммы.
    // Осталась громкая, если остаток около 80%; если бы механизм резал не по
    // громкости, осталось бы примерно 50%.
    CHECK(sum_rest > sum_all * 0.65f);
    CHECK(sum_rest < sum_all);
}

// --- 3. Пол бюджета соблюдается при любой перегрузке ---
void test_cull_budget_never_falls_below_floor() {
    Fixture fx;
    engine::TrackerEngine& engine = make_chorus(fx, 32, 256);
    engine.set_voice_cull_enabled(true);
    engine.set_overload_hint_pct(200); // вдвое сверх реального времени

    for (int row = 0; row < 250; ++row) render_sum(engine, kSamplesPerRow);

    std::printf("test_cull_budget_never_falls_below_floor\n");
    std::printf("  budget=%u floor=%u\n", static_cast<unsigned>(engine.voice_budget()),
                (unsigned)SOUNDSINTH_VOICE_CULL_MIN_VOICES);
    CHECK(engine.voice_budget() >= SOUNDSINTH_VOICE_CULL_MIN_VOICES);
}

// --- 4. Нагрузка упала - бюджет возвращается ---
void test_cull_budget_recovers_when_load_drops() {
    Fixture fx;
    engine::TrackerEngine& engine = make_chorus(fx, 16, 512);
    engine.set_voice_cull_enabled(true);

    engine.set_overload_hint_pct(100);
    for (int row = 0; row < 30; ++row) render_sum(engine, kSamplesPerRow);
    const uint8_t low = engine.voice_budget();

    // Подъём намеренно медленный: голос за SOUNDSINTH_VOICE_CULL_RISE_TICKS
    // тиков (см. config.h). От low до потолка нужно (64 - low) * RISE_TICKS
    // тиков; прогоняется MAX_VOICES * RISE_TICKS + 32, с запасом.
    engine.set_overload_hint_pct(50); // ниже нижнего порога SOUNDSINTH_VOICE_CULL_LOW_PCT
    const int rows_to_recover =
        static_cast<int>(SOUNDSINTH_MAX_VOICES) * static_cast<int>(SOUNDSINTH_VOICE_CULL_RISE_TICKS) + 32;
    for (int row = 0; row < rows_to_recover; ++row) render_sum(engine, kSamplesPerRow);
    const uint8_t restored = engine.voice_budget();

    std::printf("test_cull_budget_recovers_when_load_drops\n");
    std::printf("  low=%u restored=%u max=%u\n", (unsigned)low, (unsigned)restored,
                (unsigned)SOUNDSINTH_MAX_VOICES);
    CHECK(low < SOUNDSINTH_MAX_VOICES);
    CHECK(restored == SOUNDSINTH_MAX_VOICES);
}

// --- 5. Между порогами бюджет не трогается (нет автоколебаний) ---
void test_cull_budget_is_stable_between_thresholds() {
    Fixture fx;
    engine::TrackerEngine& engine = make_chorus(fx, 16, 256);
    engine.set_voice_cull_enabled(true);

    engine.set_overload_hint_pct(100);
    for (int row = 0; row < 10; ++row) render_sum(engine, kSamplesPerRow);
    const uint8_t settled = engine.voice_budget();

    // Середина между порогами считается из них, а не задана числом: пороги
    // подбираются замером и уже менялись, а тест проверяет саму мёртвую зону,
    // а не конкретные проценты.
    constexpr uint32_t kMidPct = (SOUNDSINTH_VOICE_CULL_HIGH_PCT + SOUNDSINTH_VOICE_CULL_LOW_PCT) / 2u;
    engine.set_overload_hint_pct(kMidPct);
    for (int row = 0; row < 50; ++row) render_sum(engine, kSamplesPerRow);

    std::printf("test_cull_budget_is_stable_between_thresholds\n");
    std::printf("  settled=%u after_mid_load=%u\n", (unsigned)settled,
                (unsigned)engine.voice_budget());
    CHECK(engine.voice_budget() == settled);
}

// --- 6. Выключенный механизм не меняет ничего ---
void test_cull_disabled_changes_nothing() {
    Fixture ref_fx, off_fx;
    engine::TrackerEngine& ref_engine = make_chorus(ref_fx, 24, 64);
    engine::TrackerEngine& off_engine = make_chorus(off_fx, 24, 64);
    // Второму задаём перегрузку, но механизм не включаем.
    off_engine.set_overload_hint_pct(200);

    bool identical = true;
    for (int row = 0; row < 12; ++row) {
        const std::vector<float> a = render_sum(ref_engine, kSamplesPerRow);
        const std::vector<float> b = render_sum(off_engine, kSamplesPerRow);
        for (uint32_t i = 0; i < kSamplesPerRow; ++i) {
            if (a[i] != b[i]) identical = false;
        }
    }

    std::printf("test_cull_disabled_changes_nothing\n");
    std::printf("  identical=%d budget=%u culled=%lu\n", identical ? 1 : 0,
                (unsigned)off_engine.voice_budget(), (unsigned long)off_engine.voices_culled());
    CHECK(identical);
    CHECK(off_engine.voice_budget() == SOUNDSINTH_MAX_VOICES);
    CHECK(off_engine.voices_culled() == 0);
}

// --- 7. Гашение идёт через затухание, а не обрывом ---
//
// Проверка на щелчок. Берётся самый большой перепад между соседними
// отсчётами за весь прогон: при сбросе через 44-сэмпловую рампу он должен
// остаться того же порядка, что и без сброса, а не подскочить на всю
// амплитуду погашенного голоса.
void test_cull_fades_out_without_click() {
    auto max_step = [](engine::TrackerEngine& engine, int rows) {
        float worst = 0.0f;
        for (int r = 0; r < rows; ++r) {
            const std::vector<float> s = render_sum(engine, kSamplesPerRow);
            for (uint32_t i = 1; i < kSamplesPerRow; ++i) {
                const float d = std::fabs(s[i] - s[i - 1]);
                if (d > worst) worst = d;
            }
        }
        return worst;
    };

    Fixture ref_fx, cull_fx;
    engine::TrackerEngine& ref_engine = make_chorus(ref_fx, 24, 96);
    const float ref_step = max_step(ref_engine, 60);

    engine::TrackerEngine& cull_engine = make_chorus(cull_fx, 24, 96);
    cull_engine.set_voice_cull_enabled(true);
    cull_engine.set_overload_hint_pct(100);
    const float cull_step = max_step(cull_engine, 60);

    std::printf("test_cull_fades_out_without_click\n");
    std::printf("  ref_step=%.1f cull_step=%.1f culled=%lu\n", ref_step, cull_step,
                (unsigned long)cull_engine.voices_culled());
    CHECK(cull_engine.voices_culled() > 0);
    // Сравнение с эталонным прогоном, а не с абстрактным порогом: свой
    // максимальный перепад есть и без сброса - это атака ноты на первой
    // строке. Тест проверяет, добавляет ли сброс свой скачок. Обрыв голоса
    // амплитудой 1000 поднял бы максимум примерно на эту величину; через рампу
    // в 44 сэмпла он не поднимается.
    CHECK(cull_step <= ref_step);
}

// --- 8. Ярус важнее громкости: NNA-хвост уходит раньше живого ---
//
// Главная из проверок ярусов. Хвосты здесь громче живых голосов (нота
// взята на полной громкости, перезапущена на тихой), и выбор только по
// громкости увёл бы живые каналы, оставив отпущенные ноты.
void test_cull_takes_nna_tail_before_live_voice() {
    Fixture fx;
    constexpr uint8_t kChannels = 16;
    const uint16_t smp = fx.add_constant_sample(1000, 8192);
    const uint16_t ins = fx.add_instrument(smp, NewNoteAction::Continue);
    std::vector<std::vector<PatternCell>> pattern;
    for (uint16_t r = 0; r < 96; ++r) {
        std::vector<PatternCell> row(kChannels);
        if (r == 0 || r == 1) {
            // Строка 0 - громко, строка 1 - тихо: старая нота уходит в фон громкой,
            // новая живёт тихой.
            for (uint8_t ch = 0; ch < kChannels; ++ch) row[ch] = note_cell(48, ins, r == 0 ? 64 : 8);
        }
        pattern.push_back(row);
    }
    fx.add_pattern(pattern, kChannels);
    engine::TrackerEngine& engine = fx.finalize(kChannels);
    engine.set_voice_cull_enabled(true);

    render_sum(engine, kSamplesPerRow);                       // строка 0: 16 живых
    const std::vector<float> both = render_sum(engine, kSamplesPerRow); // строка 1: 16 живых + 16 хвостов
    const float sum_both = both[800];

    engine.set_overload_hint_pct(100);
    for (int row = 0; row < 80; ++row) render_sum(engine, kSamplesPerRow);
    const std::vector<float> rest = render_sum(engine, kSamplesPerRow);
    const float sum_rest = rest[800];

    std::printf("test_cull_takes_nna_tail_before_live_voice\n");
    std::printf("  both=%.1f rest=%.1f budget=%u culled=%lu\n", sum_both, sum_rest,
                static_cast<unsigned>(engine.voice_budget()), (unsigned long)engine.voices_culled());
    // Хвост звучит на 64/64, живой на 8/64, то есть хвосты дают восемь девятых
    // суммы. Если ушли они, остаток маленький; если бы движок резал по
    // громкости, он убрал бы тихих живых и остаток был бы велик.
    CHECK(sum_rest < sum_both * 0.25f);
}

// --- 9. Внутри яруса решает цена, а не только громкость ---
//
// Оба набора звучат на одной громкости (одинаковый volume, значит
// одинаковый mix_gain), но один играет вдвое выше, а у Dpcm8 распаковка
// последовательная, и цена растёт с шагом. Уйти должен дорогой.
void test_cull_prefers_expensive_voice_at_equal_loudness() {
    Fixture fx;
    constexpr uint8_t kChannels = 16;
    constexpr uint8_t kExpensive = 8;
    // Разная амплитуда сэмпла при одинаковой громкости канала - только так по
    // сумме можно различить, кто выжил: mix_gain от амплитуды сэмпла не
    // зависит.
    const uint16_t loud_smp = fx.add_constant_sample(1000, 8192);
    const uint16_t quiet_smp = fx.add_constant_sample(200, 8192);
    const uint16_t loud_ins = fx.add_instrument(loud_smp);
    const uint16_t quiet_ins = fx.add_instrument(quiet_smp);

    std::vector<std::vector<PatternCell>> pattern;
    for (uint16_t r = 0; r < 96; ++r) {
        std::vector<PatternCell> row(kChannels);
        if (r == 0) {
            for (uint8_t ch = 0; ch < kChannels; ++ch) {
                // Нота 60 на октаву выше 48: шаг вдвое больше и вдвое больше вызовов
                // декодера на выходной сэмпл.
                row[ch] = ch < kExpensive ? note_cell(60, loud_ins, 64) : note_cell(48, quiet_ins, 64);
            }
        }
        pattern.push_back(row);
    }
    fx.add_pattern(pattern, kChannels);
    engine::TrackerEngine& engine = fx.finalize(kChannels);
    engine.set_voice_cull_enabled(true);

    const std::vector<float> all = render_sum(engine, kSamplesPerRow);
    const float sum_all = all[800];

    engine.set_overload_hint_pct(100);
    for (int row = 0; row < 80; ++row) render_sum(engine, kSamplesPerRow);
    const std::vector<float> rest = render_sum(engine, kSamplesPerRow);
    const float sum_rest = rest[800];

    std::printf("test_cull_prefers_expensive_voice_at_equal_loudness\n");
    std::printf("  all=%.1f rest=%.1f budget=%u culled=%lu\n", sum_all, sum_rest,
                static_cast<unsigned>(engine.voice_budget()), (unsigned long)engine.voices_culled());
    // Дорогие дают 8*1000, дешёвые 8*200, то есть 8000 и 1600 из 9600. Ушли
    // дорогие, если остаток около 1600. Если бы цена не учитывалась, выбор был
    // бы произвольным и остаток заметно больше.
    CHECK(sum_rest < sum_all * 0.35f);
}

} // namespace

void run_voice_cull_tests() {
    test_cull_budget_falls_under_overload_and_voices_go();
    test_cull_budget_step_matches_overload();
    test_cull_picks_the_quietest_voice();
    test_cull_budget_never_falls_below_floor();
    test_cull_budget_recovers_when_load_drops();
    test_cull_budget_is_stable_between_thresholds();
    test_cull_disabled_changes_nothing();
    test_cull_fades_out_without_click();
    test_cull_takes_nna_tail_before_live_voice();
    test_cull_prefers_expensive_voice_at_equal_loudness();
}
