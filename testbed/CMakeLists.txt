# The game runtime: loads a scene and plays it, with no editor UI, console or
# command server (those are editor/'s). Same engine library as the editor.
add_executable(Testbed main.c)

target_link_libraries(Testbed PRIVATE PBR_Engine)
