# Сеанс трека: приём файла от хоста, загрузка по фазам, телеметрия,
# транспорт и общее состояние между загрузкой и звуком. Платформонезависимо.
#
# Отдельно от ядра синтезатора: звуку про сеанс знать нечего, а сеансу про
# устройства ZX - нечего сверх контракта.
add_library(soundsinth_player STATIC
    nav/nav.cpp
    nav/nav_board.cpp
    config/config_page.cpp
    config/config_term.cpp
    config/config_ui.cpp
    config/config_tools.cpp
    config/tool_wifi.cpp
    config/config_service.cpp
    config/config_term_service.cpp
    config/config_apply.cpp
    config/tools_service.cpp
    config/tools_view.cpp
    bank_sd.cpp
    gs/bridge.cpp
    gs/fx_player.cpp
    gs/midi_sched.cpp
    gs/rx_layout.cpp
    audio/buffer_pool.cpp
    protocol/host_protocol.cpp
    protocol/track_meta.cpp
    load/bus_byte_source.cpp
    load/session_loader.cpp
    load/sample_prefetch.cpp
    load/progressive_loader.cpp
    live/session.cpp
    live/live_tap.cpp
    live/ay_live.cpp
    live/live_clock.cpp
    live/saa_live.cpp
    live/dac_live.cpp
    audio/render_task.cpp
    audio/sequencer_task.cpp
    shared_state.cpp
    audio/chain.cpp
    banks.cpp
    session.cpp
    fm_tables.cpp
    vgm/vgm.cpp
    vgm/vgm_track.cpp
)

# Корень включений - родительский каталог, как у ядра и устройств: пути вида
# "player/shared_state.h" разрешаются от backend/.
target_include_directories(soundsinth_player PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Устройства - напрямую: арбитр носителя сеанс зовёт сам, контракта под
# него не надо.
target_link_libraries(soundsinth_player PUBLIC platform soundsinth_core soundsinth_devices)
# FatFs - банку с карты: файл .ssb открывается через файловую систему.
target_link_libraries(soundsinth_player PRIVATE fatfs)
target_compile_features(soundsinth_player PUBLIC cxx_std_17)

# FM-часть живого TurboSound FM.
target_link_libraries(soundsinth_player PRIVATE ymfm)

# Как у ядра: исключений и RTTI в этом коде нет, а на МК они стоят
# unwind-таблиц и type_info.
target_compile_options(soundsinth_player PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/GR->
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions;-fno-unwind-tables;-fno-rtti>
)
