#pragma once

// Сеанс трека: единственный владелец Callbacks у HostProtocol. Крутится на
// том ядре, которое обслуживает связь с хостом.
//   - ждёт 0x02 (сессия) -> строит BusByteSource (окно - буфер приёма
//     протокола) -> грузит трек по фазам:
//       A. run_session_load(metadata_only): заголовки, паттерны,
//          дескрипторы, без PCM;
//       B. player::load::plan_playback_order(): порядок загрузки сэмплов;
//       C. префетч: сэмплы первых позиций order-листа;
//       D. g_playback_frames = 0,
//          shared::g_song_generation++, protocol.mark_session_ready():
//          здесь стартует звук (телеметрия до этого отложена, "0 длина /
//          0 время" не бывает);
//       E. хвост плана догружается фоном по одному сэмплу за проход
//          внешнего цикла, под играющий звук.
//     Всё на Core1 синхронно, межъядерных блокировок не нужно. Сэмпл, до
//     которого фон не дошёл, молчит (sample_cache_find -> nullptr ->
//     voice_trigger голос не запускает);
//   - раз в секунду, пока трек играет, передаёт позицию из
//     shared::g_playback_frames в protocol.set_position(), по концу - Ended.
//
// on_session_start только взводит флаг: он вызывается изнутри
// HostProtocol::poll(), а загрузка сама крутит poll(), пока ждёт данных.
// Загрузка стартует с верхнего уровня цикла session_orchestrator_run().

#include <cstdint>

#include "player/load/bus_byte_source.h"
#include "player/protocol/host_protocol.h"
#include "player/load/progressive_loader.h"
#include "player/load/sample_prefetch.h"
#include "player/load/session_loader.h"
#include "player/config.h"

namespace player {

struct SessionOrchestrator {
    explicit SessionOrchestrator(player::protocol::HostProtocol& p) : protocol(p) {}

    player::protocol::HostProtocol& protocol;
    bool session_pending = false;
    bool aborted = false;
    uint32_t pending_file_length = 0;
    // Стратегию обхода файла выбирает хост (параметр команды начала
    // сессии): Wild Commander платит за прыжок назад перечитыванием файла с
    // начала, TR-DOS переходит на любое место без перечитывания.
    player::load::LoadOrder pending_load_order = player::load::LoadOrder::ByFile;
    // Не null, пока жива сессия, а не только пока разбираются метаданные:
    // фоновая догрузка тянет данные уже после старта звука тем же
    // источником. on_reset передаётся сюда.
    player::load::BusByteSource* current_bus_source = nullptr;
    alignas(player::load::BusByteSource) unsigned char bus_source_storage[sizeof(player::load::BusByteSource)];

    bool playing = false; // трек загружен и мог начать играть (звать ли set_position)
    bool host_here = true; // клиент хоста опрашивает порт состояния (для строки лога)
    // Итог связи этой сессии уже в журнале (конец плана или отказ); нет -
    // его печатает старт следующей сессии. До первой сессии печатать нечего.
    bool link_reported = true;
    // Самый долгий шаг фоновой догрузки сессии и его сэмпл - в строку конца
    // плана.
    uint32_t longest_step_us = 0;
    uint16_t longest_step_sample = 0;

    // Итог фазы A сессии: формат, длительность прохода (по ней виден конец
    // трека), причины отказа каждого загрузчика - для отчёта о провале. В
    // объекте, а не на стеке Core1.
    player::load::SessionLoadResult load_result;
    // План и состояние фоновой догрузки: очередь сэмплов, их первые и
    // последние позиции, упреждение (lead_positions - из
    // SOUNDSINTH_MIDI_PREFETCH_SECONDS, только у .mid), счётчики отказов и
    // вытеснений.
    player::load::ProgressiveLoader plan;
};

// Колбэки протокола: их ставит порт, когда поднимает связь.
player::protocol::HostProtocol::Callbacks session_orchestrator_callbacks(SessionOrchestrator& orch);

[[noreturn]] void session_orchestrator_run(SessionOrchestrator& orch);

// Длительность (выходные отсчёты) и счётчики трека - хосту, file_info.
// Звать после успешной run_session_load, до mark_session_ready: хост
// показывает их сразу после разбора, до первого звука. Зовут оркестратор и
// мост GS (модуль через плеер GS показывается так же).
void publish_file_info(uint32_t total_frames, const soundsinth::model::Song& song);

// Обслуживание карты и протокола без ожидания - насос для долгих работ
// Core1, где ждать нечего (распаковка сэмпла банка, снос движка перед
// разбором модуля GS): __wfi усыпил бы Core1 до следующего обращения хоста.
void serve_without_wait(void*);

} // namespace player
