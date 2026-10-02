// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "core/codec/dpcm8.h"
#include "core/engine/engine_defs.h" // kSampleRateHz
#include "core/engine/tracker_engine.h"
#include "core/model/song.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_packer.h"
#include "core/codec/sample_pack.h"

// NNA (New Note Action).
// Интеграционные тесты через полный TrackerEngine: nna_slots_ и voices_
// приватны, DCT/DCA-скан и voice-stealing реализованы в
// TrackerEngine::handle_note_trigger_nna, поэтому их нельзя проверить на
// уровне effect_dispatch.h, как test_effect_dispatch_porta.cpp делает для
// остальных эффектов (см. DispatchContext::on_note_trigger_nna в .h).
//
// Поведение наблюдается через смешанный звук (render_add), а не через
// внутреннее состояние. Все синтетические сэмплы - константный
// DPCM8-сигнал без панорамы и огибающих громкости (pan=32 нейтрально,
// envelope_volume либо отсутствует, либо везде 64). Тогда для любого
// одного голоса mix_l[i]+mix_r[i] равно его вкладу (pan_l+pan_r==1 всегда,
// см. tracker_engine.cpp mix_native_sample), а вклады нескольких
// одновременно звучащих голосов складываются линейно. Сколько голосов
// звучит, читается по амплитуде суммы L+R, без доступа к приватным полям.
//
// note=48, c5_speed=kSampleRateHz, FrequencyModel::Amiga - та же связка,
// что в test_voice.cpp (voice.step==0x10000, нативный и выходной сэмпл
// 1:1), позиция декодера предсказуема по числу отрендеренных фреймов.
// default_speed=1 (1 тик/строка), default_tempo=125 -> 110250/125=882
// сэмпла/тик без остатка, границы строк известны точно.
//
// Строка N звучит в окне [N*kSamplesPerRow, (N+1)*kSamplesPerRow), в том
// числе первая: её тик 0 отыгрывается целиком
// (TrackerEngine::first_tick_pending_). Раньше было не так: первый
// advance_tick() уходил на строку 1, не отрисовав ни одного фрейма строки
// 0, и тесты компенсировали это лишней пустой строкой спереди.

namespace {

using namespace soundsinth;
using soundsinth::model::DuplicateCheckAction;
using soundsinth::model::DuplicateCheckType;
using soundsinth::model::NewNoteAction;
using soundsinth::model::PatternCell;

constexpr uint32_t kSamplesPerRow = 882; // см. заголовок файла

PatternCell note_cell(uint8_t instrument_1based) {
    PatternCell c;
    c.note       = 48;
    c.instrument = instrument_1based;
    return c;
}

PatternCell empty_cell() {
    return PatternCell{};
}

// Синтетическая песня на один (по умолчанию) или N каналов: константный
// DPCM8-сигнал по каждому добавленному сэмплу, инструменты и паттерн
// строит вызывающий тест. Подход к PSRAM/PatternPacker тот же, что в
// test_sequencer.cpp и test_voice.cpp, но собран в один Fixture ради
// полного TrackerEngine: нужен memory::TrackMemory целиком, а не только
// PsramStore, TrackerEngine читает ещё и sample_cache.
struct Fixture {
    memory::TrackMemory mem;
    std::vector<uint8_t> scratch = std::vector<uint8_t>(memory::kPatternPackBufferBytes);
    std::vector<soundsinth::model::SampleDescriptor> samples;
    std::vector<soundsinth::model::Instrument> instruments;
    std::vector<soundsinth::model::Pattern> patterns;
    std::vector<uint16_t> order;
    soundsinth::model::Song song;
    std::unique_ptr<engine::TrackerEngine> engine_ptr;

    // Хранилища огибающих: Instrument::volume_envelope указывает сюда, адрес
    // должен жить весь тест.
    std::vector<std::unique_ptr<soundsinth::model::Envelope>> envelopes;
    bool linear_model = false; // FrequencyModel::Linear вместо Amiga

    // Паттерны тут делаются после сэмплов, поэтому место под них
    // оставляется заранее: иначе страницы сэмплов заберут блок целиком.
    Fixture() {
        memory::track_memory_create(mem);
        memory::psram_reserve_track_bytes(mem.psram, 512u * 1024u);
    }
    ~Fixture() { memory::track_memory_destroy(mem); }

    uint16_t add_constant_sample(int16_t value, uint32_t length) {
        std::vector<int16_t> native(length, value);
        sample_pack::SamplePacker packer(mem.psram, sample_pack::ResidentEncoding::Dpcm8);
        CHECK(packer.add_samples(native.data(), length));
        const sample_pack::PackResult result = packer.finish();
        CHECK(result.ok);

        soundsinth::model::SampleDescriptor sd;
        sd.resident_encoding = soundsinth::model::ResidentEncoding::Dpcm8;
        sd.length_samples    = length;
        sd.c5_speed          = engine::kSampleRateHz;
        sd.default_volume    = 64;
        const uint16_t index = static_cast<uint16_t>(samples.size());
        samples.push_back(sd);
        CHECK(memory::sample_cache_alloc_slot(mem.sample_cache, index, result.first_page, result.checkpoint_first_page) != nullptr);
        return index;
    }

    // has_envelope==true - константная (везде 64) volume envelope, нужна
    // только чтобы открыть key_released-путь у Off (см. effect_dispatch.cpp
    // Note-Off и handle_note_trigger_nna). Громкость она не меняет:
    // envelope_volume везде 64, формула в mix_native_sample не искажается.
    uint16_t add_instrument(NewNoteAction nna, DuplicateCheckType dct, DuplicateCheckAction dca, uint16_t sample_index, bool has_envelope = false,
                            uint32_t fadeout_rate = 0, uint8_t envelope_value = 64) {
        soundsinth::model::Instrument ins;
        ins.nna                  = nna;
        ins.dct                  = dct;
        ins.dca                  = dca;
        ins.default_sample_index = sample_index;
        ins.fadeout_rate         = fadeout_rate;
        if (has_envelope) {
            envelopes.push_back(std::make_unique<soundsinth::model::Envelope>());
            soundsinth::model::Envelope& env = *envelopes.back();
            env.enabled                      = true;
            env.point_count                  = 1;
            env.points[0]                    = {0, envelope_value};
            ins.volume_envelope              = &env;
        }
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

        soundsinth::model::Pattern p;
        p.row_count     = row_count;
        p.channel_count = channel_count;
        p.psram_offset  = offset;
        patterns.push_back(p);
    }

    // Меандр с периодом period отсчётов - по нему видна высота (переходы
    // через ноль), а не только число звучащих голосов.
    uint16_t add_square_sample(uint32_t period, uint32_t length, int8_t finetune) {
        std::vector<int16_t> native(length);
        for (uint32_t i = 0; i < length; ++i)
            native[i] = (i % period) < period / 2 ? 1000 : -1000;
        sample_pack::SamplePacker packer(mem.psram, sample_pack::ResidentEncoding::Dpcm8);
        CHECK(packer.add_samples(native.data(), length));
        const sample_pack::PackResult result = packer.finish();
        CHECK(result.ok);
        soundsinth::model::SampleDescriptor sd;
        sd.resident_encoding = soundsinth::model::ResidentEncoding::Dpcm8;
        sd.length_samples    = length;
        sd.c5_speed          = engine::kSampleRateHz;
        sd.default_volume    = 64;
        sd.finetune          = finetune;
        const uint16_t index = static_cast<uint16_t>(samples.size());
        samples.push_back(sd);
        CHECK(memory::sample_cache_alloc_slot(mem.sample_cache, index, result.first_page, result.checkpoint_first_page) != nullptr);
        return index;
    }

