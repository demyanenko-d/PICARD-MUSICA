# Прошивка RP2350B. Корневой проект: на него завязан плагин Pico для
# VS Code.
#
# Хостовая сборка (движок, тесты, плеер) - отдельный CMake-проект в
# backend/ports/pc/: CMake держит один тулчейн на один проход configure, а
# ARM-кросскомпилятор Pico SDK и MSVC в одной конфигурации несовместимы.
#
# platform_rp2350 реализует интерфейсы backend/platform/: os.h поверх
# FreeRTOS-Kernel и memory.h. Плюс вывод I2S на PIO и DMA, драйвер
# PSRAM на QMI и шина Z80. Раскладка по ядрам: Core0 - FreeRTOS и
# синтезатор, Core1 - шина Z80 и загрузка трека без ОС.

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD weact_studio_rp2350b_core CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Имя проекта совпадает с таргетом: плагин Pico для VS Code строит путь к
# .elf из имени проекта.
project(soundsinth_firmware C CXX ASM)

# Эмулятор Z-Controller: 1 включён (умолчание), 0 - вторая прошивка релиза.
# Перекрывает умолчание SOUNDSINTH_RP2350_ZCONTROLLER (там оно под #ifndef).
set(SOUNDSINTH_ZCONTROLLER 1 CACHE STRING "Эмулятор Z-Controller: 1 включён, 0 выключен")
add_compile_definitions(SOUNDSINTH_RP2350_ZCONTROLLER=${SOUNDSINTH_ZCONTROLLER})

# Пула будильников нет: он единственный путь, которым ожидание уходит в
# spin_lock_blocking с запретом прерываний, а на Core1 в это время идёт шина.
# sleep_ms тогда крутит busy_wait - его зовёт только старт Core0 на выдержку
# питания и частоты. Обработчик пула и его записи уходят из образа.
# Определение общее на сборку: pico_time попадает и в библиотеку платформы, и
# в саму прошивку, а два разных её перевода компоновщик не сводит.
add_compile_definitions(PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1)

# Замер сборки кадра I2S: 1 - вместо музыки знакопеременный узор, одинаковый
# в обоих каналах. На выходе смотрят соотношение каналов: совпадают - кадр
# собран из одного отсчёта, противофаза - правое слово от следующего.
# Только для замера, в обычной сборке 0.
set(SOUNDSINTH_I2S_PHASE_TEST 0 CACHE STRING "Узор для замера фазы каналов I2S: 1 включён")
add_compile_definitions(SOUNDSINTH_RP2350_I2S_PHASE_TEST=${SOUNDSINTH_I2S_PHASE_TEST})


# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# FreeRTOS-Kernel V11.2.0, порт CM33 без TrustZone. Качается в build/_deps,
# а не в общий с ПК .deps/: FetchContent держит там сборку под генератор
# Visual Studio, здесь Ninja.
include(FetchContent)

add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/backend/ports/rp2350/rtos)

set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)
set(FREERTOS_PORT "GCC_ARM_CM33_NTZ_NONSECURE" CACHE STRING "" FORCE)

FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG V11.2.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(freertos_kernel)

# Ядру нужен таргет platform. fatfs - прошивке: банк с карты читается через
# FatFs тем же арбитром и кэшем секторов, что у DivMMC и Z-Controller.
add_subdirectory(backend/platform)
add_subdirectory(backend/lib/fatfs)
add_subdirectory(backend/core)
add_subdirectory(backend/devices)
add_subdirectory(backend/player)

# Горячий путь синтезатора, читающий PSRAM, - в SRAM: код идёт из флеша, а
# Flash-XIP и PSRAM-QMI делят одну шину. Только для прошивки, ПК-сборка
# ключа не получает.
target_compile_definitions(soundsinth_core PUBLIC SOUNDSINTH_RP2350_HOT_PATH=1)
target_compile_definitions(soundsinth_devices PUBLIC SOUNDSINTH_RP2350_HOT_PATH=1)
target_compile_definitions(soundsinth_player PUBLIC SOUNDSINTH_RP2350_HOT_PATH=1)
# Ради platform/mono_time.h: часы на плате встроены и читают регистр таймера.
target_link_libraries(soundsinth_devices PUBLIC pico_platform hardware_timer)
# pico/platform.h нужен ради __not_in_flash_func: движок сам по себе
# платформонезависим и про таргеты pico-sdk не знает.
# hardware_timer (time_us_32()) - platform/clock.h, замеры занятости ядра в
# движке и задаче рендера.
target_link_libraries(soundsinth_core PUBLIC pico_platform hardware_timer)

