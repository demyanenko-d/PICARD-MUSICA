#include "player/shared_state.h"

namespace shared {

soundsinth::memory::TrackMemory g_track_memory;
soundsinth::bank::Bank g_bank;
soundsinth::bank::Bank g_flash_bank;
soundsinth::model::Song g_song;
std::atomic<uint32_t> g_song_generation{0};
std::atomic<uint32_t> g_stale_generation{0};
std::atomic<uint32_t> g_app_task_loops{0};
std::atomic<uint32_t> g_engine_generation{0};
std::atomic<uint32_t> g_core1_loops{0};
std::atomic<Core1Phase> g_core1_phase{Core1Phase::Loop};
std::atomic<uint32_t> g_track_end_frame{0};
std::atomic<uint32_t> g_playback_frames{0};
std::atomic<bool> g_teardown_requested{false};
std::atomic<bool> g_song_ended{false};
std::atomic<bool> g_playback_finished{false};
std::atomic<bool> g_playback_paused{false};
std::atomic<uint32_t> g_seek_frames{0};
std::atomic<uint16_t> g_order_pos{0};
std::atomic<uint16_t> g_row_pos{0};
std::atomic<bool> g_engine_alive{false};
std::atomic<bool> g_load_in_progress{false};
std::atomic<bool> g_background_loading{false};
std::atomic<uint32_t> g_render_busy_us{0};
std::atomic<uint32_t> g_voice_count_ticks{0};
std::atomic<uint32_t> g_voice_count_sq_sum{0};
std::atomic<uint32_t> g_voice_count_peak{0};
std::atomic<uint32_t> g_voice_cull_level{0};
std::atomic<uint32_t> g_samples_in_use[kSamplesInUseWords];

bool try_stop_engine_for_load() {
    g_load_in_progress.store(true, std::memory_order_seq_cst);
    if (!g_engine_alive.load(std::memory_order_seq_cst)) return true;
    g_teardown_requested.store(true, std::memory_order_release);
    return false;
}

bool try_mark_engine_alive() {
    g_engine_alive.store(true, std::memory_order_seq_cst);
    if (!g_load_in_progress.load(std::memory_order_seq_cst)) return true;
    g_engine_alive.store(false, std::memory_order_release);
    return false;
}

void track_load_begin(void (*pump)(void*), void* user) {
    while (!try_stop_engine_for_load()) {
        if (pump) pump(user);
    }
    // Движок снят и не соберётся, пока загрузка идёт. Просьба о сносе,
    // поставленная в цикле после того, как Core0 уже снял движок, живому
    // движку не нужна, а остаться не должна: Core0 снёс бы ею следующий,
    // только что собранный.
    g_teardown_requested.store(false, std::memory_order_release);
    // Песня текущего поколения переписывается. Видна app_task после
    // release-записи track_load_end.
    g_stale_generation.store(g_song_generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
    // Позиция и карта прошлого трека. Писатель снят, новый движок опубликует
    // свои с первого тика; до него вытеснение видит позицию 0 - "ещё ничего
    // не отыграло", а не позицию прошлого трека.
    for (auto& w : g_samples_in_use) w.store(0, std::memory_order_relaxed);
    g_order_pos.store(0, std::memory_order_relaxed);
    g_row_pos.store(0, std::memory_order_relaxed);
    // Конец прошлого трека. Здесь, а не в начале сессии: старый движок уже
    // снят и не допишет g_song_ended поверх сброса, и сброс получает и
    // модуль плеера GS - иначе после доигравшего трека платы он играл бы в
    // тишину.
    g_song_ended.store(false, std::memory_order_relaxed);
    g_playback_finished.store(false, std::memory_order_relaxed);
    // Пауза прошлого трека новый не наследует.
    g_playback_paused.store(false, std::memory_order_relaxed);
    g_seek_frames.store(0, std::memory_order_relaxed);
}

void track_load_end() { g_load_in_progress.store(false, std::memory_order_release); }

void publish_loaded_track(uint32_t end_frame) {
    g_playback_frames.store(0, std::memory_order_relaxed);
    g_track_end_frame.store(end_frame, std::memory_order_relaxed);
    g_song_generation.fetch_add(1, std::memory_order_release);
}

} // namespace shared
