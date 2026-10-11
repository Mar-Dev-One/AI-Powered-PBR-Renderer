# Headless tests: no window or GL context, so they run anywhere ctest does.
add_executable(WorldTests world_tests.c)
target_link_libraries(WorldTests PRIVATE PBR_Engine)
add_test(NAME WorldTests COMMAND WorldTests)

add_executable(SerializeTests serialize_tests.c)
target_link_libraries(SerializeTests PRIVATE PBR_Engine)
add_test(NAME SerializeTests COMMAND SerializeTests)

add_executable(CommandTests command_tests.c)
target_link_libraries(CommandTests PRIVATE PBR_Engine)
add_test(NAME CommandTests COMMAND CommandTests)

add_executable(HistoryTests history_tests.c)
target_link_libraries(HistoryTests PRIVATE PBR_Engine)
add_test(NAME HistoryTests COMMAND HistoryTests)

add_executable(PlacementTests placement_tests.c)
target_link_libraries(PlacementTests PRIVATE PBR_Engine)
target_compile_definitions(PlacementTests PRIVATE TEST_SCENES_DIR="${CMAKE_CURRENT_BINARY_DIR}/scenes")
add_test(NAME PlacementTests COMMAND PlacementTests)
