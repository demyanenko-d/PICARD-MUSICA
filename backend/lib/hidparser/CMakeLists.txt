# Разборщик дескрипторов HID из LUFA (Dean Camera), взят как есть.
# Лицензия - в шапке каждого файла: разрешено использовать, изменять и
# распространять при сохранении уведомления.
#
# Правлены только строки #include в HIDParser.h: вместо LUFA/Common/Common.h
# подставлен lufa_env.h этого каталога. Всё остальное - верховья, чтобы
# обновление оставалось сравнением файлов.
#
# Кучи не трогает: таблицу предметов даёт вызывающий, размеры заданы
# макросами в lufa_env.h. Собирается и в прошивку, и в хостовые тесты.
add_library(hidparser STATIC
    HIDParser.c
)
target_include_directories(hidparser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hidparser PUBLIC c_std_99)

# Чужой код: свои предупреждения к нему не применяем, но и чинить его не
# собираемся - пусть остаётся сравнимым с верховьями.
if (NOT MSVC)
    target_compile_options(hidparser PRIVATE -Wno-unused-parameter)
endif()
