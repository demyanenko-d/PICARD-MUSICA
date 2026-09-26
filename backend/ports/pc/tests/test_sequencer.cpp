#include "testing.h"

#include <vector>

#include "core/engine/effect_dispatch.h"
#include "core/engine/sequencer.h"
#include "core/engine/song_duration.h"
#include "core/model/song.h"
#include "core/memory/psram_store.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_packer.h"

// Тесты секвенсора на синтетических Song-фикстурах, без реальных файлов
// (тот же принцип, что у тестов загрузчиков: сначала точная синтетика,
// потом дымовая проверка на реальном файле - она отдельно, не автотест).

namespace {

using namespace soundsinth;
using soundsinth::model::Effect;
using soundsinth::model::Pattern;
using soundsinth::model::PatternCell;
using soundsinth::model::Song;

PatternCell make_cell(Effect fx = Effect::None, uint8_t fx_param = 0) {
    PatternCell c;
    c.effect.type = fx;
    c.effect.param = fx_param;
    return c;
}

// Строит один паттерн в PSRAM из явно заданных строк (rows[row][channel])
// и возвращает готовый Pattern-дескриптор. scratch - переиспользуемый
// буфер PatternPacker (роль сценария PatternPack), один и тот
// же буфер можно использовать для нескольких паттернов подряд.
Pattern build_pattern(memory::PsramStore& psram, uint8_t* scratch, uint32_t scratch_size,
                       const std::vector<std::vector<PatternCell>>& rows, uint8_t channel_count) {
    const uint16_t row_count = static_cast<uint16_t>(rows.size());
    patterns::PatternPacker packer(scratch, scratch_size, row_count, channel_count);
    for (const auto& row : rows) {
        CHECK(packer.add_row(row.data()));
    }
    CHECK(packer.ok());
    const uint32_t offset = packer.finish(psram);
    CHECK(offset != memory::kPatternAllocFailed);

    Pattern p;
    p.row_count = row_count;
    p.channel_count = channel_count;
    p.psram_offset = offset;
    return p;
}

struct Fixture {
    memory::PsramStore psram{};
    uint8_t scratch[memory::kPatternPackBufferBytes];
    std::vector<Pattern> patterns;
    std::vector<uint16_t> order;
    Song song;

    Fixture() { memory::psram_create(psram); }
    ~Fixture() { memory::psram_destroy(psram); }

    void add_pattern(const std::vector<std::vector<PatternCell>>& rows, uint8_t channel_count) {
        patterns.push_back(build_pattern(psram, scratch, sizeof(scratch), rows, channel_count));
    }

    void finalize(uint8_t channel_count, uint16_t restart_position = 0) {
        song.patterns = patterns.data();
        song.pattern_count = static_cast<uint16_t>(patterns.size());
        song.order = order.data();
        song.order_count = static_cast<uint16_t>(order.size());
        song.channel_count = channel_count;
        song.restart_position = restart_position;
    }
};

void test_sequential_no_effects() {
    std::printf("test_sequencer_sequential_no_effects\n");
    Fixture f;
    f.add_pattern({{make_cell()}, {make_cell()}}, 1); // паттерн 0: 2 строки
    f.add_pattern({{make_cell()}, {make_cell()}, {make_cell()}}, 1); // паттерн 1: 3 строки
    f.order = {0, 1, soundsinth::model::kOrderEnd};
    f.finalize(1);
    f.song.default_speed = 2;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.pattern_idx, 0);
    CHECK_EQ(ps.row, 0);

    // speed=2 - 2 тика на строку. Тик 1: остаёмся на row0. Тик 2: row1.
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.row, 0);
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.pattern_idx, 0);
    CHECK_EQ(ps.row, 1);

    // Ещё 2 тика - паттерн 0 закончился (2 строки), переход на паттерн 1 row0.
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.pattern_idx, 1);
    CHECK_EQ(ps.row, 0);

    // Домотать до конца паттерна 1 (3 строки * 2 тика = 6 тиков, 2 уже
    // сделаны) и до kOrderEnd -> restart_position=0.
    for (int i = 0; i < 6; ++i) CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.pattern_idx, 0);
    CHECK_EQ(ps.row, 0);
    CHECK(!ps.song_ended);
}

