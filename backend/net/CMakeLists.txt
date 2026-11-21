# Сетевое содержимое: архив zxart.ee каталогами (zxart) на виртуальном томе
# FAT32 (vdisk). Платформонезависимо; транспорт - контракт net/hal/net.h,
# его реализует порт. Над devices: том собирает сектора по разметке FAT
# из devices/storage.
add_library(soundsinth_net STATIC
    vdisk/vdisk.cpp
    vdisk/fat_names.cpp
    zxart/json_stream.cpp
    zxart/zxart.cpp
)

target_include_directories(soundsinth_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(soundsinth_net PUBLIC platform soundsinth_core soundsinth_devices)
target_compile_features(soundsinth_net PUBLIC cxx_std_17)

target_compile_options(soundsinth_net PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/GR->
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions;-fno-unwind-tables;-fno-rtti>
)
