# Эмуляторы устройств ZX: платформонезависимо, как и ядро синтезатора.
# Зависит только от platform и своего контракта devices/hal, который
# реализует порт. Синтезатор эту библиотеку не включает нигде.
add_library(soundsinth_devices STATIC
    sd/card_protocol.cpp
    sd/spi_emu.cpp
    storage/storage.cpp
    storage/diskio.cpp
    storage/partition.cpp
    storage/volumes.cpp
    storage/fat_check.cpp
    zcontroller/zcontroller.cpp
    divmmc_card/divmmc_card.cpp
    bdi/bdi.cpp
    gs/gs_device.cpp
    hid/keyboard.cpp
    hid/mouse.cpp
    hid/joystick.cpp
    hid/usb_map.cpp
    hid/report_map.cpp
)

# Корень включений - родительский каталог: пути вида
# "devices/sd/card_protocol.h" разрешаются от backend/.
target_include_directories(soundsinth_devices PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(soundsinth_devices PUBLIC platform)
# FatFs - ради переходника diskio: файловую систему на карте читает он.
target_link_libraries(soundsinth_devices PRIVATE fatfs)
# Разборщик дескрипторов HID: им слой hid узнаёт, где у устройства оси,
# шляпка и кнопки. PUBLIC - его типы видны в заголовках hid.
target_link_libraries(soundsinth_devices PUBLIC hidparser)
# Цикл объявлен явно: ff.c зовёт disk_* из diskio.cpp, а статические
# библиотеки компоновщик просматривает по одному разу слева направо -
# без этого переходник в сборку не попадает вовсе.
target_link_libraries(fatfs PRIVATE soundsinth_devices)
target_compile_features(soundsinth_devices PUBLIC cxx_std_17)

# Как у ядра: исключений и RTTI в коде устройств нет, а на МК они стоят
# unwind-таблиц и type_info.
target_compile_options(soundsinth_devices PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/GR->
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions;-fno-unwind-tables;-fno-rtti>
)

# Beta Disk, который на плате выполняется из банков 8-15 ОЗУ DivMMC: скрипт
# компоновщика переносит туда эту библиотеку целиком, и новый файл в ней не
# забыть. Зависимость взаимная, как у fatfs.
add_library(soundsinth_bdi STATIC
    bdi/vg93.cpp
    bdi/disk.cpp
    bdi/drive.cpp
)
target_link_libraries(soundsinth_bdi PUBLIC soundsinth_devices)
target_link_libraries(soundsinth_devices PUBLIC soundsinth_bdi)
target_compile_options(soundsinth_bdi PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/GR->
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions;-fno-unwind-tables;-fno-rtti>
)

# Диск: сборка дорожек и образ на носителе - вне горячего пути.
set_source_files_properties(bdi/disk.cpp PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Os>")