# platform_rp2350 - реализация HAL (platform/os.h, platform/memory.h) +
# PSRAM/I2S-драйверы + Z80-шина. Симметрично
# platform_pc (backend/ports/pc/CMakeLists.txt).
add_library(platform_rp2350 STATIC
    backend/platform/os_freertos.cpp
    backend/platform/log_rings.cpp
    backend/ports/rp2350/hal/memory_pool.cpp
    backend/ports/rp2350/psram/psram_driver.cpp
    backend/ports/rp2350/audio/i2s_sink.cpp
    backend/ports/rp2350/hostlink/bus.cpp
    backend/ports/rp2350/hostlink/bus_setup.cpp
    backend/ports/rp2350/hostlink/divmmc.cpp
    backend/ports/rp2350/hostlink/hostlink.cpp
    backend/ports/rp2350/hal/z80_ports.cpp
    backend/ports/rp2350/hal/host_link.cpp
)
target_link_libraries(platform_rp2350
    PUBLIC platform soundsinth_core soundsinth_devices soundsinth_player freertos_kernel freertos_config
    PRIVATE fatfs hardware_pio hardware_dma hardware_irq hardware_clocks hardware_xip_cache hardware_uart hardware_spi pico_multicore pico_sync
)
# Реализации контрактов platform/ и devices/hal лежат здесь, а зовут их ядро
# и библиотека устройств: цикл между статическими библиотеками. Объявлен
# явно - компоновщик разбирает архивы слева направо по разу, и без этого
# определения не находятся.
target_link_libraries(soundsinth_core PRIVATE platform_rp2350)
target_link_libraries(soundsinth_devices PRIVATE platform_rp2350)
target_link_libraries(soundsinth_player PRIVATE platform_rp2350)

# gpio_put/gpio_get через регистры SIO, без сопроцессора GPIO: один и тот же
# код на обоих ядрах. PUBLIC - для всей прошивки.
target_compile_definitions(platform_rp2350 PUBLIC PICO_USE_GPIO_COPROCESSOR=0)

# PICO_USE_SW_SPIN_LOCKS не переопределять: pico_sync собрана с умолчанием,
# а инлайновые critical_section_enter_blocking/exit с другим значением
# читают сохранённый адрес спинлока как регистр SIO - HardFault.

# Включения по имени файла из корня rp2350 и hostlink/, без пути до
# внутренностей.
target_include_directories(platform_rp2350 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350/hostlink
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350/hostlink/rom_images
)

# Без -Werror, пока предупреждения не вычищены. -Wformat из -Wall ловит
# спецификатор без аргумента в snprintf.
target_compile_options(platform_rp2350 PRIVATE -Wall -Wextra)
# Как у ядра: исключений и RTTI в коде платы нет. С -fexceptions каждый .cpp
# ссылается на __aeabi_unwind_cpp_pr0 и тянет раскрутчик libgcc, а его
# скрипт компоновки SDK кладёт в SRAM. -ffunction-sections/-fdata-sections
# сюда не брать: адресация обработчиков шины без общего якоря секции длиннее.
target_compile_options(platform_rp2350 PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-unwind-tables;-fno-rtti>)
target_compile_options(soundsinth_core PRIVATE -Wall -Wextra)
target_compile_options(soundsinth_devices PRIVATE -Wall -Wextra)
target_compile_options(soundsinth_player PRIVATE -Wall -Wextra)
# Загрузка трека идёт на Core1 со стеком 4 КБ, MSPLIM там нет: кадр больше
# 1400 байт - предупреждение сборки, а не порча стека на плате. Самые большие
# кадры загрузчиков - около 1.3 КБ (IT).
target_compile_options(soundsinth_core PRIVATE -Wframe-larger-than=1400)

# PIO-программы (Z80-шина + I2S) - сгенерированные .pio.h нужны только
# platform_rp2350 (единственная библиотека, которая их #include'ит).
# Шинных файлов ровно два, и делятся они по границе СИГНАЛА: у Z80 цикл
# порта отмечен IORQ, цикл памяти - MREQ, и они взаимно исключены.
pico_generate_pio_header(platform_rp2350
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350/hostlink/ports.pio
)
pico_generate_pio_header(platform_rp2350
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350/hostlink/memory.pio
)
pico_generate_pio_header(platform_rp2350
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350/audio/i2s_pio.pio
)
pico_generate_pio_header(platform_rp2350
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350/hostlink/trap_join.pio
)

