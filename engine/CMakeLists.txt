# The engine, as one static library: PBR_Engine. Everything is in it -- the
# runtime a game needs (window, RHI, renderer, models and assets, the World
# and its scene files, project settings) and the tools on top (the command
# layer and its undo history, the command server, the chat session with the
# AI agent, the Dear ImGui panels). game/, editor/, testbed/ and tests/ link
# it.
#
# A program that uses only the runtime carries none of the tools: a static
# library only contributes the object files a program references, and
# runtime code never includes a tools header (Commands/, Net/, UI/) -- App
# reaches a UI only through the App.ui hook table (App.h). So such a program
# stays C only, without ImGui's C++ or Winsock.
#
# The dependencies stay libraries of their own -- glfw3, glad and cimgui --
# that PBR_Engine links; `cmake --install` (the end of this file) puts them
# next to it in an SDK folder.

# GLFW's own install rules would add its headers, a CMake package and a
# pkg-config file to the SDK; ours install what the engine needs.
set(GLFW_INSTALL     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS  OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/glfw)
add_subdirectory(deps/glad)
include(cmake/cimgui.cmake)

# cglm is used header-only: the engine calls only its inline glm_*()
# functions, never the compiled glmc_*() ones, so the library its own
# CMakeLists builds (a DLL by default) isn't needed.
add_library(cglm INTERFACE)
target_include_directories(cglm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/deps/cglm/include)

add_library(PBR_Engine STATIC
    # --- Runtime ---
    src/App.c
    src/Core/Log.c
    src/Core/FileIO.c
    src/Core/Paths.c
    src/Core/Image.c
    src/Core/Encoding.c
    src/Core/Process.c
    src/Platform/Window.c
    src/Platform/Input.c
    src/Platform/InputActions.c
    src/Platform/Dialog.c
    src/RHI/RHI.c
    src/RHI/OpenGL/RHI_GL.c
    src/RHI/OpenGL/RHI_GL_Buffer.c
    src/RHI/OpenGL/RHI_GL_Shader.c
    src/RHI/OpenGL/RHI_GL_Texture.c
    src/RHI/OpenGL/RHI_GL_Framebuffer.c
    src/Renderer/Renderer.c
    src/Renderer/GpuProfiler.c
    src/Renderer/IBL.c
    src/Renderer/Lighting.c
    src/Renderer/PostProcess.c
    src/Renderer/ReflectionProbes.c
    src/Renderer/SceneRenderer.c
    src/Renderer/ShaderReload.c
    src/Renderer/Shadow.c
    src/Renderer/SpotShadow.c
    src/Renderer/ShadowCache.c
    src/Renderer/PointShadow.c
    src/Scene/Camera.c
    src/Scene/OrbitCamera.c
    src/Scene/Light.c
    src/Scene/Mesh.c
    src/Scene/Material.c
    src/Scene/Model.c
    src/Scene/ModelObj.c
    src/Scene/ModelGltf.c
    src/Assets/AssetCache.c
    src/World/World.c
    src/World/Reflect.c
    src/World/Serialize.c
    src/World/Environment.c
    src/World/PlayerController.c
    src/World/SceneTransition.c
    src/Project/Project.c
    deps/cjson/cJSON.c

    # --- Tools ---
    src/Commands/Commands.c
    src/Commands/History.c
    src/Commands/JsonRpc.c
    src/Net/CommandServer.c
    src/Chat/ChatSession.c
    src/UI/ImGuiLayer.c
    src/UI/EntityPanel.c
    src/UI/Inspector.c
    src/UI/GizmoBridge.cpp
    src/UI/CommandConsole.c
    src/UI/LogPanel.c
    src/UI/ChatPanel.c
)

# PUBLIC: consumers #include headers like "App.h", "RHI/RHI.h", "Core/Paths.h"
# straight from this root. NOTE: this also exposes RHI/OpenGL/RHI_GL_Internal.h,
# which is meant to stay backend-private -- an include boundary this doesn't
# enforce (would need a dedicated include/PBR/ split), only documents via the
# comment in that file.
target_include_directories(PBR_Engine
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# PUBLIC: Scene/Camera.h includes <cglm/cglm.h> directly, and UI code calls
# cimgui's ig*() functions directly. glfw/glad are PUBLIC too, defensively,
# even though Platform/Window.h only forward-declares GLFWwindow as an opaque
# type today.
target_link_libraries(PBR_Engine PUBLIC
    glfw
    glad
    OpenGL::GL
    cglm
    cimgui
)

# Winsock for Net/CommandServer.c (POSIX sockets need no extra library).
if(WIN32)
    target_link_libraries(PBR_Engine PUBLIC ws2_32)
endif()

# stb_image, fast_obj and cgltf are single vendored headers (no CMakeLists of
# their own, unlike the git-submodule deps above), needed only by
# Core/Image.c, Scene/ModelObj.c and Scene/ModelGltf.c respectively -- all
# PRIVATE, same reasoning as the DEV_*_ROOT definitions below (implementation
# detail, not something consumers need to see).
target_include_directories(PBR_Engine PRIVATE deps/stb deps/fast_obj deps/cgltf)

# cJSON (vendored v1.7.18, MIT) is PUBLIC: World/Serialize.h and the command
# layer hand cJSON trees across the API.
target_include_directories(PBR_Engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/cjson)

# The development fallbacks for Core/Paths.h's two roots: the engine's data
# (shaders) and the default project (assets/). At runtime an engine/ folder
# next to the executable and --project <dir> take precedence, so a copied
# build doesn't depend on this source tree. PRIVATE: only Core/Paths.c reads
# them; the apps ask paths_engine_root() / paths_project_root().
target_compile_definitions(PBR_Engine PRIVATE
    DEV_ENGINE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    DEV_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/assets"
)

# --- SDK -----------------------------------------------------------------------
# `cmake --install build --config <Config> --prefix <dir>` makes a folder
# another project can use without CMake (sdk/README.md says how):
#
#   include/          the engine's headers (as in src/) and the ones they or
#                     a program need from the dependencies: cglm, cJSON,
#                     GLFW, glad (+ KHR), cimgui
#   lib/<Config>/     PBR_Engine.lib, glfw3.lib, glad.lib, cimgui.lib -- one
#                     folder per configuration: Debug and Release libraries
#                     use different C runtimes and can't be mixed
#   engine/shaders/   loaded at runtime: copy engine/ next to the program
#                     (Core/Paths.h)
#
# Installing Debug and Release into the same prefix fills both lib/ folders.

install(TARGETS PBR_Engine glfw glad cimgui
    ARCHIVE DESTINATION lib/\${CMAKE_INSTALL_CONFIG_NAME}
)

install(DIRECTORY src/ DESTINATION include
    FILES_MATCHING PATTERN "*.h"
    PATTERN "RHI_GL_Internal.h" EXCLUDE   # the GL backend's own
)
install(DIRECTORY deps/cglm/include/cglm DESTINATION include)
install(FILES     deps/cjson/cJSON.h DESTINATION include)
install(DIRECTORY deps/glfw/include/GLFW DESTINATION include)
install(DIRECTORY deps/glad/include/ DESTINATION include)
install(FILES     deps/cimgui/cimgui.h deps/cimgui/cimgui_impl.h DESTINATION include)

install(DIRECTORY shaders DESTINATION engine)
install(FILES sdk/README.md DESTINATION .)
