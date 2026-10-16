# The editor: the scene panels, command console, chat with the AI agent and
# the command server, on top of the same engine the game runtime (testbed/)
# uses. It's the only app that turns on Dear ImGui (App.use_imgui).
add_executable(Editor main.c)

target_link_libraries(Editor PRIVATE PBR_Engine)

# F5 in the editor starts the game following its edits (World/LiveScene.h),
# so the editor needs to know where the Testbed binary is -- and that it's
# built whenever the editor is.
add_dependencies(Editor Testbed)
target_compile_definitions(Editor PRIVATE TESTBED_EXE="$<TARGET_FILE:Testbed>")

# The chat agent's Python environment lives in the source tree (agent/README.md).
target_compile_definitions(Editor PRIVATE AGENT_DIR="${CMAKE_SOURCE_DIR}/agent")

# Export (Project/Project.h project_export()) ships the Game executable.
# Players need a Release build: GAME_RELEASE_EXE is where a multi-config
# build (Visual Studio) puts it, and the editor uses it when it's there;
# otherwise the Game of the editor's own configuration (GAME_EXE), with a
# warning that it isn't for players.
add_dependencies(Editor Game)
target_compile_definitions(Editor PRIVATE
    GAME_EXE="$<TARGET_FILE:Game>"
    GAME_RELEASE_EXE="$<TARGET_FILE_DIR:Game>/../Release/$<TARGET_FILE_NAME:Game>")

# The C++ runtime DLLs a Release build needs on a PC that may not have them
# (vcruntime140.dll, msvcp140.dll, ...), as CMake finds them for this
# compiler; Export copies them next to the game ('|'-separated).
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
string(REPLACE ";" "|" GAME_RUNTIME_DLLS "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
target_compile_definitions(Editor PRIVATE GAME_RUNTIME_DLLS="${GAME_RUNTIME_DLLS}")