void test_position_jump() {
    std::printf("test_sequencer_position_jump\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::PositionJump, 2)}}, 1); // паттерн 0: прыжок на order_pos=2
    f.add_pattern({{make_cell()}}, 1);                        // паттерн 1: должен быть пропущен
    f.add_pattern({{make_cell()}}, 1);                        // паттерн 2: цель прыжка
    f.order = {0, 1, 2};
    f.finalize(1);
    f.song.default_speed = 1;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK(ps.pending_position_jump);
    CHECK_EQ(ps.position_jump_target, 2);

    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.order_pos, 2);
    CHECK_EQ(ps.pattern_idx, 2); // паттерн 1 пропущен целиком
}

void test_pattern_break_row_target() {
    std::printf("test_sequencer_pattern_break_row_target\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::PatternBreak, 2)}, {make_cell()}, {make_cell()}, {make_cell()}}, 1); // 4 строки, брейк на строке 0
    f.add_pattern({{make_cell()}, {make_cell()}, {make_cell()}, {make_cell()}, {make_cell()}}, 1);          // 5 строк
    f.order = {0, 1};
    f.finalize(1);
    f.song.default_speed = 1;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.pattern_idx, 1);
    CHECK_EQ(ps.row, 2); // сразу на строку 2 паттерна 1, строки 1-3 паттерна 0 пропущены
}

void test_position_jump_and_pattern_break_same_row() {
    std::printf("test_sequencer_position_jump_and_pattern_break_same_row\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::PositionJump, 2), make_cell(Effect::PatternBreak, 1)}}, 2);
    f.add_pattern({{make_cell()}}, 2);
    f.add_pattern({{make_cell()}, {make_cell()}, {make_cell()}}, 2);
    f.order = {0, 1, 2};
    f.finalize(2);
    f.song.default_speed = 1;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.order_pos, 2);
    CHECK_EQ(ps.pattern_idx, 2);
    CHECK_EQ(ps.row, 1); // строка из Pattern Break, паттерн - из Position Jump
}

void test_pattern_loop_per_channel() {
    std::printf("test_sequencer_pattern_loop_per_channel\n");
    Fixture f;
    // row0: поставить точку возврата; row1: пусто; row2: луп на 2 повтора.
    f.add_pattern({{make_cell(Effect::PatternLoop, 0)}, {make_cell()}, {make_cell(Effect::PatternLoop, 2)}}, 1);
    f.add_pattern({{make_cell()}}, 1); // следующий паттерн - увидеть, что доходим сюда только после исчерпания лупа
    f.order = {0, 1};
    f.finalize(1);
    f.song.default_speed = 1;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr)); // row0

    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row1
    CHECK_EQ(ps.row, 1);
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row2, читает SBx(2), заводит счётчик, прыгает
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // применение прыжка -> row0
    CHECK_EQ(ps.pattern_idx, 0);
    CHECK_EQ(ps.row, 0);
    CHECK_EQ(ps.loop_counter[0], 2);

    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row1
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row2, SBx(2) второй раз: counter 2->1, ещё прыгаем
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row0 снова
    CHECK_EQ(ps.row, 0);
    CHECK_EQ(ps.loop_counter[0], 1);

    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row1
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row2, SBx(2) третий раз: counter 1->0, луп исчерпан
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // естественный переход дальше -> паттерн 1
    CHECK_EQ(ps.pattern_idx, 1);
}

void test_pattern_loop_global_target() {
    std::printf("test_sequencer_pattern_loop_global_target\n");
    Fixture f;
    // Точка возврата и триггер лупа на разных каналах (0 и 1); при
    // kFlowLoopGlobalTarget это должно работать через один общий слот
    // (channels[0]), а не по отдельности на каждом канале.
    f.add_pattern({{make_cell(Effect::PatternLoop, 0), make_cell()},
                    {make_cell(), make_cell(Effect::PatternLoop, 1)}},
                   2);
    f.order = {0};
    f.finalize(2);
    f.song.default_speed = 1;
    f.song.flow_mode = soundsinth::model::kFlowLoopGlobalTarget;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr)); // row0 -> loop_start_row[0]
    CHECK_EQ(ps.loop_start_row[0], 0);

    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row1, SBx(1) на канале 1, но пишет в слот 0
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // применение прыжка -> row0
    CHECK_EQ(ps.row, 0);
    CHECK_EQ(ps.loop_counter[0], 1);
    CHECK_EQ(ps.loop_counter[1], 0); // канал 1 не тронут - всё через общий слот 0
}

