# PC-сборка (движок+тесты+плеер на хостовом компиляторе - рендер в WAV
# для разработки и сверки с эталонными плеерами) -
# САМОСТОЯТЕЛЬНЫЙ CMake-проект, настраивается отдельно от корневого
# (тот теперь - прошивка RP2350B, см. корневой CMakeLists.txt за полным
# объяснением, почему их нельзя было оставить в одном дереве). Раньше
# большая часть этого файла жила в корневом CMakeLists.txt - перенесено
# сюда, никакой логики не изменилось, только расположение и то, что
# пути к platform/lib/fatfs/core теперь относительные (../...),
# не от корня репозитория.
#
# Собирается отдельно от прошивки:
#   cmake -S backend/ports/pc -B build/pc [-DSOUNDSINTH_BUILD_TESTS=ON]
#   cmake --build build/pc --config Release

cmake_minimum_required(VERSION 3.20)
project(SoundSinthPC LANGUAGES CXX C) # C - для FatFs/FreeRTOS-Kernel (FetchContent)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Исходники, которые CMake FetchContent скачивает для SDL2/FreeRTOS-Kernel,
# должны пережить удаление build/ - это обычный способ
# "почистить и пересобрать", и он не должен требовать повторного
# скачивания зависимостей из сети. Намеренно указывает на .deps в корне
# репозитория (не внутри build/) - то же самое место, что
# использовалось до переноса, чтобы уже скачанные SDL2/FreeRTOS-Kernel не
# тянулись заново.
#
# Каталог свой у каждой разрядности: 32-битная сборка (ею память считается
# как на плате - указатель 4 байта) не должна подхватывать объектные файлы
# 64-битной. Общий каталог на две конфигурации уже стоил дня поисков.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(FETCHCONTENT_BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../.deps32 CACHE PATH "" FORCE)
else()
    set(FETCHCONTENT_BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../.deps CACHE PATH "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    # <windows.h> (затягивается портом FreeRTOS-симулятора, platform_pc)
    # определяет макросы min/max, которые ломают std::min/std::max в коде,
    # который его подключает.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(SOUNDSINTH_BUILD_PC_APP "Build the PC player app (SDL2/WAV output)" ON)
option(SOUNDSINTH_BUILD_TESTS  "Build unit tests" OFF)

# Вне дерева этого CMakeLists.txt (../...) - add_subdirectory
# требует явный binary_dir для таких случаев.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../platform    ${CMAKE_BINARY_DIR}/platform)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/fatfs   ${CMAKE_BINARY_DIR}/lib_fatfs)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/hidparser ${CMAKE_BINARY_DIR}/lib_hidparser)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core       ${CMAKE_BINARY_DIR}/core)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../devices     ${CMAKE_BINARY_DIR}/devices)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../player      ${CMAKE_BINARY_DIR}/player)
# Счётчики цены для тестов (обходы цепочки страниц); в прошивке их нет.
target_compile_definitions(soundsinth_core PUBLIC SOUNDSINTH_TEST_COUNTERS=1)

include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL2)

# FreeRTOS-Kernel Windows-симулятор (MSVC_MINGW порт) - реализация
# platform::os.h на PC. Даёт настоящий FreeRTOS API уже
# на PC-этапе разработки, а не std::thread-имитацию его семантики.
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/rtos)

set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)
set(FREERTOS_PORT "MSVC_MINGW" CACHE STRING "" FORCE)

FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG V11.2.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(freertos_kernel)

add_library(platform_pc STATIC
    ../../platform/os_freertos.cpp
    memory_pool.cpp
    sdl_sink.cpp
    wav_writer.cpp
    fatfs_diskio.cpp
    fatfs_byte_source.cpp
    usb_host_stub.cpp
    config_flash_stub.cpp
    boot_mode_stub.cpp
)

target_include_directories(platform_pc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(platform_pc
    # PUBLIC: backend/ports/pc/app сам создаёт свою верхнюю FreeRTOS-задачу и
    # вызывает vTaskStartScheduler() (см. main.cpp), поэтому ему тоже
    # нужны заголовки FreeRTOS напрямую.
    PUBLIC platform soundsinth_core soundsinth_devices soundsinth_player fatfs freertos_kernel freertos_config
    PRIVATE SDL2::SDL2-static
)

if(SOUNDSINTH_BUILD_PC_APP)
    add_subdirectory(app)
endif()

if(SOUNDSINTH_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(tools)
endif()