    engine::TrackerEngine& finalize(uint8_t channel_count, uint16_t speed = 1) {
        song.samples          = samples.data();
        song.sample_count     = static_cast<uint16_t>(samples.size());
        song.instruments      = instruments.data();
        song.instrument_count = static_cast<uint16_t>(instruments.size());
        song.patterns         = patterns.data();
        song.pattern_count    = static_cast<uint16_t>(patterns.size());
        order                 = {0, soundsinth::model::kOrderEnd};
        song.order            = order.data();
        song.order_count      = static_cast<uint16_t>(order.size());
        song.channel_count    = channel_count;
        song.default_speed    = speed; // по умолчанию 1 тик/строка - 1 строка == kSamplesPerRow сэмплов
        song.default_tempo    = 125;
        song.frequency_model  = linear_model ? soundsinth::model::FrequencyModel::Linear : soundsinth::model::FrequencyModel::Amiga;
        engine_ptr            = std::make_unique<engine::TrackerEngine>(song, mem);
        return *engine_ptr;
    }
};

// Рендерит n_frames в свежие буферы (engine продолжает с текущей позиции)
// и возвращает |L[i]+R[i]| для нужных индексов. Почему эта сумма равна
// суммарной амплитуде всех звучащих голосов - см. заголовок файла.
std::vector<float> render_sum(engine::TrackerEngine& engine, uint32_t n_frames) {
    std::vector<int32_t> mix_l(n_frames, 0), mix_r(n_frames, 0);
    mixbus::SoundSource* src = engine.as_sound_source();
    src->render_add(src->self, mix_l.data(), mix_r.data(), n_frames);
    // Шина в Q24.8 - в единицы int16, как на выходе MixBus.
    constexpr float kUnit = static_cast<float>(1u << mixbus::kMixFracBits);
    std::vector<float> sum(n_frames);
    for (uint32_t i = 0; i < n_frames; ++i)
        sum[i] = std::fabs(static_cast<float>(mix_l[i] + mix_r[i])) / kUnit;
    return sum;
}

void test_nna_continue_backgrounds_old_voice_and_sums_energy() {
    std::printf("test_nna_continue_backgrounds_old_voice_and_sums_energy\n");

    Fixture f;
    const uint16_t s   = f.add_constant_sample(1000, 5000);
    const uint16_t ins = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    f.add_pattern({{note_cell(ins)}, {note_cell(ins)}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);

    const auto sum = render_sum(eng, kSamplesPerRow + 400); // строка 0 целиком + немного в строку 1

    const float before = sum[800]; // строка 0 - один голос
    const float after = sum[kSamplesPerRow + 300]; // строка 1, вскоре после ретриггера - старый голос ушёл в фон, оба звучат
    std::printf("  before=%.1f after=%.1f\n", before, after);
    CHECK(before > 500.0f && before < 1500.0f); // ~1000, один голос
    CHECK(after > 1500.0f && after < 2500.0f);  // ~2000, два голоса одновременно
}

// Выключенный канал (Song::channel_muted, IT ChnPan 0x80): его ноты не
// звучат ни сами, ни хвостами NNA, соседний канал играет как обычно.
void test_muted_channel_is_silent_with_nna() {
    std::printf("test_muted_channel_is_silent_with_nna\n");

    Fixture f;
    const uint16_t s   = f.add_constant_sample(1000, 5000);
    const uint16_t ins = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    f.add_pattern({{note_cell(ins), note_cell(ins)}, {note_cell(ins), note_cell(ins)}, {empty_cell(), empty_cell()}}, 2);
    f.song.channel_muted       = 2; // канал 1
    engine::TrackerEngine& eng = f.finalize(2);

    const auto sum     = render_sum(eng, kSamplesPerRow + 400);
    const float before = sum[800];
    const float after  = sum[kSamplesPerRow + 300];
    std::printf("  before=%.1f after=%.1f\n", before, after);
    CHECK(before > 500.0f && before < 1500.0f); // ~1000: звучит только канал 0
    CHECK(after > 1500.0f && after < 2500.0f);  // ~2000: канал 0 и его хвост, у канала 1 ни голоса, ни хвоста
}

void test_nna_cut_default_does_not_double() {
    std::printf("test_nna_cut_default_does_not_double\n");

    Fixture f;
    const uint16_t s = f.add_constant_sample(1000, 5000);
    // NewNoteAction::Cut - поведение, которое было до поддержки NNA, регрессия
    // не должна его менять: старый голос обрывается, а не уходит в фон.
    const uint16_t ins = f.add_instrument(NewNoteAction::Cut, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    f.add_pattern({{note_cell(ins)}, {note_cell(ins)}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);

    const auto sum    = render_sum(eng, kSamplesPerRow + 400);
    const float after = sum[kSamplesPerRow + 300];
    std::printf("  after=%.1f\n", after);
    CHECK(after > 500.0f && after < 1500.0f); // ~1000, не удвоилось
}

void test_nna_works_at_full_channel_count() {
    std::printf("test_nna_works_at_full_channel_count\n");

    // Договор поменялся, тест закрепляет новый.
    //
    // Раньше пул считался как SOUNDSINTH_MAX_VOICES минус число каналов песни,
    // и у песни на все 64 канала он выходил нулевым: NNA молча вырождался в
    // Cut, а тест это закреплял, то есть закреплял дефект.
    //
    // Теперь размер массивов (SOUNDSINTH_MAX_SLOTS) отделён от потолка
    // звучащих голосов (SOUNDSINTH_MAX_VOICES): место для хвостов есть сверх
    // живых каналов всегда, и NNA работает при любом их числе.
    Fixture f;
    const uint16_t s   = f.add_constant_sample(1000, 5000);
    const uint16_t ins = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    std::vector<PatternCell> row0(SOUNDSINTH_MAX_VOICES), row1(SOUNDSINTH_MAX_VOICES), row2(SOUNDSINTH_MAX_VOICES);
    row0[0] = note_cell(ins);
    row1[0] = note_cell(ins);
    f.add_pattern({row0, row1, row2}, SOUNDSINTH_MAX_VOICES);
    engine::TrackerEngine& eng = f.finalize(SOUNDSINTH_MAX_VOICES);

    const auto sum    = render_sum(eng, kSamplesPerRow + 400);
    const float after = sum[kSamplesPerRow + 300];
    std::printf("  after=%.1f\n", after);
    CHECK(after > 1500.0f && after < 2500.0f); // ~2000: старый голос ушёл в фон и звучит вместе с новым
}

// Потолок SOUNDSINTH_MAX_VOICES: 63 живых голоса и два хвоста NNA на одно
// место. Гасится тихий хвост по громкости этого тика, громкий звучит;
// лишний хвост не замирает, а гаснет (tails_dropped). Громкость - усиление
// голоса, без амплитуды сэмпла, поэтому хвосты различаются громкостью
// сэмпла по умолчанию.
void test_nna_tail_over_ceiling_drops_quietest() {
    std::printf("test_nna_tail_over_ceiling_drops_quietest\n");

    Fixture f;
    const uint16_t base             = f.add_constant_sample(100, 20000);
    const uint16_t loud             = f.add_constant_sample(4000, 20000);
    const uint16_t quiet            = f.add_constant_sample(4000, 20000);
    f.samples[quiet].default_volume = 16; // звучит как 1000
    const uint16_t ins_base         = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, base);
    const uint16_t ins_loud         = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, loud);
    const uint16_t ins_quiet        = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, quiet);
    constexpr uint8_t kChannels     = SOUNDSINTH_MAX_VOICES - 1;
    std::vector<PatternCell> row0(kChannels), row1(kChannels), row2(kChannels);
    for (uint8_t ch = 0; ch < kChannels - 2; ++ch)
        row0[ch] = note_cell(ins_base);
    // Тихий уходит в фон первым и берёт слот 0: по номеру слота остался бы он.
    row0[kChannels - 2] = note_cell(ins_quiet);
    row0[kChannels - 1] = note_cell(ins_loud);
    // Новые ноты уводят тихий и громкий голоса в фон: живых 63, хвостов 2.
    row1[kChannels - 2] = note_cell(ins_base);
    row1[kChannels - 1] = note_cell(ins_base);
    f.add_pattern({row0, row1, row2}, kChannels);
    engine::TrackerEngine& eng = f.finalize(kChannels);

    const auto sum    = render_sum(eng, kSamplesPerRow + 400);
    const float after = sum[kSamplesPerRow + 300];
    std::printf("  after=%.1f dropped=%u\n", after, eng.tails_dropped());
    // 63 * 100 + громкий хвост 4000 = 10300; тихий оставленный дал бы 7300.
    CHECK(after > 9800.0f && after < 10800.0f);
    CHECK_EQ(eng.tails_dropped(), 1u);
    CHECK(eng.active_voice_count() <= SOUNDSINTH_MAX_VOICES);
}

void test_nna_off_with_envelope_releases_and_fades_out() {
    std::printf("test_nna_off_with_envelope_releases_and_fades_out\n");

    Fixture f;
    const uint16_t s = f.add_constant_sample(1000, 5000);
    // fadeout_rate=32768 (половина Q16.16 65536 за тик), speed=1 -> 1
    // тик/строка: строка 1 (ретриггер) доводит fadeout до 50%, строка 2 (без
    // ноты) - до 0%, слот освобождается.
    const uint16_t ins = f.add_instrument(NewNoteAction::Off, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, /*has_envelope=*/true, 32768);
    f.add_pattern({{note_cell(ins)}, {note_cell(ins)}, {empty_cell()}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);

    const auto sum     = render_sum(eng, 3 * kSamplesPerRow);
    const float before = sum[800];                      // строка 0 - один голос
    const float mid    = sum[kSamplesPerRow + 300];     // строка 1 - фон на ~50% fadeout, плюс новый голос на 100%
    const float after  = sum[2 * kSamplesPerRow + 300]; // строка 2 - фон уже освобождён (0%), только новый голос
    std::printf("  before=%.1f mid=%.1f after=%.1f\n", before, mid, after);
    CHECK(before > 500.0f && before < 1500.0f); // ~1000
    CHECK(mid > 1200.0f && mid < 1900.0f);      // ~1500 (1000 новый + ~500 затухающий хвост)
    CHECK(after > 500.0f && after < 1500.0f);   // ~1000 - хвост полностью затух и освобождён
}

void test_nna_fade_releases_even_without_volume_envelope() {
    std::printf("test_nna_fade_releases_even_without_volume_envelope\n");

    // Fade, в отличие от Off, стартует fadeout безусловно, даже если у
    // инструмента нет volume envelope (has_envelope=false) - тот же принцип,
    // что у KeyOff, см. handle_note_trigger_nna и effect_dispatch.cpp. Фазы
    // before/mid/after те же, что в Off-тесте.
    Fixture f;
    const uint16_t s   = f.add_constant_sample(1000, 5000);
    const uint16_t ins = f.add_instrument(NewNoteAction::Fade, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, /*has_envelope=*/false, 32768);
    f.add_pattern({{note_cell(ins)}, {note_cell(ins)}, {empty_cell()}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);

    const auto sum    = render_sum(eng, 3 * kSamplesPerRow);
    const float mid   = sum[kSamplesPerRow + 300];
    const float after = sum[2 * kSamplesPerRow + 300];
    std::printf("  mid=%.1f after=%.1f\n", mid, after);
    CHECK(mid > 1200.0f && mid < 1900.0f);
    CHECK(after > 500.0f && after < 1500.0f);
}

// NNA Off у инструмента без огибающей громкости - как KeyOff у IT: затухание
// по fadeout (как Fade); при нулевом fadeout хвост держится.
void test_nna_off_without_envelope_fades_by_fadeout() {
    std::printf("test_nna_off_without_envelope_fades_by_fadeout\n");
    for (uint32_t fadeout : {32768u, 0u}) {
        Fixture f;
        const uint16_t s   = f.add_constant_sample(1000, 5000);
        const uint16_t ins = f.add_instrument(NewNoteAction::Off, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, /*has_envelope=*/false, fadeout);
        f.add_pattern({{note_cell(ins)}, {note_cell(ins)}, {empty_cell()}, {empty_cell()}}, 1);
        engine::TrackerEngine& eng = f.finalize(1);
        const auto sum             = render_sum(eng, 3 * kSamplesPerRow);
        const float mid            = sum[kSamplesPerRow + 300];
        const float after          = sum[2 * kSamplesPerRow + 300];
        std::printf("  fadeout %u: mid=%.1f after=%.1f\n", fadeout, mid, after);
        if (fadeout != 0) {
            CHECK(mid > 1200.0f && mid < 1900.0f);    // новый 1000 + гаснущий ~500
            CHECK(after > 500.0f && after < 1500.0f); // хвост затух
        } else {
            CHECK(mid > 1900.0f && after > 1900.0f); // хвост держится
        }
    }
}

// DCA Off без огибающей громкости: дубликат уходит в затухание, а не
// обрывается (раньше - мгновенный Cut). NNA Continue уводит его в фон.
void test_dca_off_without_envelope_fades() {
    std::printf("test_dca_off_without_envelope_fades\n");
    Fixture f;
    const uint16_t s   = f.add_constant_sample(1000, 5000);
    const uint16_t ins = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Sample, DuplicateCheckAction::Off, s, /*has_envelope=*/false, 32768);
    f.add_pattern({{note_cell(ins)}, {note_cell(ins)}, {empty_cell()}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);
    const auto sum             = render_sum(eng, 3 * kSamplesPerRow);
    const float mid            = sum[kSamplesPerRow + 300];
    const float after          = sum[2 * kSamplesPerRow + 300];
    std::printf("  mid=%.1f after=%.1f\n", mid, after);
    CHECK(mid > 1200.0f && mid < 1900.0f);
    CHECK(after > 500.0f && after < 1500.0f);
}

void test_nna_dct_sample_cuts_previously_backgrounded_tail_same_channel() {
    std::printf("test_nna_dct_sample_cuts_previously_backgrounded_tail_same_channel\n");

    Fixture f;
    const uint16_t s     = f.add_constant_sample(1000, 5000);
    const uint16_t ins_a = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    // Второй инструмент: другой номер, тот же сэмпл. DuplicateCheckType::Sample
    // совпадает по sample_index независимо от номера инструмента - квирк
    // kQuirkItDctRequiresInstrumentMatch снят нарочно (у IT он стоит всегда,
    // тот случай - в тесте ниже).
    const uint16_t ins_b = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Sample, DuplicateCheckAction::Cut, s);
    f.add_pattern({{note_cell(ins_a)}, {note_cell(ins_a)}, {note_cell(ins_b)}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);

    // Строка 0: триггер A (голос 1).
    // Строка 1: триггер A снова (Continue) -> голос 1 уходит в фон, голос 2
    // живой.
    // Строка 2: триггер B (DCT=Sample, DCA=Cut) -> оба совпадающих по сэмплу
    // голоса (живой голос 2 и фоновый голос 1) обрываются DCT/DCA; NNA для
    // голоса 2 уже не применяется (к этому моменту voice_active==false) ->
    // после строки 2 звучит один голос (голос 3).
    const auto sum          = render_sum(eng, 3 * kSamplesPerRow + 400);
    const float before_row2 = sum[2 * kSamplesPerRow - 100]; // конец строки 1 - два голоса (1 фон + 1 живой)
    const float after_row2  = sum[3 * kSamplesPerRow + 300]; // строка 2, после DCT/DCA-обрыва обоих совпадающих
    std::printf("  before_row2=%.1f after_row2=%.1f\n", before_row2, after_row2);
    CHECK(before_row2 > 1500.0f && before_row2 < 2500.0f); // ~2000 - два голоса до DCT/DCA
    CHECK(after_row2 > 500.0f && after_row2 < 1500.0f);    // ~1000 - только новый голос, оба старых обрублены
}

// DCT=Sample, как у IT - с kQuirkItDctRequiresInstrumentMatch: другой
// инструмент с тем же сэмплом не обрывает (живой уходит в фон, прежний
// фоновый снимается правилом "один на канал" - два голоса), тот же
// инструмент с тем же сэмплом обрывает (один голос).
void test_nna_dct_sample_with_it_instrument_match() {
    std::printf("test_nna_dct_sample_with_it_instrument_match\n");
    {
        Fixture f;
        const uint16_t s     = f.add_constant_sample(1000, 5000);
        const uint16_t ins_a = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
        const uint16_t ins_b = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Sample, DuplicateCheckAction::Cut, s);
        f.add_pattern({{note_cell(ins_a)}, {note_cell(ins_a)}, {note_cell(ins_b)}, {empty_cell()}}, 1);
        f.song.quirks              = soundsinth::model::kQuirkItDctRequiresInstrumentMatch;
        engine::TrackerEngine& eng = f.finalize(1);
        const auto sum             = render_sum(eng, 3 * kSamplesPerRow + 400);
        const float after_row2     = sum[3 * kSamplesPerRow + 300];
        std::printf("  a different instrument: after_row2=%.1f\n", after_row2);
        CHECK(after_row2 > 1500.0f && after_row2 < 2500.0f);
    }
    {
        Fixture f;
        const uint16_t s     = f.add_constant_sample(1000, 5000);
        const uint16_t ins_a = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Sample, DuplicateCheckAction::Cut, s);
        f.add_pattern({{note_cell(ins_a)}, {note_cell(ins_a)}, {note_cell(ins_a)}, {empty_cell()}}, 1);
        f.song.quirks              = soundsinth::model::kQuirkItDctRequiresInstrumentMatch;
        engine::TrackerEngine& eng = f.finalize(1);
        const auto sum             = render_sum(eng, 3 * kSamplesPerRow + 400);
        const float after_row2     = sum[3 * kSamplesPerRow + 300];
        std::printf("  the same instrument: after_row2=%.1f\n", after_row2);
        CHECK(after_row2 > 500.0f && after_row2 < 1500.0f);
    }
}

// Кража слота при полном пуле: 32 фоновых голоса каналов 0..31, живые
// сняты kNoteCut, канал 32 уводит 33-й. Крадётся самый тихий по формуле
// выбора: при линейной огибающей - Y (громкость 8, огибающая 64), в
// децибелах - X (громкость 64, огибающая 32 = -48 дБ). При равной
// громкости - самый старый (меньший alloc_seq). Разность суммы: новый
// живой +375, новый фоновый +625, минус украденный.
float nna_steal_delta(bool decibel, bool equal_loudness, uint32_t* steals) {
    Fixture f;
    const uint16_t s            = f.add_constant_sample(1000, 40000);
    const uint16_t s_double     = f.add_constant_sample(2000, 40000);
    const uint16_t im           = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 64);
    const uint16_t im2          = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s_double, true, 0, 64);
    const uint16_t ix           = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 32);
    const uint16_t iy           = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 64);
    constexpr uint8_t kChannels = 33;
    std::vector<std::vector<PatternCell>> rows(7, std::vector<PatternCell>(kChannels));
    auto with_volume = [](PatternCell c, uint8_t v) {
        c.volume.type  = soundsinth::model::VolumeColumnType::SetVolume;
        c.volume.param = v;
        return c;
    };
    for (uint8_t c = 0; c < 32; ++c) {
        uint16_t ins = im;
        uint8_t vol  = 40;
        if (equal_loudness) {
            if (c == 0) ins = im2; // та же громкость по формуле, вдвое громче в сумме
        } else if (c == 30) {
            ins = ix;
            vol = 64;
        } else if (c == 31) {
            ins = iy;
            vol = 8;
        }
        rows[0][c]      = with_volume(note_cell(static_cast<uint8_t>(ins)), vol);
        rows[1][c]      = note_cell(static_cast<uint8_t>(ins));
        rows[2][c].note = soundsinth::model::kNoteCut; // живые молчат, фоновые остаются
    }
    rows[3][32] = with_volume(note_cell(static_cast<uint8_t>(im)), 40);
    rows[4][32] = note_cell(static_cast<uint8_t>(im));
    f.add_pattern(rows, kChannels);
    if (decibel) f.song.quirks = soundsinth::model::kQuirkEnvelopeDecibel;
    engine::TrackerEngine& eng = f.finalize(kChannels);
    const auto sum             = render_sum(eng, 6 * kSamplesPerRow);
    *steals                    = eng.nna_steals();
    return sum[5 * kSamplesPerRow + 400] - sum[4 * kSamplesPerRow - 100];
}

