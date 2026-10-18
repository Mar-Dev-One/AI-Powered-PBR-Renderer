# The editor: the scene panels, command console, chat with the AI agent and
# the command server -- engine/'s PBR_Tools, on top of the PBR_Runtime the
# game (game/) uses. It's the only app with a UI layer (App.ui = imgui_layer()).
# Editor.h says which file holds which part.
add_executable(Editor main.c Actions.c Agent.c Export.c HostCommands.c Menus.c Panels.c Viewport.c
                      SceneDocument.c PlaySession.c)

target_link_libraries(Editor PRIVATE PBR_Tools)

# The chat agent's Python environment lives in the source tree (agent/README.md).
target_compile_definitions(Editor PRIVATE AGENT_DIR="${CMAKE_SOURCE_DIR}/agent")

# Export (Project/Project.h project_export()) ships the Game executable.
# Players need a non-Debug build. A non-Debug editor ships its own
# configuration's Game (GAME_EXE). A Debug editor ships the Release Game
# where a multi-config build (Visual Studio) puts it, GAME_RELEASE_EXE, if
# it's been built -- warning when it's older than GAME_EXE -- else its own
# Debug Game, with a warning that it isn't for players.
add_dependencies(Editor Game)
target_compile_definitions(Editor PRIVATE
    GAME_EXE="$<TARGET_FILE:Game>"
    GAME_EXE_IS_DEBUG=$<CONFIG:Debug>
    GAME_RELEASE_EXE="$<TARGET_FILE_DIR:Game>/../Release/$<TARGET_FILE_NAME:Game>")

# The runtime DLLs a Release game needs on a PC that may not have them, as
# CMake finds them for this compiler; Export copies them next to the game
# ('|'-separated). The game is C only, so it imports just vcruntime140.dll
# (the rest of the C runtime, api-ms-win-crt-*, is part of Windows 10 and
# later): the C++ library (msvcp140*.dll, concrt140.dll) and C++ exception
# support (vcruntime140_1.dll) stay out. Should the game gain C++ code,
# `dumpbin /dependents Game.exe` lists what to let through.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
set(GAME_RUNTIME_DLL_LIST ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
list(FILTER GAME_RUNTIME_DLL_LIST INCLUDE REGEX "/vcruntime140\\.dll$")
string(REPLACE ";" "|" GAME_RUNTIME_DLLS "${GAME_RUNTIME_DLL_LIST}")
target_compile_definitions(Editor PRIVATE GAME_RUNTIME_DLLS="${GAME_RUNTIME_DLLS}")
