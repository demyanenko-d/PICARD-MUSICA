# Эмуляторы устройств ZX: платформонезависимо, как и ядро синтезатора.
# Зависит только от platform и своего контракта devices/hal, который
# реализует порт. Синтезатор эту библиотеку не включает нигде.
add_library(soundsinth_devices STATIC
    sd/card_protocol.cpp
    sd/spi_emu.cpp
    storage/storage.cpp
    storage/diskio.cpp
    zcontroller/zcontroller.cpp
    gs/gs_device.cpp
    hid/keyboard.cpp
    hid/mouse.cpp
    hid/joystick.cpp
    hid/usb_map.cpp
    hid/report_map.cpp
)

# Корень включений - родительский каталог, как у ядра: пути вида
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