void test_nna_steal_takes_quietest_then_oldest() {
    std::printf("test_nna_steal_takes_quietest_then_oldest\n");
    uint32_t steals    = 0;
    const float linear = nna_steal_delta(false, false, &steals);
    CHECK_EQ(steals, 1u);
    const float decibel = nna_steal_delta(true, false, &steals);
    CHECK_EQ(steals, 1u);
    const float oldest = nna_steal_delta(false, true, &steals);
    CHECK_EQ(steals, 1u);
    std::printf("  sum difference: linear %.1f (Y), dB %.1f (X), equal %.1f (the higher)\n", linear, decibel, oldest);
    CHECK(std::fabs(linear - 875.0f) <= 10.0f);  // 1000 - 125
    CHECK(std::fabs(decibel - 996.0f) <= 10.0f); // 1000 - ~4
    CHECK(std::fabs(oldest + 250.0f) <= 10.0f);  // 1000 - 1250: канал 0, вдвое громче
}

void test_nna_dct_scope_is_limited_to_origin_channel() {
    std::printf("test_nna_dct_scope_is_limited_to_origin_channel\n");

    Fixture f;
    const uint16_t s     = f.add_constant_sample(1000, 5000);
    const uint16_t ins_a = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    const uint16_t ins_b = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Sample, DuplicateCheckAction::Cut, s);
    // 2 канала. Канал 0: строка0=A, строка1=A (уводит первый голос в фон,
    // origin_channel=0). Канал 1: строка1=B - первый триггер на канале 1,
    // DCT=Sample совпадает с фоновым хвостом канала 0 по сэмплу, но область
    // сканирования ограничена origin_channel, канал 0 не должен пострадать.
    f.add_pattern(
        {
            {note_cell(ins_a), empty_cell()},
            {note_cell(ins_a), note_cell(ins_b)},
            {empty_cell(), empty_cell()},
        },
        2);
    engine::TrackerEngine& eng = f.finalize(2);

    const auto sum = render_sum(eng, 2 * kSamplesPerRow + 400);
    const float after = sum[2 * kSamplesPerRow + 300]; // строка 2 - канал0: живой(строка1)+фон(строка0), канал1: живой(строка1) = 3 голоса
    std::printf("  after=%.1f\n", after);
    CHECK(after > 2500.0f && after < 3500.0f); // ~3000 - фон канала 0 пережил триггер на канале 1
}

