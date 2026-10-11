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

add_executable(JsonRpcTests jsonrpc_tests.c)
target_link_libraries(JsonRpcTests PRIVATE PBR_Engine)
add_test(NAME JsonRpcTests COMMAND JsonRpcTests)

# End-to-end: a headless host behind the real socket server, driven by a
# stdlib-only Python WebSocket client.
add_executable(ServerHost server_host.c)
target_link_libraries(ServerHost PRIVATE PBR_Engine)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME ServerTests
             COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/server_test.py
                     $<TARGET_FILE:ServerHost> ${CMAKE_CURRENT_BINARY_DIR}/server-discovery.json)
endif()

add_executable(EncodingTests encoding_tests.c)
target_link_libraries(EncodingTests PRIVATE PBR_Engine)
add_test(NAME EncodingTests COMMAND EncodingTests)