add_executable(soundsinth_firmware
    backend/ports/rp2350/main.cpp
    # Реализация контракта devices/hal: в исполняемом файле, а не в статической
    # библиотеке порта - иначе объект не доживает до разбора библиотеки устройств.
    backend/ports/rp2350/hal/spi.cpp
    backend/ports/rp2350/hal/media.cpp
    backend/ports/rp2350/hal/audio_out.cpp
    backend/ports/rp2350/log.cpp
)
target_include_directories(soundsinth_firmware PRIVATE
    # Включения по имени файла из корня rp2350 и hostlink/, без пути до
    # внутренностей.
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350/hostlink
    # Образы ПЗУ - в подкаталоге; путь добавлен, чтобы включение писалось по
    # имени файла.
    ${CMAKE_CURRENT_LIST_DIR}/backend/ports/rp2350/hostlink/rom_images
)

# Core1 без ОС: цикл шины, загрузка и разбор формата на одном стеке вместе с
# кадрами прерываний шины. 2 КБ по умолчанию переполняются на первой
# загрузке трека и портят соседнюю память (буфер debug_log). PICO_STACK_SIZE
# задаёт оба стека: Core0 до планировщика (SCRATCH_Y) и Core1 (SCRATCH_X);
# 0x1000 - предел: SCRATCH_X/Y по 4 КБ.
target_compile_definitions(soundsinth_firmware PRIVATE PICO_STACK_SIZE=0x1000)

# printf только на Core0 до запуска Core1, дальше печать через debug_log:
# замок stdout не нужен, а его код pico_sync лёг бы в SRAM.
target_compile_definitions(soundsinth_firmware PRIVATE PICO_STDOUT_MUTEX=0)

# Без LTO: -flto только на своих таргетах без pico-sdk даёт при компоновке
# "dangerous relocation: unsupported relocation" (частичный LTO на Thumb).
pico_set_program_name(soundsinth_firmware "SoundSinth")
pico_set_program_version(soundsinth_firmware "0.4")

# copy_to_ram выключен: весь бинарник вместе со статикой в SRAM не
# помещается (ошибка линковки "region RAM overflowed"). Код выполняется из
# Flash-XIP; риск конкуренции Flash-XIP и PSRAM-QMI за общую шину закрыт
# точечно: __not_in_flash_func на горячих путях (ключ SOUNDSINTH_RP2350_HOT_PATH
# выше), а не копированием всего бинарника.
# psram/psram_driver.cpp держит свои __not_in_flash_func независимо от
# этого - та часть не про конкуренцию, а про физическую невозможность
# читать флеш во время QMI direct-mode PSRAM-инициализации.

# Лог - физический UART (GPIO32/33), пины задаёт main.cpp: умолчания платы
# могут не совпадать. USB CDC выключен.
pico_enable_stdio_uart(soundsinth_firmware 1)
pico_enable_stdio_usb(soundsinth_firmware 0)

target_link_libraries(soundsinth_firmware
    pico_stdlib
    pico_multicore
    hardware_dma
    # hardware_pio - ради заголовка: hw_config.h держит номера блоков типом
    # PIO.
    hardware_pio
    hardware_spi   # spi_pins.cpp: реализация devices/hal/spi.h
    platform_rp2350
)

# pico-sdk называет векторы исключений isr_svcall/isr_pendsv/isr_systick,
# FreeRTOS-порт (GCC/ARM_CM33_NTZ) - SVC_Handler/PendSV_Handler/
# SysTick_Handler (CMSIS-имена) - алиасим линкером
# (configCHECK_HANDLER_INSTALLATION=0 в FreeRTOSConfig.h
# отключает попытку FreeRTOS проверить это самостоятельно).
target_link_options(soundsinth_firmware PRIVATE
    -Wl,--defsym=isr_svcall=SVC_Handler
    -Wl,--defsym=isr_pendsv=PendSV_Handler
    -Wl,--defsym=isr_systick=SysTick_Handler
)

pico_add_extra_outputs(soundsinth_firmware)
