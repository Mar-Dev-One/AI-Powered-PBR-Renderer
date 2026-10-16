# The game runtime: loads a scene and plays it, with no editor UI, console or
# command server (those are editor/'s). Same engine library as the editor.
add_executable(Testbed main.c)

target_link_libraries(Testbed PRIVATE PBR_Engine)

# The same program as an exported game ships it (Project/Project.h
# project_export()): no console window on Windows, its log in log.txt next
# to it instead. main() stays the entry point.
add_executable(Game main.c)

target_link_libraries(Game PRIVATE PBR_Engine)
target_compile_definitions(Game PRIVATE PBR_GAME_RUNTIME)
if(MSVC)
    target_link_options(Game PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()
