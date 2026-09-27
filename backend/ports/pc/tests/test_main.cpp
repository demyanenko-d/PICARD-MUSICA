#include <cstdio>

#include "testing.h"

// Golden- и юнит-тесты - по одной функции run_*_tests() на
// модуль, регистрируются здесь вручную (без макро-реестра - на этом
// масштабе явный список проще и понятнее, см. testing.h).
void run_dpcm8_tests();
void run_resident_encoding_policy_tests();
void run_pattern_packer_tests();
void run_sample_pack_tests();
void run_bank_pcm_source_tests();
void run_mixbus_limiter_tests();
void run_mod_loader_tests();
void run_fatfs_mod_tests();
void run_s3m_loader_tests();
void run_xm_loader_tests();
void run_it_decompress_tests();
void run_it_loader_tests();
void run_bus_byte_source_tests();
void run_host_protocol_tests();
void run_session_loader_over_bus_tests();
void run_deferred_sample_load_tests();
void run_sample_prefetch_tests();
void run_tick_observer_tests();
void run_seek_tests();
void run_midi_replay_tests();
void run_solo_export_tests();
void run_loop_unroll_tests();
void run_progressive_loader_tests();
void run_midi_live_tests();
void run_live_stream_tests();
void run_live_chain_tests();
void run_track_transition_stress_tests();
void run_midi_loader_tests();
void run_it_golden_tests();
void run_psram_pattern_alloc_tests();
void run_gs_device_tests();
void run_ay_midi_tests();
void run_debug_ring_tests();
void run_hid_tests();
void run_hid_parser_tests();
void run_sd_spi_emu_tests();
void run_sequencer_tests();
void run_sequencer_libxmp_tests();
void run_voice_tests();
void run_effect_dispatch_porta_tests();
void run_nna_tests();
void run_voice_cull_tests();
void run_reverb_filter_tests();

int main() {
    run_dpcm8_tests();
    run_resident_encoding_policy_tests();
    run_pattern_packer_tests();
    run_sample_pack_tests();
    run_bank_pcm_source_tests();
    run_mixbus_limiter_tests();
    run_mod_loader_tests();
    run_fatfs_mod_tests();
    run_s3m_loader_tests();
    run_xm_loader_tests();
    run_it_decompress_tests();
    run_it_loader_tests();
    run_bus_byte_source_tests();
    run_host_protocol_tests();
    run_session_loader_over_bus_tests();
    run_deferred_sample_load_tests();
    run_sample_prefetch_tests();
    run_tick_observer_tests();
    run_seek_tests();
    run_midi_replay_tests();
    run_solo_export_tests();
    run_loop_unroll_tests();
    run_progressive_loader_tests();
    run_midi_live_tests();
    run_live_stream_tests();
    run_live_chain_tests();
    run_track_transition_stress_tests();
    run_midi_loader_tests();
    run_it_golden_tests();
    run_psram_pattern_alloc_tests();
    run_gs_device_tests();
    run_ay_midi_tests();
    run_debug_ring_tests();
    run_hid_tests();
    run_hid_parser_tests();
    run_sd_spi_emu_tests();
    run_sequencer_tests();
    run_sequencer_libxmp_tests();
    run_voice_tests();
    run_effect_dispatch_porta_tests();
    run_nna_tests();
    run_voice_cull_tests();
    run_reverb_filter_tests();

    std::printf("\n%d/%d проверок пройдено\n", testing::g_checks - testing::g_failures, testing::g_checks);
    if (testing::g_failures > 0) {
        std::printf("ПРОВАЛ: %d\n", testing::g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
