add_library(soundsinth_core STATIC
    audio/mixbus.cpp
    memory/psram_alloc.cpp
    memory/psram_store.cpp
    memory/sample_cache_catalog.cpp
    memory/arena.cpp
    memory/track_memory.cpp
    bank/bank_reader.cpp
    formats/midi.cpp
    formats/midi_convert.cpp
    formats/midi_live.cpp
    codec/dpcm8.cpp
    codec/block_locate.cpp
    codec/sample_pack.cpp
    codec/pattern_packer.cpp
    codec/pattern_reader.cpp
    model/amiga_period.cpp
    model/envelope.cpp
    codec/loop_unroll.cpp
    codec/pack_file_pcm.cpp
    formats/mod.cpp
    formats/s3m.cpp
    formats/xm.cpp
    formats/it_decompress.cpp
    formats/it.cpp
    engine/sequencer.cpp
    engine/effect_dispatch.cpp
    engine/resonant_filter.cpp
    engine/reverb.cpp
    engine/voice.cpp
    engine/tracker_engine.cpp
    engine/voice_mixer.cpp
    engine/voice_arbiter.cpp
    engine/voice_link.cpp
    engine/song_duration.cpp
    live_midi/ay_midi.cpp
    config/config.cpp
    config/config_store.cpp
    config/config_ini.cpp
    config/config_page.cpp
)

# Корень включений - родительский каталог: .h и .cpp лежат рядом, пути
# вида "core/engine/voice.h" разрешаются от backend/. Заголовки все
# публичные, оба потребителя (прошивка и ПК-плеер) собираются из этого
# дерева, отдельный include/ не нужен.
target_include_directories(soundsinth_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# FatFs ядру не нужен: FatFsByteSource живёт в backend/ports/pc, прошивка
# подключает fatfs сама (носитель, банк с карты).
target_link_libraries(soundsinth_core PUBLIC platform)
target_compile_features(soundsinth_core PUBLIC cxx_std_17)

# soundsinth_core собирается и в прошивку: исключения и RTTI на МК - лишний
# вес (unwind-таблицы, type_info), а в коде их нет (SoundSource - структура
# с указателями на функции, а не виртуальный класс). PRIVATE - только для
# этой библиотеки, ПК-часть (platform_pc, pc_player) не ограничена.
target_compile_options(soundsinth_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/GR->
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions;-fno-rtti>
)
# Только C++: в прошивке ядро собирает и C-исходники pico_platform (они
# приходят интерфейсными исходниками через platform), -fno-rtti у C -
# предупреждение компилятора.
# У MSVC нет аналога -fno-exceptions: /GR- запрещает только RTTI. Отсутствие
# исключений на ПК держится на коде (throw/try/catch нет), компилятором оно
# проверяется в сборке прошивки (GCC, -fno-exceptions).

# Цикл по каналам в apply_continuous_effects GCC раздваивает по признакам
# песни (модель частоты, таблица LFO IT), и копии лежат в SRAM. Без
# раздвоения код тика в SRAM меньше, цена - проверка признаков на канал.
set_source_files_properties(engine/effect_dispatch.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:GNU>:-fno-unswitch-loops>")
