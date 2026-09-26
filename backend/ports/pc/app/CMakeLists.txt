add_executable(pc_player
    main.cpp
    sine_source.cpp
)

target_link_libraries(pc_player PRIVATE soundsinth_core platform_pc)
