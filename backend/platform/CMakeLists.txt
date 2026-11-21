add_library(platform INTERFACE)
# Корень включений - родительский каталог, как у soundsinth_core:
# #include "platform/hot_path.h".
target_include_directories(platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(platform INTERFACE cxx_std_17)

# Версия прошивки - одна на сборку: образ .uf2, BOARD INFO, журналы.
# Имя платы на шине - только числа версии, без суффикса (-beta): хосты
# сравнивают их парой чисел, а имя ровно 16 знаков.
set(SOUNDSINTH_VERSION "0.50-beta")
string(REGEX REPLACE "-.*$" "" SOUNDSINTH_VERSION_NUM "${SOUNDSINTH_VERSION}")
set(SOUNDSINTH_VERSION "${SOUNDSINTH_VERSION}" PARENT_SCOPE)
target_compile_definitions(platform INTERFACE SOUNDSINTH_VERSION_STR="${SOUNDSINTH_VERSION}"
                                              SOUNDSINTH_VERSION_NUM_STR="${SOUNDSINTH_VERSION_NUM}")
