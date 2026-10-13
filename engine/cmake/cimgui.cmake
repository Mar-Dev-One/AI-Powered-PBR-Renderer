# Builds cimgui (the C API for Dear ImGui) plus the GLFW + OpenGL3 backends,
# as a static library the rest of the (C) project can link against.
#
# Dear ImGui itself is C++; this project is pure C, so cimgui is what lets
# us call it from C. The backend .cpp files are compiled in here too and
# exposed with C linkage (IMGUI_IMPL_API) so RHI/Window code never has to
# touch C++.

set(CIMGUI_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/deps/cimgui)
set(IMGUI_DIR    ${CIMGUI_DIR}/imgui)
set(IMGUIZMO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/ImGuizmo/src)

add_library(cimgui STATIC
    ${CIMGUI_DIR}/cimgui.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    # ImGuizmo (MIT, git submodule): the editor's move/rotate/scale handles.
    # Built against this same ImGui; its C face is engine/src/UI/Gizmo.h.
    ${IMGUIZMO_DIR}/ImGuizmo.cpp
)

target_include_directories(cimgui PUBLIC
    ${CIMGUI_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUIZMO_DIR}
)

target_compile_definitions(cimgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1
    CIMGUI_USE_GLFW=1
    CIMGUI_USE_OPENGL3=1
)

# CIMGUI_DEFINE_ENUMS_AND_STRUCTS makes cimgui.h define its own plain-C
# struct layouts (ImVec2, ImRect, etc). cimgui.cpp itself must NOT see this
# -- it's built against the real C++ ImGui types -- so this is INTERFACE
# only: it reaches C code that includes cimgui.h, not cimgui.cpp.
target_compile_definitions(cimgui INTERFACE
    CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)

if (WIN32)
    target_compile_definitions(cimgui PUBLIC "IMGUI_IMPL_API=extern \"C\" __declspec(dllexport)")
else()
    target_compile_definitions(cimgui PUBLIC "IMGUI_IMPL_API=extern \"C\"")
endif()

target_link_libraries(cimgui PUBLIC glfw OpenGL::GL)
