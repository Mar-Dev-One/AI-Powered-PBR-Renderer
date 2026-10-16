# The game runtime: plays a project's scenes, with no editor UI, console or
# command server (those are editor/'s): it links engine/'s PBR_Runtime only.
# Export (Project/Project.h project_export()) ships it, renamed after the
# project.
#
# Debug builds are console programs, so the log shows in the terminal while
# developing. Other configurations -- the one players get -- open no console
# window on Windows and write the log to log.txt next to the program
# instead (PBR_GAME_NO_CONSOLE). main() stays the entry point either way.
add_executable(Game main.c)

target_link_libraries(Game PRIVATE PBR_Runtime)
target_compile_definitions(Game PRIVATE $<$<NOT:$<CONFIG:Debug>>:PBR_GAME_NO_CONSOLE>)
if(MSVC)
    target_link_options(Game PRIVATE $<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
endif()