// kFlowLoopDelaysSameRowBreak: PositionJump на той же строке, что и
// сработавший Pattern Loop, должен побеждать луп (см. sequencer.cpp и
// quirks.h, там сверка с OpenMPT). Два эффекта одновременно возможны
// только на разных каналах одной строки, у ячейки эффект один. Без флага,
// по умолчанию движка (луп всегда побеждает), тот же сценарий должен прыгнуть назад (луп), а не вперёд
// (джамп).
void test_pattern_loop_vs_position_jump_same_row() {
    std::printf("test_sequencer_pattern_loop_vs_position_jump_same_row\n");
    Fixture f;
    // row0 ch0: точка возврата лупа. row1: пусто. row2 ch0: SBx(2) (луп на
    // 2 повтора), row2 ch1: Bxx(1) (PositionJump на order 1) - оба на одной
    // строке, разные каналы.
    f.add_pattern({{make_cell(Effect::PatternLoop, 0), make_cell()},
                    {make_cell(), make_cell()},
                    {make_cell(Effect::PatternLoop, 2), make_cell(Effect::PositionJump, 1)}},
                   2);
    f.add_pattern({{make_cell(), make_cell()}}, 2); // order 1 - цель PositionJump
    f.order = {0, 1};
    f.finalize(2);
    f.song.default_speed = 1;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr)); // row0
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row1
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row2, читает оба эффекта
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // применение прыжка
    // Без флага, по умолчанию: луп побеждает, прыжок отброшен, возврат на
    // row0 того же паттерна.
    CHECK_EQ(ps.pattern_idx, 0);
    CHECK_EQ(ps.row, 0);

    // Тот же сценарий с флагом: джамп должен победить луп.
    Fixture g;
    g.add_pattern({{make_cell(Effect::PatternLoop, 0), make_cell()},
                    {make_cell(), make_cell()},
                    {make_cell(Effect::PatternLoop, 2), make_cell(Effect::PositionJump, 1)}},
                   2);
    g.add_pattern({{make_cell(), make_cell()}}, 2);
    g.order = {0, 1};
    g.finalize(2);
    g.song.default_speed = 1;
    g.song.flow_mode = soundsinth::model::kFlowLoopDelaysSameRowBreak;

    engine::PlayState ps2;
    CHECK(engine::sequencer_init(g.song, g.psram, ps2, nullptr, nullptr));
    CHECK(engine::sequencer_tick(g.song, g.psram, ps2, nullptr, nullptr));
    CHECK(engine::sequencer_tick(g.song, g.psram, ps2, nullptr, nullptr));
    CHECK(engine::sequencer_tick(g.song, g.psram, ps2, nullptr, nullptr));
    CHECK_EQ(ps2.pattern_idx, 1); // джамп победил - ушли на order 1, а не вернулись на row0
    CHECK_EQ(ps2.row, 0);
}

void test_set_speed_and_tempo_tick_duration() {
    std::printf("test_sequencer_set_speed_and_tempo_tick_duration\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::SetTempo, 140)}}, 1);
    f.order = {0, soundsinth::model::kOrderEnd};
    f.finalize(1);
    f.song.default_speed = 6;
    f.song.default_tempo = 125;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.tempo, 140); // применился сразу на первой строке, до первого tick()

    // Тик - фиксированное целое, округлённое к ближайшему (см. sequencer.h,
    // так же считает OpenMPT): 220500/280 = 787.5 -> 788, и таким остаётся
    // тик за тиком, без дробной части. Тик 0 строки 0 считает уже
    // sequencer_init.
    CHECK_EQ(ps.last_tick_samples, 788u);
    for (int i = 0; i < 1000; ++i) {
        CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
        CHECK_EQ(ps.last_tick_samples, 788u);
    }
}

