# Headless tests: no window or GL context, so they run anywhere ctest does.
add_executable(WorldTests world_tests.c)
target_link_libraries(WorldTests PRIVATE PBR_Engine)
add_test(NAME WorldTests COMMAND WorldTests)

add_executable(PathsTests paths_tests.c)
target_link_libraries(PathsTests PRIVATE PBR_Engine)
target_compile_definitions(PathsTests PRIVATE PATHS_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/paths_project")
add_test(NAME PathsTests COMMAND PathsTests)

add_executable(ProjectTests project_tests.c)
target_link_libraries(ProjectTests PRIVATE PBR_Engine)
target_compile_definitions(ProjectTests PRIVATE PROJECT_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/project_scratch")
add_test(NAME ProjectTests COMMAND ProjectTests)

add_executable(SerializeTests serialize_tests.c)
target_link_libraries(SerializeTests PRIVATE PBR_Engine)
add_test(NAME SerializeTests COMMAND SerializeTests)

# Every scene in assets/scenes/ still loads with today's format rules.
add_executable(SceneFileTests scene_file_tests.c)
target_link_libraries(SceneFileTests PRIVATE PBR_Engine)
add_test(NAME SceneFileTests COMMAND SceneFileTests)

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

# pbr_agent (agent/): its Python tests against ServerHost, when its virtual
# environment has been set up (see agent/README.md).
if(WIN32)
    set(PBR_AGENT_PYTHON ${CMAKE_SOURCE_DIR}/agent/.venv/Scripts/python.exe)
else()
    set(PBR_AGENT_PYTHON ${CMAKE_SOURCE_DIR}/agent/.venv/bin/python)
endif()
if(EXISTS ${PBR_AGENT_PYTHON})
    add_test(NAME AgentTests
             COMMAND ${PBR_AGENT_PYTHON} -m unittest discover -s ${CMAKE_SOURCE_DIR}/agent/tests)
    set_tests_properties(AgentTests PROPERTIES ENVIRONMENT "PBR_SERVER_HOST=$<TARGET_FILE:ServerHost>")
endif()
