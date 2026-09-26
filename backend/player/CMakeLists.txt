# Сеанс трека: приём файла от хоста, загрузка по фазам, телеметрия,
# транспорт, поверхность между загрузкой и звуком. Платформонезависимо.
#
# Отдельно от ядра синтезатора: звуку про сеанс знать нечего, а сеансу про
# устройства ZX - нечего сверх контракта. Что нужно от порта, объявлено в
# player/hal.
add_library(soundsinth_player STATIC
    bank_sd.cpp
    gs/bridge.cpp
    audio/buffer_pool.cpp
    protocol/host_protocol.cpp
    load/bus_byte_source.cpp
    load/session_loader.cpp
    load/sample_prefetch.cpp
    load/progressive_loader.cpp
    live/session.cpp
    live/ay_tap.cpp
    audio/render_task.cpp
    audio/sequencer_task.cpp
    shared_state.cpp
    audio/chain.cpp
    banks.cpp
    session.cpp
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

# Как у ядра: исключений и RTTI в этом коде нет, а на МК они стоят
# unwind-таблиц и type_info.
target_compile_options(soundsinth_player PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/GR->
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions;-fno-unwind-tables;-fno-rtti>
)