// Темпы, на которых деление не точное: на них видно, что дробная часть
// отбрасывается при округлении, а не копится. Расхождение с OpenMPT было
// найдено на bpm=128: накопление давало -43 мс за две минуты (см.
// sequencer.h).
void test_tick_duration_rounds_to_nearest() {
    std::printf("test_sequencer_tick_duration_rounds_to_nearest\n");
    struct { uint16_t tempo; uint32_t samples; } cases[] = {
        {125, 882u}, // 882.000 без остатка
        {128, 861u}, // 861.328 -> вниз
        {140, 788u}, // 787.500 -> вверх (к ближайшему, полшага вверх)
        {150, 735u}, // 735.000 без остатка
        {32,  3445u}, // 3445.313 -> вниз, нижняя граница допустимого темпа
    };
    for (const auto& c : cases) {
        Fixture f;
        f.add_pattern({{make_cell()}}, 1);
        f.order = {0, soundsinth::model::kOrderEnd};
        f.finalize(1);
        f.song.default_speed = 6;
        f.song.default_tempo = c.tempo;

        engine::PlayState ps;
        CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
        CHECK_EQ(ps.last_tick_samples, c.samples);
    }
}

// Смена темпа посреди песни должна менять длительность тика со
// следующего тика, без переходного состояния.
void test_tempo_change_mid_song_updates_tick_duration() {
    std::printf("test_sequencer_tempo_change_mid_song_updates_tick_duration\n");
    Fixture f;
    std::vector<std::vector<PatternCell>> rows;
    rows.push_back({make_cell(Effect::SetTempo, 140)}); // row 0 - начальный темп
    rows.push_back({make_cell()});                      // row 1 - пустая
    rows.push_back({make_cell(Effect::SetTempo, 100)}); // row 2 - смена темпа
    rows.push_back({make_cell()});                      // row 3 - пустая
    f.add_pattern(rows, 1);
    f.order = {0, soundsinth::model::kOrderEnd};
    f.finalize(1);
    f.song.default_speed = 1; // 1 тик/строка - тик N доводит до строки N

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.tempo, 140);
    CHECK_EQ(ps.last_tick_samples, 788u);

    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> строка 1
    CHECK_EQ(ps.tempo, 140);
    CHECK_EQ(ps.last_tick_samples, 788u);

    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> строка 2, темп применён
    CHECK_EQ(ps.tempo, 100);
    CHECK_EQ(ps.last_tick_samples, 1103u); // тик 0 строки с Txx - уже новым темпом: 220500/200 = 1102.5 -> 1103

    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.last_tick_samples, 1103u);
}

// MOD (kQuirkModTempoOnSecondTick): новый темп - со второго тика строки, и на
// строке 0 тоже; при скорости 1 это тик следующей строки.
void test_mod_tempo_on_second_tick() {
    std::printf("test_sequencer_mod_tempo_on_second_tick\n");
    Fixture f;
    std::vector<std::vector<PatternCell>> rows;
    rows.push_back({make_cell(Effect::SetTempo, 140)});
    rows.push_back({make_cell()});
    rows.push_back({make_cell(Effect::SetTempo, 100)});
    rows.push_back({make_cell()});
    f.add_pattern(rows, 1);
    f.order = {0, soundsinth::model::kOrderEnd};
    f.finalize(1);
    f.song.default_speed = 1;
    f.song.default_tempo = 125;
    f.song.quirks = soundsinth::model::kQuirkModTempoOnSecondTick;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.tempo, 140);
    CHECK_EQ(ps.last_tick_samples, 882u); // тик 0 строки 0 - ещё темпом 125
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> строка 1
    CHECK_EQ(ps.last_tick_samples, 788u);
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> строка 2 с T=100
    CHECK_EQ(ps.last_tick_samples, 788u);
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> строка 3
    CHECK_EQ(ps.last_tick_samples, 1103u);
}

void test_pattern_delay() {
    std::printf("test_sequencer_pattern_delay\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::PatternDelay, 2)}, {make_cell()}}, 1);
    f.order = {0};
    f.finalize(1);
    f.song.default_speed = 1;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.pattern_delay_rows_left, 2);

    // 2 повтора строки 0 (не читая её заново), затем настоящий переход на row1.
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.row, 0);
    CHECK_EQ(ps.pattern_delay_rows_left, 1);
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.row, 0);
    CHECK_EQ(ps.pattern_delay_rows_left, 0);
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.row, 1);
}