void test_nna_tails_coexist_across_channels() {
    std::printf("test_nna_tails_coexist_across_channels\n");

    // Что проверяется теперь и почему не прежнее.
    //
    // Раньше тест исчерпывал пул числом каналов: пул считался как
    // SOUNDSINTH_MAX_VOICES минус каналы песни, и при 63 каналах оставался
    // один слот. Теперь пул не зависит от числа каналов (config.h,
    // SOUNDSINTH_MAX_SLOTS) и всегда равен SOUNDSINTH_MAX_NNA_VOICES, а
    // хвостов у канала не больше одного - исчерпать его можно только
    // тридцатью тремя каналами, и там раньше упрётся бюджет голосов. Кража
    // слота такой постройкой недостижима, закреплять её этим тестом нечем.
    //
    // Проверяемым осталось главное: хвосты разных каналов сосуществуют, а не
    // вытесняют друг друга - то, чего не было при нулевом пуле.
    Fixture f;
    const uint16_t s            = f.add_constant_sample(1000, 500000);
    const uint16_t ins          = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    constexpr uint8_t kChannels = 10;
    std::vector<PatternCell> row0(kChannels), row1(kChannels), row2(kChannels);
    for (uint8_t c = 0; c < kChannels; ++c) {
        row0[c] = note_cell(ins);
        row1[c] = note_cell(ins);
    }
    f.add_pattern({row0, row1, row2}, kChannels);
    engine::TrackerEngine& eng = f.finalize(kChannels);

    const auto sum    = render_sum(eng, kSamplesPerRow + 400);
    const float after = sum[kSamplesPerRow + 300];
    std::printf("  after=%.1f (expecting %d voices)\n", after, 2 * kChannels);
    // Десять живых плюс десять хвостов: каждый канал увёл свой старый
    // голос в фон, и ни один не вытеснил чужой.
    const float expect = 1000.0f * 2 * kChannels;
    CHECK(after > expect - 1500.0f && after < expect + 1500.0f);
}

