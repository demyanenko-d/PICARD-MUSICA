#include "core/engine/sequencer.h"

#include "platform/hot_path.h"
#include "core/codec/pattern_reader.h"

namespace soundsinth::engine {

using soundsinth::model::Effect;
using soundsinth::model::Pattern;
using soundsinth::model::PatternCell;
using soundsinth::model::Song;

namespace {

struct ResolveResult {
    bool found = false;
    uint16_t order_pos = 0;
};

// Следующая воспроизводимая позиция order начиная с start_pos
// (включительно): пропускает kOrderSkip и битые индексы паттерна, на
// kOrderEnd и за концом order уходит на restart_position. Не больше
// order_count+1 попыток: у вырожденного файла без воспроизводимых
// позиций - "не найдено", а не зависание.
ResolveResult resolve_next_order_pos(const Song& song, uint16_t start_pos) {
    if (song.order_count == 0) return {};
    uint16_t pos = start_pos;
    for (uint32_t guard = 0; guard <= static_cast<uint32_t>(song.order_count); ++guard) {
        if (pos >= song.order_count) {
            pos = song.restart_position;
            continue;
        }
        const uint16_t pat = song.order[pos];
        if (pat == soundsinth::model::kOrderEnd) {
            pos = song.restart_position;
            continue;
        }
        if (pat == soundsinth::model::kOrderSkip || pat >= song.pattern_count) {
            ++pos;
            continue;
        }
        return {true, pos};
    }
    return {};
}

// Читает строку ps.row текущего паттерна из PSRAM (PatternReader),
// отдаёт её через on_new_row и разбирает в ней только эффекты хода
// воспроизведения, остальные пропускает. Выставляет pending_* (применяет
// advance_row_or_pattern), speed, frame_delay, pattern_delay_rows_left
// (читает sequencer_tick) и tempo (compute_tick_samples этого же тика, у MOD
// - со второго). Вызывается на каждой смене строки из sequencer_tick, на
// горячем пути тика: в SRAM, не делит QMI с кодом из флеша.
void SOUNDSINTH_HOT_PATH(read_row_and_scan)(const Song& song, memory::PsramStore& psram, PlayState& ps,
                                            RowCallback on_new_row, void* user) {
    const Pattern& pat = song.patterns[ps.pattern_idx];
    PatternCell cells[soundsinth::model::kMaxPatternChannels];

    if (song.row_fetch != nullptr) {
        // Строку делает источник (у .mid - конвертер из файла): в зоне
        // паттернов её нет вовсе.
        for (uint8_t ch = 0; ch < pat.channel_count; ++ch) {
            cells[ch] = PatternCell{};
        }
        song.row_fetch(song.row_fetch_user, ps.pattern_idx, ps.row, cells, pat.channel_count);
    } else if (pat.psram_offset != Pattern::kInvalidOffset && pat.channel_count > 0) {
        patterns::PatternReader reader(memory::psram_pattern_ptr(psram, pat.psram_offset), pat.row_count,
                                       pat.channel_count);
        reader.read_row(ps.row, cells);
    } else {
        for (uint8_t ch = 0; ch < pat.channel_count; ++ch) {
            cells[ch] = PatternCell{};
        }
    }

    if (on_new_row) on_new_row(user, cells, pat.channel_count);

    // Сбрасывается только здесь, при переходе на новую строку; повторы
    // Pattern Delay read_row_and_scan не вызывают.
    ps.frame_delay = 0;

    const bool global_loop_target = (song.flow_mode & soundsinth::model::kFlowLoopGlobalTarget) != 0;

    for (uint8_t ch = 0; ch < pat.channel_count; ++ch) {
        const auto& effect = cells[ch].effect;
        switch (effect.type) {
            case Effect::PositionJump:
                ps.pending_position_jump = true;
                ps.position_jump_target = effect.param;
                break;
            case Effect::PatternBreak:
                ps.pending_pattern_break = true;
                ps.pattern_break_row = effect.param; // BCD S3M уже развёрнут загрузчиком
                break;
            case Effect::PatternLoop: {
                const uint8_t loop_ch = global_loop_target ? 0 : ch;
                uint8_t& counter = ps.loop_counter[loop_ch];
                if (effect.param == 0) {
                    ps.loop_start_row[loop_ch] = ps.row; // точка возврата, не прыгать
                    break;
                }
                // Первый заход заводит счётчик, следующие его уменьшают; на нуле
                // Pattern Loop исчерпан, дальше как обычно.
                counter = (counter == 0) ? effect.param : static_cast<uint8_t>(counter - 1u);
                if (counter > 0) {
                    ps.pending_pattern_loop = true;
                    ps.pattern_loop_channel = loop_ch;
                }
                break;
            }
            case Effect::SetSpeed:
                if (effect.param > 0) ps.speed = effect.param;
                break;
            case Effect::SetTempo:
                // Длительность тика пересчитывает sequencer_tick: с тика 0
                // этой строки, у MOD - со второго (kQuirkModTempoOnSecondTick).
                // Тик - целое, переносить дробь между темпами не нужно.
                if (effect.param >= soundsinth::model::kMinTempo) ps.tempo = effect.param;
                break;
            case Effect::PatternDelay:
                if (effect.param > 0) ps.pattern_delay_rows_left = effect.param;
                break;
            case Effect::FinePatternDelay:
                // S6x/X6x разных каналов одной строки суммируются.
                ps.frame_delay += effect.param;
                break;
            default:
                break; // остальные эффекты здесь не разбираются
        }
    }
}

// Переходы строки применены или отброшены.
void clear_pending_flow(PlayState& ps) {
    ps.pending_position_jump = false;
    ps.pending_pattern_break = false;
    ps.pending_pattern_loop = false;
}

// Строка row в паттерне из row_count строк; за концом - последняя.
uint16_t clamp_row(uint16_t row, uint16_t row_count) {
    if (row < row_count) return row;
    return row_count > 0 ? static_cast<uint16_t>(row_count - 1u) : 0;
}

// Тики строки истекли, Pattern Delay больше не держит: следующая строка или
// паттерн. false - воспроизводимой позиции нет, song_ended.
bool SOUNDSINTH_HOT_PATH(advance_row_or_pattern)(const Song& song, PlayState& ps) {
    uint16_t next_pattern_idx = ps.pattern_idx;
    uint16_t next_row;

    // kFlowLoopDelaysSameRowBreak: Position Jump на строке сработавшего
    // Pattern Loop побеждает его (петля в этот раз не срабатывает). Pattern
    // Break без Position Jump на исход не влияет.
    const bool jump_overrides_loop =
        (song.flow_mode & soundsinth::model::kFlowLoopDelaysSameRowBreak) != 0 && ps.pending_position_jump;
    const bool loop = ps.pending_pattern_loop && !jump_overrides_loop;
    if (loop) {
        next_row = ps.loop_start_row[ps.pattern_loop_channel];
    } else {
        const uint16_t row_count = song.patterns[ps.pattern_idx].row_count;
        const bool natural_pattern_end = static_cast<uint32_t>(ps.row) + 1 >= row_count;
        if (ps.pending_pattern_break || natural_pattern_end) {
            const uint16_t target_start =
                ps.pending_position_jump ? ps.position_jump_target : static_cast<uint16_t>(ps.order_pos + 1);
            const ResolveResult resolved = resolve_next_order_pos(song, target_start);
            if (!resolved.found) {
                ps.song_ended = true;
                clear_pending_flow(ps);
                return false;
            }
            ps.order_pos = resolved.order_pos;
            next_pattern_idx = song.order[ps.order_pos];
            next_row = ps.pending_pattern_break
                           ? clamp_row(ps.pattern_break_row, song.patterns[next_pattern_idx].row_count)
                           : 0;
        } else {
            next_row = static_cast<uint16_t>(ps.row + 1);
        }
    }

    ps.pattern_idx = next_pattern_idx;
    ps.row = next_row;
    ps.last_advance_was_loop = loop;
    clear_pending_flow(ps);
    return true;
}

// Длительность тика в отсчётах. Зовётся ровно раз на тик, включая тик 0
// строки 0: на тике с новой строкой - после её чтения (Txx с тика 0), у MOD -
// до (со второго тика).
void SOUNDSINTH_HOT_PATH(compute_tick_samples)(PlayState& ps) {
    const uint32_t denom = static_cast<uint32_t>(ps.tempo) * 2u;
    ps.last_tick_samples = (kSampleRateHz * 5u + denom / 2u) / denom; // к ближайшему
}

} // namespace

void sequencer_live_tick(PlayState& ps) {
    ps.tick_in_row = 0; // тик и есть строка
    ++ps.row;
    compute_tick_samples(ps);
}

bool sequencer_init(const Song& song, memory::PsramStore& psram, PlayState& ps, RowCallback on_new_row, void* user) {
    ps = PlayState{};
    ps.speed = (song.default_speed == 0 || song.default_speed > UINT8_MAX) ? soundsinth::model::kDefaultSpeed
                                                                           : static_cast<uint8_t>(song.default_speed);
    ps.tempo = song.default_tempo >= soundsinth::model::kMinTempo ? song.default_tempo : soundsinth::model::kDefaultTempo;
    ps.global_volume = song.default_global_volume;

    const ResolveResult resolved = resolve_next_order_pos(song, 0);
    if (!resolved.found) {
        ps.song_ended = true;
        return false;
    }
    ps.order_pos = resolved.order_pos;
    ps.pattern_idx = song.order[ps.order_pos];
    ps.row = 0;
    // Тик 0 строки 0 - такой же тик, как прочие: его длительность считается
    // здесь, чтобы движок его отыграл, а не проскочил (иначе строка 0 на тик
    // короче, и весь трек уезжает на 20 мс вперёд относительно трекеров).
    const bool tempo_on_second_tick = (song.quirks & soundsinth::model::kQuirkModTempoOnSecondTick) != 0;
    if (tempo_on_second_tick) compute_tick_samples(ps);
    read_row_and_scan(song, psram, ps, on_new_row, user);
    if (!tempo_on_second_tick) compute_tick_samples(ps);
    return true;
}

// Каждый тик воспроизведения (13-102 в секунду): в SRAM, не делит QMI с
// кодом из флеша.
bool SOUNDSINTH_HOT_PATH(sequencer_tick)(const Song& song, memory::PsramStore& psram, PlayState& ps,
                                         RowCallback on_new_row, void* user) {
    if (ps.song_ended) return false;

    ++ps.tick_in_row;
    // speed + frame_delay (FinePatternDelay S6x/X6x), а не только speed
    if (ps.tick_in_row < static_cast<uint32_t>(ps.speed) + ps.frame_delay) {
        compute_tick_samples(ps);
        return true;
    }
    ps.tick_in_row = 0;

    if (ps.pattern_delay_rows_left > 0) {
        --ps.pattern_delay_rows_left;
        compute_tick_samples(ps);
        return true; // повтор текущей строки без повторного чтения
    }

    // Тик 0 новой строки: у S3M, XM, IT - темпом этой строки, у MOD - прежним.
    const bool tempo_on_second_tick = (song.quirks & soundsinth::model::kQuirkModTempoOnSecondTick) != 0;
    if (tempo_on_second_tick) compute_tick_samples(ps);
    const bool advanced = advance_row_or_pattern(song, ps);
    if (advanced) read_row_and_scan(song, psram, ps, on_new_row, user);
    if (!tempo_on_second_tick) compute_tick_samples(ps);
    return advanced;
}

} // namespace soundsinth::engine