void test_song_ended_degenerate_order() {
    std::printf("test_sequencer_song_ended_degenerate_order\n");
    Fixture f;
    f.add_pattern({{make_cell()}}, 1);
    f.order = {soundsinth::model::kOrderSkip, soundsinth::model::kOrderSkip};
    f.finalize(1, /*restart_position=*/0); // restart тоже ведёт на skip - играть вообще нечего

    engine::PlayState ps;
    CHECK(!engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK(ps.song_ended);
}

void dispatch_callback(void* user, const PatternCell* cells, uint8_t channel_count) {
    engine::dispatch_row_effects(static_cast<engine::DispatchContext*>(user), cells, channel_count);
}

// Панорама и громкость канала из заголовка - стартовые, эффекты строки 0
// ложатся поверх них. Строку 0 sequencer_init отыгрывает сам, поэтому
// channels_init - до него, иначе заголовок затирает Mxx первой строки.
void test_init_header_channel_state_under_row0_effects() {
    std::printf("test_sequencer_init_header_channel_state_under_row0_effects\n");
    Fixture f;
    PatternCell volcol_pan = make_cell();
    volcol_pan.volume.type = soundsinth::model::VolumeColumnType::SetPanning;
    volcol_pan.volume.param = 56;
    f.add_pattern({{make_cell(Effect::SetChannelVolume, 40), make_cell(Effect::SetPanning, 0xC0), make_cell(),
                    volcol_pan}},
                  4);
    f.order = {0};
    f.finalize(4);
    for (uint8_t c = 0; c < 4; ++c) {
        f.song.channel_pan[c] = 10;
        f.song.channel_volume[c] = 20;
    }

    engine::PlayState ps;
    engine::ChannelState ch[4];
    engine::DispatchContext ctx;
    ctx.channels = ch;
    ctx.song = &f.song;
    ctx.ps = &ps;
    engine::channels_init(f.song, ch);
    CHECK(engine::sequencer_init(f.song, f.psram, ps, dispatch_callback, &ctx));
    CHECK_EQ(ch[0].channel_volume, 40); // Mxx строки 0
    CHECK_EQ(ch[0].pan, 10);
    // Xxx и панорама колонки громкости в канале без ноты - тоже поверх
    // заголовка: панорама канала держится до ноты.
    CHECK_EQ(ch[1].pan, 48);            // 0xC0 / 4
    CHECK_EQ(ch[3].pan, 56);
    CHECK_EQ(ch[2].pan, 10);            // без эффектов - заголовок
    CHECK_EQ(ch[2].channel_volume, 20);

    // Разводка Paula у MOD - вместо панорамы заголовка.
    f.song.quirks |= soundsinth::model::kQuirkModHardwarePanning;
    engine::channels_init(f.song, ch);
    CHECK(engine::sequencer_init(f.song, f.psram, ps, dispatch_callback, &ctx));
    CHECK_EQ(ch[0].pan, 0);
    CHECK_EQ(ch[1].pan, 48);
    CHECK_EQ(ch[2].pan, 64);
    CHECK_EQ(ch[0].channel_volume, 40);
}

// Длительность одного прохода: повтор строки Pattern Delay и возврат Pattern
// Loop - не конец трека. Тик при tempo 125 - 882 отсчёта.
uint32_t duration_of(Fixture& f, engine::DurationStats* stats = nullptr) {
    std::vector<uint64_t> scratch((engine::kDurationScratchBytes + 7) / 8);
    return engine::compute_song_total_frames(f.song, f.psram, reinterpret_cast<uint8_t*>(scratch.data()),
                                             engine::kDurationScratchBytes, stats);
}

// Сколько кадров секвенсор отыгрывает до возврата на строку 0 позиции 0:
// тик 0 строки 0 (посчитан в sequencer_init) и все тики до чтения строки
// повтора, её тик уже не считается.
uint32_t engine_pass_frames(Fixture& f) {
    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    uint32_t frames = ps.last_tick_samples;
    for (uint32_t guard = 0; guard < 100000; ++guard) {
        if (!engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)) break;
        if (ps.tick_in_row == 0 && ps.order_pos == 0 && ps.row == 0) break;
        frames += ps.last_tick_samples;
    }
    return frames;
}