// Средний период волны между переходами через ноль снизу вверх в
// [from, from + count).
float mean_zero_cross_period(const std::vector<float>& x, uint32_t from, uint32_t count) {
    uint32_t first = 0, last = 0, crossings = 0;
    for (uint32_t i = from + 1; i < from + count && i < x.size(); ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            if (crossings == 0) first = i;
            last = i;
            ++crossings;
        }
    }
    return crossings > 1 ? float(last - first) / float(crossings - 1) : 0.0f;
}

// Тик ретриггера (E93) звучит той же высотой, что соседние: voice_trigger
// считает шаг от ноты без finetune сэмпла (модель Amiga), и синхронизация
// высоты идёт после ретриггера. Раньше порядок был обратный, и у сэмпла с
// finetune +7/8 полутона тик ретриггера звучал без него: период меандра 64
// отсчёта вместо 60.8.
void test_retrigger_keeps_channel_pitch() {
    std::printf("test_retrigger_keeps_channel_pitch\n");

    Fixture f;
    const uint16_t s   = f.add_square_sample(64, 40000, 112); // finetune +7 * 16
    const uint16_t ins = f.add_instrument(NewNoteAction::Cut, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    PatternCell cell   = note_cell(ins);
    cell.effect.type   = soundsinth::model::Effect::Retrigger;
    cell.effect.param  = 3; // каждые 3 тика: при speed 6 - на тике 3
    f.add_pattern({{cell}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1, /*speed=*/6);

    std::vector<int32_t> mix_l(6 * kSamplesPerRow, 0), mix_r(6 * kSamplesPerRow, 0);
    mixbus::SoundSource* src = eng.as_sound_source();
    src->render_add(src->self, mix_l.data(), mix_r.data(), static_cast<uint32_t>(mix_l.size()));
    std::vector<float> x(mix_l.size());
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(mix_l[i] + mix_r[i]);

    const float tick2 = mean_zero_cross_period(x, 2 * kSamplesPerRow, kSamplesPerRow);
    const float tick3 = mean_zero_cross_period(x, 3 * kSamplesPerRow + 100, kSamplesPerRow - 100); // после перезапуска
    std::printf("  period: tick 2 %.2f, tick 3 (retrigger) %.2f\n", tick2, tick3);
    CHECK(tick2 > 60.0f && tick2 < 61.7f);
    CHECK(tick3 > 60.0f && tick3 < 61.7f);
}

// Период звучания меандра по тикам 6..23 при Glissando (E31/S11) и
// тон-портаменто с ноты 48 на 51 (строка 1 - 3xx, строка 2 - 300, дальше
// без эффекта), speed 6.
std::vector<float> glissando_periods(soundsinth::model::QuirkFlags quirks, bool linear) {
    Fixture f;
    f.linear_model     = linear;
    const uint16_t s   = f.add_square_sample(64, 60000, 0);
    const uint16_t ins = f.add_instrument(NewNoteAction::Cut, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    PatternCell r0     = note_cell(static_cast<uint8_t>(ins));
    r0.effect.type     = soundsinth::model::Effect::GlissandoControl;
    r0.effect.param    = 1;
    PatternCell r1     = note_cell(static_cast<uint8_t>(ins));
    r1.note            = 51;
    r1.effect.type     = soundsinth::model::Effect::TonePorta;
    r1.effect.param    = 3;
    PatternCell r2;
    r2.effect.type  = soundsinth::model::Effect::TonePorta;
    r2.effect.param = 0;
    f.add_pattern({{r0}, {r1}, {r2}, {empty_cell()}, {empty_cell()}}, 1);
    f.song.quirks              = quirks;
    engine::TrackerEngine& eng = f.finalize(1, /*speed=*/6);
    std::vector<int32_t> mix_l(24 * kSamplesPerRow, 0), mix_r(24 * kSamplesPerRow, 0);
    mixbus::SoundSource* src = eng.as_sound_source();
    for (uint32_t d = 0; d < mix_l.size(); d += SOUNDSINTH_AUDIO_BUFFER_FRAMES) {
        const uint32_t k =
            static_cast<uint32_t>(mix_l.size()) - d < SOUNDSINTH_AUDIO_BUFFER_FRAMES ? static_cast<uint32_t>(mix_l.size()) - d : SOUNDSINTH_AUDIO_BUFFER_FRAMES;
        src->render_add(src->self, mix_l.data() + d, mix_r.data() + d, k);
    }
    std::vector<float> x(mix_l.size());
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(mix_l[i] + mix_r[i]);
    std::vector<float> periods;
    for (uint32_t t = 6; t < 24; ++t)
        periods.push_back(mean_zero_cross_period(x, t * kSamplesPerRow + 60, kSamplesPerRow - 60));
    return periods;
}

void print_periods(const char* name, const std::vector<float>& p) {
    std::printf("  %s:", name);
    for (size_t i = 0; i < p.size(); ++i)
        std::printf("%s%.2f", i % 6 == 0 ? " | " : " ", p[i]);
    std::printf("\n");
}

// Glissando: звучащая высота ступенями по полутонам. ProTracker - только на
// строке с тон-портаменто и не на её тике 0; S3M/IT - и дальше; XM -
// ступень к ближайшему полутону.
void test_glissando_sounding_pitch_by_mode() {
    std::printf("test_glissando_sounding_pitch_by_mode\n");
    const auto pt      = glissando_periods(soundsinth::model::kQuirkGlissandoPtMode, false);
    const auto plain   = glissando_periods(0, false);
    const auto nearest = glissando_periods(soundsinth::model::kQuirkGlissandoNearest, false);
    const auto linear  = glissando_periods(0, true);
    print_periods("ProTracker", pt);
    print_periods("S3M/IT", plain);
    print_periods("XM", nearest);
    print_periods("Linear", linear);
    auto near = [](float got, float want) { return std::fabs(got - want) <= 0.1f; };
    // Индекс i - тик 6 + i: строка 1 - 0..5, строка 2 - 6..11, строка 3 - 12..17.
    CHECK(near(pt[0], 64.00f));
    CHECK(near(pt[1], 60.38f));     // C#: ступень на строке с 3xx
    CHECK(near(pt[6], 61.75f));     // тик 0 строки с 300 - без ступени
    CHECK(near(pt[10], 56.93f));    // D
    CHECK(near(pt[12], 59.46f));    // строка без 3xx - без ступени
    CHECK(near(plain[6], 60.38f));  // S3M/IT: ступень держится и на тике 0
    CHECK(near(plain[12], 56.93f)); // и на строке без эффекта
    for (int i = 0; i < 5; ++i)
        CHECK(near(nearest[i], 64.00f)); // XM: к ближайшему полутону
    CHECK(near(nearest[5], 60.38f));
    CHECK(near(linear[1], 60.38f)); // модель Linear - те же ступени
    CHECK(near(linear[7], 57.00f));
}

// Правый край законов панорамы: FT2 до конца вправо не доводит (L/R =
// 4096/65408), соседнее значение - по таблице корня; корень без особого
// края и линейный закон дают L = 0.
void test_pan_law_right_edge() {
    std::printf("test_pan_law_right_edge\n");
    auto lr = [](soundsinth::model::Song::PanLaw law, uint8_t pan, int32_t* l, int32_t* r) {
        Fixture f;
        const uint16_t s   = f.add_constant_sample(10000, 5000);
        const uint16_t ins = f.add_instrument(NewNoteAction::Cut, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
        f.add_pattern({{note_cell(static_cast<uint8_t>(ins))}, {empty_cell()}}, 1);
        f.song.pan_law             = law;
        f.song.channel_pan[0]      = pan;
        engine::TrackerEngine& eng = f.finalize(1);
        std::vector<int32_t> mix_l(kSamplesPerRow, 0), mix_r(kSamplesPerRow, 0);
        mixbus::SoundSource* src = eng.as_sound_source();
        src->render_add(src->self, mix_l.data(), mix_r.data(), kSamplesPerRow);
        *l = mix_l[800];
        *r = mix_r[800];
    };
    int32_t l = 0, r = 0;
    lr(soundsinth::model::Song::PanLaw::Ft2Sqrt, 64, &l, &r);
    CHECK(r > 0);
    CHECK(std::fabs(static_cast<double>(l) / r - 4096.0 / 65408.0) < 1e-4);
    lr(soundsinth::model::Song::PanLaw::Ft2Sqrt, 63, &l, &r);
    CHECK(std::fabs(static_cast<double>(l) / r - 8192.0 / 65022.0) < 1e-4);
    lr(soundsinth::model::Song::PanLaw::Sqrt, 64, &l, &r);
    CHECK_EQ(l, 0);
    lr(soundsinth::model::Song::PanLaw::Linear, 64, &l, &r);
    CHECK_EQ(l, 0);
}

// Retrigger на выключенном канале голоса не запускает: сумма та же, что у
// одного звучащего канала.
void test_muted_channel_retrigger_stays_silent() {
    std::printf("test_muted_channel_retrigger_stays_silent\n");
    Fixture f;
    const uint16_t s    = f.add_constant_sample(1000, 20000);
    const uint16_t ins  = f.add_instrument(NewNoteAction::Cut, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    PatternCell retrig  = note_cell(static_cast<uint8_t>(ins));
    retrig.effect.type  = soundsinth::model::Effect::Retrigger;
    retrig.effect.param = 3;
    f.add_pattern({{note_cell(static_cast<uint8_t>(ins)), retrig}, {empty_cell(), retrig}, {empty_cell(), empty_cell()}}, 2);
    f.song.channel_muted       = 2; // канал 1
    engine::TrackerEngine& eng = f.finalize(2, /*speed=*/6);
    const auto sum             = render_sum(eng, 12 * kSamplesPerRow);
    uint32_t loud              = 0;
    for (uint32_t i = 100; i < sum.size(); ++i)
        loud += sum[i] > 1100.0f;
    CHECK_EQ(loud, 0u);
    CHECK(sum[4 * kSamplesPerRow] > 900.0f); // канал 0 звучит
}

// Нота, чей сэмпл ещё не в памяти, молчит и считается; после загрузки
// сэмпла следующая нота звучит, счётчик не растёт.
void test_trigger_without_sample_is_counted() {
    std::printf("test_trigger_without_sample_is_counted\n");
    Fixture f;
    const uint16_t s                = f.add_constant_sample(1000, 5000);
    const uint16_t ins              = f.add_instrument(NewNoteAction::Cut, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s);
    memory::SampleCacheEntry* entry = memory::sample_cache_find(f.mem.sample_cache, s);
    CHECK(entry != nullptr);
    const uint16_t first_page            = entry->first_page;
    const uint16_t checkpoint_first_page = entry->checkpoint_first_page;
    memory::sample_cache_free_slot(f.mem.sample_cache, entry); // сэмпл "не догружен"
    f.add_pattern({{note_cell(static_cast<uint8_t>(ins))}, {note_cell(static_cast<uint8_t>(ins))}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);
    const auto row0            = render_sum(eng, kSamplesPerRow);
    CHECK_EQ(eng.triggers_without_sample(), 1u);
    CHECK(row0[800] < 1.0f);
    CHECK(memory::sample_cache_alloc_slot(f.mem.sample_cache, s, first_page, checkpoint_first_page) != nullptr);
    const auto row1 = render_sum(eng, kSamplesPerRow);
    CHECK_EQ(eng.triggers_without_sample(), 1u);
    CHECK(row1[800] > 500.0f);
}

// Список полон (64 живых), канал 0 уводит в фон голос сэмпла B: фоновый
// сверх потолка гаснет, а не замирает, и когда место освободилось (строка 5,
// kNoteCut на канале 1), не возобновляется с того места, где стоял.
struct ObservedSamples {
    uint16_t watch            = 0;
    uint32_t ticks_with_watch = 0;
};
void observe_samples(void* user, uint16_t, const uint16_t* sample_indices, uint8_t count) {
    auto* o = static_cast<ObservedSamples*>(user);
    for (uint8_t i = 0; i < count; ++i) {
        if (sample_indices[i] == o->watch) ++o->ticks_with_watch;
    }
}
void test_tail_over_full_list_fades_not_freezes() {
    std::printf("test_tail_over_full_list_fades_not_freezes\n");
    Fixture f;
    const uint16_t sa           = f.add_constant_sample(1000, 20000);
    const uint16_t sb           = f.add_constant_sample(5000, 20000);
    const uint16_t ia           = f.add_instrument(NewNoteAction::Cut, DuplicateCheckType::Off, DuplicateCheckAction::Cut, sa);
    const uint16_t ib           = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, sb);
    constexpr uint8_t kChannels = SOUNDSINTH_MAX_VOICES;
    std::vector<std::vector<PatternCell>> rows(8, std::vector<PatternCell>(kChannels));
    rows[0][0] = note_cell(static_cast<uint8_t>(ib));
    for (uint8_t c = 1; c < kChannels; ++c)
        rows[0][c] = note_cell(static_cast<uint8_t>(ia));
    rows[1][0]      = note_cell(static_cast<uint8_t>(ia)); // B уходит в фон, живых 64
    rows[5][1].note = soundsinth::model::kNoteCut;         // в списке освобождается место
    f.add_pattern(rows, kChannels);
    engine::TrackerEngine& eng = f.finalize(kChannels);
    ObservedSamples obs;
    obs.watch = sb;
    eng.set_tick_observer(&observe_samples, &obs);
    const auto sum   = render_sum(eng, 7 * kSamplesPerRow);
    const float row5 = sum[5 * kSamplesPerRow + 800];
    std::printf("  row 5: %.1f, faded beyond the cap %u, ticks with B in the observer %u\n", row5, eng.tails_dropped(), obs.ticks_with_watch);
    CHECK(eng.tails_dropped() >= 1u);
    CHECK(std::fabs(row5 - 63000.0f) < 100.0f); // 63 живых, фоновый B не вернулся
    CHECK_EQ(obs.ticks_with_watch, 1u);         // B - только на тике 0, пока играл живым
}

// --- Ветки .mid: волновое гашение (Song::volume_ramp_samples != 0) ---

// Raw16 - значения без потерь кодека.
uint16_t add_raw16(Fixture& f, const std::vector<int16_t>& native) {
    const uint32_t length = static_cast<uint32_t>(native.size());
    sample_pack::SamplePacker packer(f.mem.psram, sample_pack::ResidentEncoding::Raw16);
    CHECK(packer.add_samples(native.data(), length));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    soundsinth::model::SampleDescriptor sd;
    sd.resident_encoding = soundsinth::model::ResidentEncoding::Raw16;
    sd.length_samples    = length;
    sd.c5_speed          = engine::kSampleRateHz;
    sd.default_volume    = 64;
    const uint16_t index = static_cast<uint16_t>(f.samples.size());
    f.samples.push_back(sd);
    CHECK(memory::sample_cache_alloc_slot(f.mem.sample_cache, index, result.first_page, result.checkpoint_first_page) != nullptr);
    return index;
}

// Синус 441 Гц (период 100 отсчётов).
uint16_t add_sine_raw16(Fixture& f, int16_t amplitude, uint32_t length) {
    std::vector<int16_t> native(length);
    for (uint32_t i = 0; i < length; ++i) {
        native[i] = static_cast<int16_t>(amplitude * std::sin(2.0 * 3.14159265358979 * i / 100.0));
    }
    return add_raw16(f, native);
}

// L+R кусками по буферу, как на плате.
std::vector<float> render_lr_chunked(engine::TrackerEngine& eng, uint32_t n_frames) {
    std::vector<int32_t> mix_l(n_frames, 0), mix_r(n_frames, 0);
    mixbus::SoundSource* src = eng.as_sound_source();
    for (uint32_t d = 0; d < n_frames; d += SOUNDSINTH_AUDIO_BUFFER_FRAMES) {
        const uint32_t k = n_frames - d < SOUNDSINTH_AUDIO_BUFFER_FRAMES ? n_frames - d : SOUNDSINTH_AUDIO_BUFFER_FRAMES;
        src->render_add(src->self, mix_l.data() + d, mix_r.data() + d, k);
    }
    std::vector<float> x(n_frames);
    for (uint32_t i = 0; i < n_frames; ++i) {
        x[i] = static_cast<float>(mix_l[i] + mix_r[i]) / static_cast<float>(1u << mixbus::kMixFracBits);
    }
    return x;
}

float max_abs(const std::vector<float>& x, uint32_t from, uint32_t to) {
    float m = 0.0f;
    for (uint32_t i = from; i < to && i < x.size(); ++i)
        m = std::fabs(x[i]) > m ? std::fabs(x[i]) : m;
    return m;
}

float max_step(const std::vector<float>& x, uint32_t from, uint32_t to) {
    float m = 0.0f;
    for (uint32_t i = from + 1; i < to && i < x.size(); ++i) {
        const float d = std::fabs(x[i] - x[i - 1]);
        m             = d > m ? d : m;
    }
    return m;
}

uint32_t zero_crossings(const std::vector<float>& x, uint32_t from, uint32_t to) {
    uint32_t n = 0;
    for (uint32_t i = from + 1; i < to && i < x.size(); ++i)
        n += (x[i - 1] < 0.0f) != (x[i] < 0.0f);
    return n;
}

void setup_midi_like(Fixture& f) {
    f.song.volume_ramp_samples = 220;
    f.song.quirks              = soundsinth::model::kQuirkEnvelopeDecibel;
}

// (а) NoteCut посреди ноты: 220 отсчётов гасится продолжение синуса (переходы
// через ноль, спад к нулю), а не замороженный отсчёт; дальше тишина.
void test_midi_wave_tail_after_note_cut() {
    std::printf("test_midi_wave_tail_after_note_cut\n");
    Fixture f;
    setup_midi_like(f);
    const uint16_t s   = add_sine_raw16(f, 10000, 20000);
    const uint16_t ins = f.add_instrument(NewNoteAction::Off, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 64);
    PatternCell cut;
    cut.note = soundsinth::model::kNoteCut;
    f.add_pattern({{note_cell(static_cast<uint8_t>(ins))}, {empty_cell()}, {empty_cell()}, {cut}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);
    const auto x               = render_lr_chunked(eng, 5 * kSamplesPerRow);
    const uint32_t cut_at      = 3 * kSamplesPerRow;
    const float amp            = max_abs(x, kSamplesPerRow, cut_at);
    std::printf("  amplitude %.0f; after the cut: zero crossings %u, start %.0f, end %.0f, beyond %.0f\n", amp, zero_crossings(x, cut_at, cut_at + 220),
                max_abs(x, cut_at, cut_at + 30), max_abs(x, cut_at + 200, cut_at + 220), max_abs(x, cut_at + 221, cut_at + 400));
    CHECK(amp > 1000.0f);
    CHECK(zero_crossings(x, cut_at, cut_at + 220) >= 3u); // волна, а не постоянная
    CHECK(max_abs(x, cut_at, cut_at + 30) > 0.5f * amp);
    CHECK(max_abs(x, cut_at + 200, cut_at + 220) < 0.05f * amp);
    CHECK(max_abs(x, cut_at + 221, cut_at + 400) == 0.0f);
}

// (б) Увод в фон (NNA Off): голос продолжает волну в слоте без ступеньки -
// новая нота беззвучная, слышен только фоновый.
void test_midi_background_keeps_wave_continuous() {
    std::printf("test_midi_background_keeps_wave_continuous\n");
    Fixture f;
    setup_midi_like(f);
    const uint16_t s          = add_sine_raw16(f, 10000, 20000);
    const uint16_t silent     = f.add_constant_sample(0, 20000);
    const uint16_t ins        = f.add_instrument(NewNoteAction::Continue, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 64);
    const uint16_t ins_silent = f.add_instrument(NewNoteAction::Cut, DuplicateCheckType::Off, DuplicateCheckAction::Cut, silent, true, 0, 64);
    f.add_pattern({{note_cell(static_cast<uint8_t>(ins))}, {empty_cell()}, {note_cell(static_cast<uint8_t>(ins_silent))}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);
    const auto x               = render_lr_chunked(eng, 4 * kSamplesPerRow);
    const uint32_t at          = 2 * kSamplesPerRow;
    const float slope          = max_step(x, kSamplesPerRow, at - 10);
    const float across         = max_step(x, at - 10, at + 50);
    std::printf("  sine step %.1f, through the steal %.1f, background after the steal %.0f\n", slope, across, max_abs(x, at + 100, at + 200));
    CHECK(across <= slope * 1.01f + 2.0f);
    CHECK(max_abs(x, at + 100, at + 200) > 0.5f * max_abs(x, kSamplesPerRow, at)); // фоновый звучит
}

// (в) NoteCut за 100 отсчётов до конца незацикленного сэмпла: волна кончается
// раньше гашения (ветка "данные кончились"), дальше гасится последнее
// значение - без ступеньки, до тишины.
void test_midi_wave_tail_runs_out_of_data() {
    std::printf("test_midi_wave_tail_runs_out_of_data\n");
    Fixture f;
    setup_midi_like(f);
    const uint32_t cut_at = 3 * kSamplesPerRow;
    const uint16_t s      = add_sine_raw16(f, 10000, cut_at + 100);
    const uint16_t ins    = f.add_instrument(NewNoteAction::Off, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 64);
    PatternCell cut;
    cut.note = soundsinth::model::kNoteCut;
    f.add_pattern({{note_cell(static_cast<uint8_t>(ins))}, {empty_cell()}, {empty_cell()}, {cut}, {empty_cell()}}, 1);
    engine::TrackerEngine& eng = f.finalize(1);
    const auto x               = render_lr_chunked(eng, 5 * kSamplesPerRow);
    const float slope          = max_step(x, kSamplesPerRow, cut_at - 10);
    const float across         = max_step(x, cut_at - 10, cut_at + 500);
    std::printf("  sine step %.1f, through the cut and the end of data %.1f, after %.0f\n", slope, across, max_abs(x, cut_at + 500, cut_at + 700));
    CHECK(across <= slope * 1.01f + 2.0f);
    CHECK(max_abs(x, cut_at + 500, cut_at + 700) == 0.0f);
}

// (г) Нота, чей сэмпл не в каталоге (не догружен, не прочитался): прежний
// голос канала гаснет без ступеньки - и при NNA Cut (голос обрывается
// новой нотой), и при NNA Off (голос уведён в фон).
void test_midi_note_without_sample_fades_previous() {
    std::printf("test_midi_note_without_sample_fades_previous\n");
    for (const NewNoteAction nna : {NewNoteAction::Cut, NewNoteAction::Off}) {
        Fixture f;
        setup_midi_like(f);
        const uint16_t s       = add_sine_raw16(f, 10000, 20000);
        const uint16_t missing = add_sine_raw16(f, 10000, 20000);
        memory::sample_cache_evict(f.mem.sample_cache, f.mem.psram, memory::sample_cache_find(f.mem.sample_cache, missing));
        const uint16_t ins         = f.add_instrument(nna, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 64);
        const uint16_t ins_missing = f.add_instrument(nna, DuplicateCheckType::Off, DuplicateCheckAction::Cut, missing, true, 0, 64);
        f.add_pattern({{note_cell(static_cast<uint8_t>(ins))}, {empty_cell()}, {note_cell(static_cast<uint8_t>(ins_missing))}, {empty_cell()}}, 1);
        engine::TrackerEngine& eng = f.finalize(1);
        const auto x               = render_lr_chunked(eng, 4 * kSamplesPerRow);
        const uint32_t at          = 2 * kSamplesPerRow;
        const float slope          = max_step(x, kSamplesPerRow, at - 10);
        const float across         = max_step(x, at - 10, at + 400);
        std::printf("  NNA %s: sine step %.1f, through a note without a sample %.1f, notes without a sample %u\n", nna == NewNoteAction::Cut ? "Cut" : "Off",
                    slope, across, eng.triggers_without_sample());
        CHECK_EQ(eng.triggers_without_sample(), 1u);
        CHECK(across <= slope * 1.01f + 2.0f);
    }
}

// Отсчётов от начала спада (последний отсчёт на полном уровне) до -48 дБ.
// Релиз огибающей в децибелах: 64 -> 0 за 40 тиков, -48 дБ - значение 32,
// через 20 тиков. Note-Off на строке 2, Txx 250 (если tempo_change) на
// строке 1.
uint32_t release_to_minus48db(bool real_time, bool tempo_change) {
    Fixture f;
    setup_midi_like(f);
    f.song.envelopes_in_real_time    = real_time;
    const uint16_t s                 = f.add_constant_sample(1000, 60000);
    const uint16_t ins               = f.add_instrument(NewNoteAction::Off, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 64);
    soundsinth::model::Envelope& env = *f.envelopes.back();
    env.point_count                  = 2;
    env.points[1]                    = {40, 0};
    env.sustain_enabled              = true;
    env.sustain_point                = 0;
    env.sustain_end                  = 0;
    std::vector<std::vector<PatternCell>> rows(64, std::vector<PatternCell>(1));
    rows[0][0] = note_cell(static_cast<uint8_t>(ins));
    if (tempo_change) {
        rows[1][0].effect.type  = soundsinth::model::Effect::SetTempo;
        rows[1][0].effect.param = 250;
    }
    rows[2][0].note = soundsinth::model::kNoteOff;
    f.add_pattern(rows, 1);
    engine::TrackerEngine& eng = f.finalize(1);
    const auto sum             = render_sum(eng, 40 * kSamplesPerRow);
    const float full           = sum[kSamplesPerRow / 2];
    uint32_t start             = kSamplesPerRow / 2; // после разгона ноты
    while (start < sum.size() && sum[start] >= 0.99f * full)
        ++start;
    uint32_t end        = start;
    const float minus48 = full * std::pow(10.0f, -48.0f / 20.0f);
    while (end < sum.size() && sum[end] > minus48)
        ++end;
    CHECK(full > 500.0f);
    CHECK(end < sum.size());
    return end - start;
}

// (г) Огибающие .mid в реальном времени: релиз после Txx 125 -> 250 длится
// столько же миллисекунд, сколько без смены темпа (допуск - тик). Без
// реального времени - вдвое короче.
void test_midi_envelope_release_keeps_time_across_tempo() {
    std::printf("test_midi_envelope_release_keeps_time_across_tempo\n");
    const uint32_t base  = release_to_minus48db(true, false);
    const uint32_t fast  = release_to_minus48db(true, true);
    const uint32_t ticks = release_to_minus48db(false, true);
    std::printf("  down to -48 dB: 125 %u, 125->250 %u, in track ticks %u samples (20 ticks of 125 is %u)\n", base, fast, ticks, 20 * kSamplesPerRow);
    CHECK(base + kSamplesPerRow >= 20 * kSamplesPerRow && base <= 21 * kSamplesPerRow);
    CHECK(fast + kSamplesPerRow >= base && fast <= base + kSamplesPerRow);
    CHECK(ticks < base * 6 / 10);
}

// Вытеснение по карте тика, как у загрузки на плате: сэмпл, которого нет в
// карте, освобождается сразу, и его страницы тут же занимает громкий сэмпл
// той же длины. Raw16: у DPCM8 постоянный мусор - нулевые дельты, и голос на
// чужих страницах не выдал бы себя.
constexpr uint32_t kCullTestSampleLength = 20000;
struct EvictUnmapped {
    Fixture* f            = nullptr;
    uint16_t sample_count = 0;
    uint32_t evicted      = 0;
};
void evict_unmapped(void* user, uint16_t, const uint16_t* sample_indices, uint8_t count) {
    auto* e = static_cast<EvictUnmapped*>(user);
    for (uint16_t s = 0; s < e->sample_count; ++s) {
        bool mapped = false;
        for (uint8_t i = 0; i < count; ++i)
            mapped = mapped || sample_indices[i] == s;
        memory::SampleCacheEntry* entry = memory::sample_cache_find(e->f->mem.sample_cache, s);
        if (mapped || entry == nullptr) continue;
        memory::sample_cache_evict(e->f->mem.sample_cache, e->f->mem.psram, entry);
        std::vector<int16_t> loud(kCullTestSampleLength, 30000);
        sample_pack::SamplePacker packer(e->f->mem.psram, sample_pack::ResidentEncoding::Raw16);
        CHECK(packer.add_samples(loud.data(), static_cast<uint32_t>(loud.size())));
        CHECK(packer.finish().ok);
        ++e->evicted;
    }
}

// 64 голоса .mid, сброс по перегрузке включён: ноты снимаются по четыре за
// строку (волновые хвосты), бюджет тем временем гасит часть голосов. Сэмпл
// каждого волнового хвоста обязан быть в карте тика - иначе хвост дочитывает
// чужие страницы.
void test_midi_wave_tails_keep_samples_mapped_under_cull() {
    std::printf("test_midi_wave_tails_keep_samples_mapped_under_cull\n");
    Fixture f;
    setup_midi_like(f);
    constexpr uint8_t kChannels = SOUNDSINTH_MAX_VOICES;
    std::vector<std::vector<PatternCell>> rows(24, std::vector<PatternCell>(kChannels));
    for (uint8_t c = 0; c < kChannels; ++c) {
        const uint16_t s        = add_raw16(f, std::vector<int16_t>(kCullTestSampleLength, 100));
        const uint16_t ins      = f.add_instrument(NewNoteAction::Off, DuplicateCheckType::Off, DuplicateCheckAction::Cut, s, true, 0, 64);
        rows[0][c]              = note_cell(static_cast<uint8_t>(ins));
        rows[1 + c / 4][c].note = soundsinth::model::kNoteCut;
    }
    f.add_pattern(rows, kChannels);
    engine::TrackerEngine& eng = f.finalize(kChannels);
    eng.set_voice_cull_enabled(true);
    eng.set_overload_hint_pct(100);
    EvictUnmapped ev;
    ev.f            = &f;
    ev.sample_count = kChannels;
    eng.set_tick_observer(&evict_unmapped, &ev);
    const auto x     = render_lr_chunked(eng, 20 * kSamplesPerRow);
    const float peak = max_abs(x, 0, 20 * kSamplesPerRow);
    std::printf("  evicted %u, culled %u, peak %.0f (64 voices at 100 is 6400)\n", ev.evicted, eng.voices_culled(), peak);
    CHECK(ev.evicted == kChannels);
    CHECK(eng.voices_culled() > 0);
    CHECK(peak <= 6400.0f * 1.02f);
}

} // namespace

void run_nna_tests() {
    test_nna_off_without_envelope_fades_by_fadeout();
    test_dca_off_without_envelope_fades();
    test_midi_wave_tails_keep_samples_mapped_under_cull();
    test_midi_envelope_release_keeps_time_across_tempo();
    test_midi_wave_tail_after_note_cut();
    test_midi_background_keeps_wave_continuous();
    test_midi_wave_tail_runs_out_of_data();
    test_midi_note_without_sample_fades_previous();
    test_glissando_sounding_pitch_by_mode();
    test_tail_over_full_list_fades_not_freezes();
    test_trigger_without_sample_is_counted();
    test_pan_law_right_edge();
    test_muted_channel_retrigger_stays_silent();
    test_nna_tail_over_ceiling_drops_quietest();
    test_retrigger_keeps_channel_pitch();
    test_nna_dct_sample_with_it_instrument_match();
    test_nna_steal_takes_quietest_then_oldest();
    test_nna_continue_backgrounds_old_voice_and_sums_energy();
    test_nna_cut_default_does_not_double();
    test_muted_channel_is_silent_with_nna();
    test_nna_works_at_full_channel_count();
    test_nna_off_with_envelope_releases_and_fades_out();
    test_nna_fade_releases_even_without_volume_envelope();
    test_nna_dct_sample_cuts_previously_backgrounded_tail_same_channel();
    test_nna_dct_scope_is_limited_to_origin_channel();
    test_nna_tails_coexist_across_channels();
}
