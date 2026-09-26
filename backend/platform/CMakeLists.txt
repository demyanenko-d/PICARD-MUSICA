add_library(platform INTERFACE)
# Корень включений - родительский каталог, как у soundsinth_core:
# #include "platform/hot_path.h".
target_include_directories(platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(platform INTERFACE cxx_std_17)