// Длина прохода - ровно первый проход, как у OpenMPT GetLength, и когда
// темп на конце не равен темпу тика 0 строки 0.
void test_duration_equals_first_pass() {
    std::printf("test_sequencer_duration_equals_first_pass\n");
    {
        // Txx на последней строке, строка 0 без Txx: у строки повтора свой
        // (замедленный) тик, в проход он не входит.
        Fixture f;
        f.add_pattern({{make_cell()}, {make_cell()}, {make_cell()}, {make_cell(Effect::SetTempo, 60)}}, 1);
        f.order = {0};
        f.finalize(1);
        f.song.default_speed = 1;
        f.song.default_tempo = 125;
        const uint32_t pass = engine_pass_frames(f);
        std::printf("  Txx 60 в конце: проход %u, движок %u\n", duration_of(f), pass);
        CHECK_EQ(duration_of(f), pass);
        CHECK_EQ(pass, 3u * 882u + 1838u);
    }
    {
        // MOD: тик 0 строки 0 - прежним темпом 125, Fxx строки 0 - со второго
        // тика; на конце темп 130.
        Fixture f;
        f.add_pattern({{make_cell(Effect::SetTempo, 130)}, {make_cell()}, {make_cell()}}, 1);
        f.order = {0};
        f.finalize(1);
        f.song.default_speed = 3;
        f.song.default_tempo = 125;
        f.song.quirks = soundsinth::model::kQuirkModTempoOnSecondTick;
        CHECK_EQ(duration_of(f), engine_pass_frames(f));
    }
    {
        // Смена темпа посреди прохода и speed больше 1.
        Fixture f;
        f.add_pattern({{make_cell()}, {make_cell()}, {make_cell(Effect::SetTempo, 200)}, {make_cell()}}, 1);
        f.order = {0};
        f.finalize(1);
        f.song.default_speed = 4;
        f.song.default_tempo = 90;
        CHECK_EQ(duration_of(f), engine_pass_frames(f));
    }
}

// Переход назад на ещё не игранную позицию - признак для вытеснения; Bxx на
// пройденную (петля в конце песни) - нет.
void test_duration_position_goes_back() {
    std::printf("test_sequencer_duration_position_goes_back\n");
    {
        // order: 0 -> B02 на order 0 уводит на 2; order 2 - B01 на
        // непройденную позицию 1; order 1 кончается, песня идёт на 2 и
        // повторяется.
        Fixture f;
        f.add_pattern({{make_cell(Effect::PositionJump, 2)}}, 1);
        f.add_pattern({{make_cell()}}, 1);
        f.add_pattern({{make_cell(Effect::PositionJump, 1)}}, 1);
        f.order = {0, 1, 2};
        f.finalize(1);
        f.song.default_speed = 1;
        engine::DurationStats st;
        duration_of(f, &st);
        CHECK(st.position_goes_back);
    }
    {
        // Три позиции подряд, B00 в конце - петля на пройденное.
        Fixture f;
        f.add_pattern({{make_cell()}}, 1);
        f.add_pattern({{make_cell(Effect::PositionJump, 0)}}, 1);
        f.order = {0, 0, 1};
        f.finalize(1);
        f.song.default_speed = 1;
        engine::DurationStats st;
        duration_of(f, &st);
        CHECK(!st.position_goes_back);
        CHECK(st.stop == engine::DurationStop::Repeat);
    }
}

