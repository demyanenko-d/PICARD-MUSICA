# Инструменты, НЕ часть автоматического тестового набора, запускаются
# вручную. Линкуются так же, как soundsinth_tests - platform_pc, не
# soundsinth_core напрямую (см. backend/ports/pc/tests/CMakeLists.txt, тот же
# повод: platform::mem_* живёт в platform_pc).

# Массовый прогон загрузчиков по внешней библиотеке.
add_executable(mass_load_scan mass_load_scan.cpp)
target_link_libraries(mass_load_scan PRIVATE platform_pc)
target_compile_features(mass_load_scan PRIVATE cxx_std_17)

# Образец файла настроек тем же кодом, каким его пишет плата.
add_executable(config_example config_example.cpp)
target_link_libraries(config_example PRIVATE soundsinth_core platform_pc)
target_compile_features(config_example PRIVATE cxx_std_17)

add_executable(sf2bake sf2bake.cpp)
target_link_libraries(sf2bake PRIVATE soundsinth_core platform_pc)
target_compile_features(sf2bake PRIVATE cxx_std_17)

# Выгрузка сэмплов банка в том виде, в каком они лежат в PSRAM у платы:
# резидентным потоком Dpcm8/Raw8. Скриптом это не сделать - прогоны сжаты
# моделью (bank_codec.h), и распаковщик нужен настоящий.
add_executable(bank_export bank_export.cpp)
target_link_libraries(bank_export PRIVATE platform_pc)
target_compile_features(bank_export PRIVATE cxx_std_17)

add_executable(_sizecheck _sizecheck.cpp)
target_link_libraries(_sizecheck PRIVATE soundsinth_core platform_pc)
target_compile_features(_sizecheck PRIVATE cxx_std_17)

# Хэш того, что строит конвертер .mid (Song, арена, зона паттернов PSRAM) -
# сторож перестроек загрузчика в tmp-проверках.
add_executable(midi_song_hash midi_song_hash.cpp)
target_link_libraries(midi_song_hash PRIVATE platform_pc)
target_compile_features(midi_song_hash PRIVATE cxx_std_17)
