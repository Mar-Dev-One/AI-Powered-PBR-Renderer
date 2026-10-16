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