void test_duration_pattern_delay_and_loop() {
    std::printf("test_sequencer_duration_pattern_delay_and_loop\n");
    {
        // SEx(2) на строке 1 из 4, speed 6: 6 + 18 + 6 + 6 тиков.
        Fixture f;
        f.add_pattern({{make_cell()}, {make_cell(Effect::PatternDelay, 2)}, {make_cell()}, {make_cell()}}, 1);
        f.order = {0};
        f.finalize(1);
        f.song.default_speed = 6;
        f.song.default_tempo = 125;
        engine::DurationStats st;
        CHECK_EQ(duration_of(f, &st), 31752u);
        CHECK(st.stop == engine::DurationStop::Repeat);
        CHECK_EQ(st.stop_row, 0);
    }
    {
        // SB0 на строке 0, SB1 на строке 2: строки 0-2 дважды и строка 3.
        Fixture f;
        f.add_pattern({{make_cell(Effect::PatternLoop, 0)}, {make_cell()}, {make_cell(Effect::PatternLoop, 1)},
                       {make_cell()}},
                      1);
        f.order = {0};
        f.finalize(1);
        f.song.default_speed = 6;
        f.song.default_tempo = 125;
        CHECK_EQ(duration_of(f), 37044u);
    }
    {
        // E60, E61, E61 в одном канале: луп не кончается, отметки тела
        // снимаются - проход кончается пределом строк на позицию.
        Fixture f;
        f.add_pattern({{make_cell(Effect::PatternLoop, 0)}, {make_cell(Effect::PatternLoop, 1)},
                       {make_cell(Effect::PatternLoop, 1)}},
                      1);
        f.order = {0};
        f.finalize(1);
        f.song.default_speed = 1;
        f.song.default_tempo = 125;
        engine::DurationStats st;
        duration_of(f, &st);
        CHECK(st.stop == engine::DurationStop::RowLimit);
    }
}

// speed + сумма S6x по каналам больше 255: строка кончается на своём тике,
// счётчик тиков не заворачивается.
void test_row_longer_than_255_ticks() {
    std::printf("test_sequencer_row_longer_than_255_ticks\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::FinePatternDelay, 15)}, {make_cell()}}, 1);
    f.order = {0};
    f.finalize(1);
    f.song.default_speed = 241;

    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    uint32_t ticks = 0;
    while (ps.row == 0 && ticks < 1000) {
        CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
        ++ticks;
    }
    CHECK_EQ(ticks, 256u);
    CHECK_EQ(ps.row, 1);
}

// Тиков, пока строка row не сменится (повторы Pattern Delay - та же строка).
uint32_t ticks_until_row_leaves(Fixture& f, engine::PlayState& ps, uint16_t row) {
    uint32_t ticks = 0;
    while (ps.row == row && ticks < 10000) {
        CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
        ++ticks;
    }
    return ticks;
}

// S6x разных каналов одной строки суммируются, держатся через повторы SEx и
// сбрасываются на новой строке.
void test_fine_pattern_delay_sum_and_reset() {
    std::printf("test_sequencer_fine_pattern_delay_sum_and_reset\n");
    {
        Fixture f;
        f.add_pattern({{make_cell(Effect::FinePatternDelay, 3), make_cell(Effect::FinePatternDelay, 2)},
                       {make_cell(), make_cell()},
                       {make_cell(), make_cell()}},
                      2);
        f.order = {0};
        f.finalize(2);
        f.song.default_speed = 3;
        engine::PlayState ps;
        CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
        CHECK_EQ(ticks_until_row_leaves(f, ps, 0), 8u); // 3 + 3 + 2
        CHECK_EQ(ticks_until_row_leaves(f, ps, 1), 3u);
    }
    {
        Fixture f;
        f.add_pattern({{make_cell(Effect::FinePatternDelay, 2), make_cell(Effect::PatternDelay, 1)},
                       {make_cell(), make_cell()},
                       {make_cell(), make_cell()}},
                      2);
        f.order = {0};
        f.finalize(2);
        f.song.default_speed = 3;
        engine::PlayState ps;
        CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
        CHECK_EQ(ticks_until_row_leaves(f, ps, 0), 10u); // (3 + 2) x 2
        CHECK_EQ(ticks_until_row_leaves(f, ps, 1), 3u);
    }
}

// Pattern Break за концом строк целевого паттерна - его последняя строка.
void test_pattern_break_clamped_to_last_row() {
    std::printf("test_sequencer_pattern_break_clamped_to_last_row\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::PatternBreak, 40)}}, 1);
    f.add_pattern(std::vector<std::vector<PatternCell>>(16, std::vector<PatternCell>{make_cell()}), 1);
    f.order = {0, 1};
    f.finalize(1);
    f.song.default_speed = 1;
    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.pattern_idx, 1);
    CHECK_EQ(ps.row, 15);
}

// Маркер и битый индекс паттерна посреди order пропускаются; Position Jump
// за order_count уходит на restart_position.
void test_order_skip_bad_index_and_jump_past_end() {
    std::printf("test_sequencer_order_skip_bad_index_and_jump_past_end\n");
    {
        Fixture f;
        f.add_pattern({{make_cell()}}, 1);
        f.add_pattern({{make_cell()}}, 1);
        f.order = {0, soundsinth::model::kOrderSkip, 7, 1};
        f.finalize(1);
        f.song.default_speed = 1;
        engine::PlayState ps;
        CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
        CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
        CHECK_EQ(ps.order_pos, 3);
        CHECK_EQ(ps.pattern_idx, 1);
    }
    {
        Fixture f;
        f.add_pattern({{make_cell(Effect::PositionJump, 9)}}, 1);
        f.add_pattern({{make_cell()}}, 1);
        f.add_pattern({{make_cell()}}, 1);
        f.order = {0, 1, 2};
        f.finalize(1, /*restart_position=*/1);
        f.song.default_speed = 1;
        engine::PlayState ps;
        CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
        CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr));
        CHECK_EQ(ps.order_pos, 1);
        CHECK_EQ(ps.pattern_idx, 1);
    }
}

