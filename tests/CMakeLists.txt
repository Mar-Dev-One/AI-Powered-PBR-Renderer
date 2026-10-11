# Headless tests: no window or GL context, so they run anywhere ctest does.
add_executable(WorldTests world_tests.c)
target_link_libraries(WorldTests PRIVATE PBR_Engine)
add_test(NAME WorldTests COMMAND WorldTests)
