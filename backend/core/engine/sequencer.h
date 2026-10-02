// SPDX-License-Identifier: MIT
#pragma once

// Тиковый секвенсор: движение по order-листу, паттернам и строкам в тиках.
// Здесь обрабатывается только то, что меняет ход воспроизведения: Position
// Jump, Pattern Break, Pattern Loop, Set Speed/Set Tempo, Pattern Delay,
// Fine Pattern Delay. Остальные эффекты строки уходят вызывающему через
// on_new_row как есть.
//
// Тик - 110250/tempo отсчётов, округлён к ближайшему целому, дробь не
// копится: так считают трекеры в классическом режиме темпа.

#include <cstdint>

#include "core/engine/engine_defs.h"
#include "core/model/song.h"
#include "core/memory/psram_store.h"

namespace soundsinth::engine {

struct PlayState {
    uint16_t order_pos    = 0; // индекс в Song::order
    uint16_t pattern_idx  = 0; // кэш Song::order[order_pos], реальный индекс паттерна (не kOrderEnd/kOrderSkip)
    uint16_t row          = 0;
    uint16_t tick_in_row  = 0;                                // 0..speed + frame_delay - 1: до 255 + 15 x 64
    uint8_t speed         = soundsinth::model::kDefaultSpeed; // тиков на строку, меняет Set Speed
    uint16_t tempo        = soundsinth::model::kDefaultTempo; // BPM, меняет Set Tempo
    uint8_t global_volume = kGlobalVolumeMax;
    // SetGlobalVolume/GlobalVolumeSlide (S3M Vxx/Wxy, XM Gxx/Hxy, IT Vxx/Wxy):
    // шкала 0..128, как у global_volume и Song::default_global_volume. S3M и
    // XM приводят свои 0..64 к ней: Vxx/Gxx - при декоде эффекта, шаг слайда
    // - диспетчер. Память - сырой параметр, шаг со знаком - на строке.
    uint8_t global_volume_slide_memory = 0;
    int8_t global_volume_slide_step    = 0;
    bool global_volume_slide_active    = false;

    uint32_t last_tick_samples = 0; // сколько отсчётов покрыл последний тик

    // Переходы, найденные при чтении строки, применяются один раз, когда
    // истекут все тики строки (advance_row_or_pattern). Сработавший Pattern
    // Loop побеждает Position Jump и Pattern Break той же строки; при
    // kFlowLoopDelaysSameRowBreak побеждает Position Jump.
    // kFlowLoopNoBreakJump не обрабатывается.
    bool pending_position_jump    = false;
    uint16_t position_jump_target = 0; // order_pos
    bool pending_pattern_break    = false;
    uint8_t pattern_break_row     = 0; // прижимается к row_count-1 целевого паттерна при применении
    bool pending_pattern_loop     = false;
    uint8_t pattern_loop_channel  = 0; // какой канал вызвал прыжок - для чтения loop_start_row

    uint8_t pattern_delay_rows_left = 0; // Pattern Delay (SEx/EEx): повторить текущую строку N раз, не читая её заново

    // FinePatternDelay (S3M/IT S6x, MPT X6x): тики к строке, S6x разных
    // каналов одной строки суммируются; длина строки (speed + frame_delay) *
    // max(pattern_delay, 1). Сбрасывается только на новой строке.
    uint16_t frame_delay = 0;

    bool song_ended = false; // не нашлось воспроизводимой позиции order, вызывающему остановиться
    bool last_advance_was_loop = false; // текущая строка - цель сработавшего Pattern Loop

    // Pattern Loop (SBx/E6x): цель и счётчик на канал паттерна. При
    // Song::flow_mode & kFlowLoopGlobalTarget используется только индекс 0, с
    // какого бы канала ни пришла команда: так сделан общий на все каналы
    // Pattern Loop ST3 и старого IT.
    uint16_t loop_start_row[soundsinth::model::kMaxPatternChannels] = {};
    uint8_t loop_counter[soundsinth::model::kMaxPatternChannels]    = {}; // 0 = Pattern Loop не идёт
};

// cells/channel_count - строка, только что вступившая в силу
// (row == PlayState::row после перехода), сырые ячейки. user - указатель
// из sequencer_init/sequencer_tick как есть.
using RowCallback = void (*)(void* user, const soundsinth::model::PatternCell* cells, uint8_t channel_count);

// Готовит PlayState к первому тику: находит первую воспроизводимую позицию
// order (пропуская kOrderSkip), читает её первую строку и вызывает
// on_new_row. Состояние каналов диспетчера секвенсор не трогает: его
// начальные значения ставит channels_init до этого вызова - эффекты строки 0
// ложатся поверх них. false - воспроизводимых позиций нет (song_ended = true).
// Живой режим: порядка, паттернов и длины трека нет, строка = тик. Двигает
// счётчики строки и считает длительность тика; сами ячейки даёт вызывающий.
void sequencer_live_tick(PlayState& ps);

bool sequencer_init(const soundsinth::model::Song& song, memory::PsramStore& psram, PlayState& ps, RowCallback on_new_row, void* user);

// Один тик. true - тик состоялся (если строка кончилась, секвенсор уже
// перешёл на новую и on_new_row вызван). false - песня кончилась:
// song_ended был true до вызова или стал из-за вырожденного order-листа.
bool sequencer_tick(const soundsinth::model::Song& song, memory::PsramStore& psram, PlayState& ps, RowCallback on_new_row, void* user);

} // namespace soundsinth::engine
