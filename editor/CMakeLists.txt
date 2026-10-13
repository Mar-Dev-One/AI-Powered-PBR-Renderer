# The editor: the scene panels, command console, chat with the AI agent and
# the command server, on top of the same engine the game runtime (testbed/)
# uses. It's the only app that turns on Dear ImGui (App.use_imgui).
add_executable(Editor main.c)

target_link_libraries(Editor PRIVATE PBR_Engine)
