# Two libraries from this one folder:
#
#   PBR_Runtime  what a game needs to load and play a project: window, RHI,
#                renderer, models and assets, the World and its scene files,
#                project settings. No UI library, no sockets. game/ links it.
#   PBR_Tools    what the editor adds on top: the command layer (and its undo
#                history), the command server, the chat session and the
#                Dear ImGui panels. editor/ links it (and through it, the
#                runtime).
#
# Runtime code never includes a Tools header: App reaches the editor's UI only
# through the App.ui hook table (App.h).

add_subdirectory(deps/glfw)
add_subdirectory(deps/glad)
add_subdirectory(deps/cglm)
include(cmake/cimgui.cmake)

# --- PBR_Runtime ----------------------------------------------------------------

add_library(PBR_Runtime STATIC
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
    src/Project/Project.c
    deps/cjson/cJSON.c
)

# PUBLIC: consumers #include headers like "App.h", "RHI/RHI.h", "Core/Paths.h"
# straight from this root. NOTE: this also exposes RHI/OpenGL/RHI_GL_Internal.h,
# which is meant to stay backend-private -- an include boundary this doesn't
# enforce (would need a dedicated include/PBR/ split), only documents via the
# comment in that file.
target_include_directories(PBR_Runtime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# PUBLIC: Scene/Camera.h includes <cglm/cglm.h> directly. glfw/glad are PUBLIC
# too, defensively, even though Platform/Window.h only forward-declares
# GLFWwindow as an opaque type today.
target_link_libraries(PBR_Runtime PUBLIC
    glfw
    glad
    OpenGL::GL
    cglm
)

# stb_image, fast_obj and cgltf are single vendored headers (no CMakeLists of
# their own, unlike the git-submodule deps above), needed only by
# Core/Image.c, Scene/ModelObj.c and Scene/ModelGltf.c respectively -- all
# PRIVATE, same reasoning as the DEV_*_ROOT definitions below (implementation
# detail, not something consumers need to see).
target_include_directories(PBR_Runtime PRIVATE deps/stb deps/fast_obj deps/cgltf)

# cJSON (vendored v1.7.18, MIT) is PUBLIC: World/Serialize.h and the command
# layer hand cJSON trees across the API.
target_include_directories(PBR_Runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/cjson)

# The development fallbacks for Core/Paths.h's two roots: the engine's data
# (shaders) and the default project (assets/). At runtime an engine/ folder
# next to the executable and --project <dir> take precedence, so a copied
# build doesn't depend on this source tree. PRIVATE: only Core/Paths.c reads
# them; the apps ask paths_engine_root() / paths_project_root().
target_compile_definitions(PBR_Runtime PRIVATE
    DEV_ENGINE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    DEV_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/assets"
)

# --- PBR_Tools ------------------------------------------------------------------

add_library(PBR_Tools STATIC
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
    src/UI/ChatPanel.c
    src/UI/LogPanel.c
)

# PUBLIC: the editor calls cimgui's ig*() functions directly.
target_link_libraries(PBR_Tools PUBLIC
    PBR_Runtime
    cimgui
)

# Winsock for Net/CommandServer.c (POSIX sockets need no extra library).
if(WIN32)
    target_link_libraries(PBR_Tools PUBLIC ws2_32)
endif()
