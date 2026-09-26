# FatFs (R0.16, elm-chan.org, BSD-style лицензия), закоммичен целиком (не
# FetchContent). У апстрима нет git-репозитория, только
# периодические zip-релизы, и нет собственного CMakeLists.txt - собираем
# сами. ffconf.h - наши настройки (LFN/read-only/code page), лежит здесь
# же, рядом с ff.c: он ищет "ffconf.h" рядом с собой (quote-include
# резолвится в каталоге включающего файла раньше, чем по -I путям).
# diskio.h - только объявления (реализация своя у ПК и у прошивки),
# diskio.c (шаблон-заглушка апстрима) не нужен и не копировался.
add_library(fatfs STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ff.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ffsystem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ffunicode.c
)
target_include_directories(fatfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fatfs PUBLIC c_std_11)
