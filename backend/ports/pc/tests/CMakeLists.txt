# Юнит- и golden-тесты - только PC, не часть soundsinth_core.
# platform::mem_* (используется TrackMemory/formats::mod) реализован
# только в platform_pc (backend/ports/pc/memory_pool.cpp) - сам `platform`
# таргет INTERFACE-only (только заголовки), поэтому линковаться нужно
# именно с platform_pc, а не напрямую с soundsinth_core - это тянет за
# собой SDL2/FreeRTOS, но они и так уже собираются, как только включён
# SOUNDSINTH_BUILD_TESTS (корневой CMakeLists.txt добавляет backend/ports/pc
# для обоих SOUNDSINTH_BUILD_PC_APP/SOUNDSINTH_BUILD_TESTS).

# libxmp - эталон верификации диспетчера эффектов: независимый разбор + плеер,
# сверяем тик-в-тик через xmp_play_frame()/xmp_get_frame_info(). Тянем
# с РЕАЛЬНОГО GitHub через FetchContent (тот же приём, что уже
# используется для SDL2/FreeRTOS-Kernel в backend/ports/pc/CMakeLists.txt),
# а не из локальной копии исходников вне репозитория (из-за этого не
# получилось подключить OpenMPT напрямую). Зависимость только у
# soundsinth_tests - soundsinth_core её не видит и не должен.
include(FetchContent)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE) # не нужна .dll рядом с тестовым exe
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    libxmp
    GIT_REPOSITORY https://github.com/libxmp/libxmp.git
    GIT_TAG libxmp-4.7.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libxmp)

add_executable(soundsinth_tests
    test_main.cpp
    test_dpcm8.cpp
    test_resident_encoding_policy.cpp
    test_pattern_packer.cpp
    test_sample_pack.cpp
    test_bank_pcm_source.cpp
    test_mixbus_limiter.cpp
    test_mod_loader.cpp
    test_fatfs_mod.cpp
    test_s3m_loader.cpp
    test_xm_loader.cpp
    test_it_decompress.cpp
    test_it_loader.cpp
    test_bus_byte_source.cpp
    test_host_protocol.cpp
    test_session_loader_over_bus.cpp
    test_deferred_sample_load.cpp
    test_sample_prefetch.cpp
    test_midi_replay.cpp
    test_seek.cpp
    test_tick_observer.cpp
    test_solo_export.cpp
    test_loop_unroll.cpp
    test_progressive_loader.cpp
    test_track_transition_stress.cpp
    test_midi_loader.cpp
    test_midi_live.cpp
    test_live_stream.cpp
    test_live_chain.cpp
    test_it_golden.cpp
    test_psram_pattern_alloc.cpp
    test_sequencer.cpp
    test_sequencer_libxmp.cpp
    test_voice.cpp
    test_effect_dispatch_porta.cpp
    test_nna.cpp
    test_voice_cull.cpp
    test_reverb_filter.cpp
    test_gs_device.cpp
    test_ay_midi.cpp
    test_debug_ring.cpp
    test_hid.cpp
    test_hid_parser.cpp
    third_party/libxmp_itsex.cpp
)
target_link_libraries(soundsinth_tests PRIVATE platform_pc libxmp::xmp_static)

# Эмулятор карты, её протокол и порты Z-Controller приходят из библиотеки
# устройств. Тест реализует их контракты сам: шину SPI (на том конце линии
# эмулятор), носитель и порты Z80.
target_sources(soundsinth_tests PRIVATE test_sd_spi_emu.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/rp2350_stub/platform_impl.cpp)
target_include_directories(soundsinth_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rp2350_stub ${CMAKE_CURRENT_SOURCE_DIR}/../../rp2350)
target_compile_features(soundsinth_tests PRIVATE cxx_std_17)