// A00 и темп ниже kMinTempo не меняют скорость и темп.
void test_zero_speed_and_low_tempo_ignored() {
    std::printf("test_sequencer_zero_speed_and_low_tempo_ignored\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::SetSpeed, 0), make_cell(Effect::SetTempo, 0x1F)}}, 2);
    f.order = {0};
    f.finalize(2);
    f.song.default_speed = 4;
    f.song.default_tempo = 125;
    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK_EQ(ps.speed, 4);
    CHECK_EQ(ps.tempo, 125);
}

// kFlowLoopDelaysSameRowBreak: Pattern Break без Position Jump на строке
// сработавшего Pattern Loop на исход не влияет - возврат на точку петли.
void test_loop_with_break_under_delays_flag() {
    std::printf("test_sequencer_loop_with_break_under_delays_flag\n");
    Fixture f;
    f.add_pattern({{make_cell(Effect::PatternLoop, 0), make_cell()},
                   {make_cell(), make_cell()},
                   {make_cell(Effect::PatternLoop, 2), make_cell(Effect::PatternBreak, 0)}},
                  2);
    f.add_pattern({{make_cell(), make_cell()}}, 2);
    f.order = {0, 1};
    f.finalize(2);
    f.song.default_speed = 1;
    f.song.flow_mode = soundsinth::model::kFlowLoopDelaysSameRowBreak;
    engine::PlayState ps;
    CHECK(engine::sequencer_init(f.song, f.psram, ps, nullptr, nullptr));
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row1
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // -> row2
    CHECK(engine::sequencer_tick(f.song, f.psram, ps, nullptr, nullptr)); // петля
    CHECK_EQ(ps.pattern_idx, 0);
    CHECK_EQ(ps.row, 0);
}

} // namespace

void run_sequencer_tests() {
    test_row_longer_than_255_ticks();
    test_duration_pattern_delay_and_loop();
    test_duration_equals_first_pass();
    test_duration_position_goes_back();
    test_sequential_no_effects();
    test_position_jump();
    test_pattern_break_row_target();
    test_position_jump_and_pattern_break_same_row();
    test_pattern_loop_per_channel();
    test_pattern_loop_global_target();
    test_pattern_loop_vs_position_jump_same_row();
    test_set_speed_and_tempo_tick_duration();
    test_tick_duration_rounds_to_nearest();
    test_tempo_change_mid_song_updates_tick_duration();
    test_mod_tempo_on_second_tick();
    test_pattern_delay();
    test_song_ended_degenerate_order();
    test_init_header_channel_state_under_row0_effects();
    test_fine_pattern_delay_sum_and_reset();
    test_pattern_break_clamped_to_last_row();
    test_order_skip_bad_index_and_jump_past_end();
    test_zero_speed_and_low_tempo_ignored();
    test_loop_with_break_under_delays_flag();
}
