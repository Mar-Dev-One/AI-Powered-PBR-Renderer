#include "ImGuiLayer.h"

#include "cimgui.h"
#include "cimgui_impl.h"

#include <GLFW/glfw3.h>

void imgui_layer_init(window* wind)
{
    igCreateContext(NULL);

    ImGuiIO* io = igGetIO_Nil();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io->ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Lets a docked-out panel become its own real OS window instead of
    // being confined to the main GLFW window -- what actually separates
    // the GUI from the main window. Needs the platform-window render step
    // in imgui_layer_render_platform_windows() below to go with it.
    io->ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    igStyleColorsDark(NULL);

    // When viewports are on, ImGui gives the main window's own background
    // the same rounding/alpha as floating windows unless we flatten it --
    // otherwise the main viewport gets a border/rounded corners that look
    // like a bug.
    ImGuiStyle* style = igGetStyle();
    if (io->ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style->WindowRounding = 0.0f;
        style->Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    GLFWwindow* handle = window_get_native_handle(wind);
    ImGui_ImplGlfw_InitForOpenGL(handle, true);
    ImGui_ImplOpenGL3_Init("#version 460");
}

void imgui_layer_shutdown(void)
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    igDestroyContext(NULL);
}

void imgui_layer_new_frame(void)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    igNewFrame();
}

void imgui_layer_render(void)
{
    igRender();
    ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
}

void imgui_layer_render_platform_windows(void)
{
    ImGuiIO* io = igGetIO_Nil();
    if (!(io->ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
        return;

    // Drawing the popped-out windows makes their own GL contexts current;
    // save/restore the main window's context around it so the next frame's
    // rendering still lands on the right window.
    GLFWwindow* backup_current_context = glfwGetCurrentContext();
    igUpdatePlatformWindows();
    igRenderPlatformWindowsDefault(NULL, NULL);
    glfwMakeContextCurrent(backup_current_context);
}

b8 imgui_layer_wants_keyboard(void)
{
    return igGetIO_Nil()->WantCaptureKeyboard;
}

b8 imgui_layer_wants_mouse(void)
{
    return igGetIO_Nil()->WantCaptureMouse;
}

const app_ui_layer*
imgui_layer(void)
{
    static const app_ui_layer layer = {
        .init                    = imgui_layer_init,
        .shutdown                = imgui_layer_shutdown,
        .new_frame               = imgui_layer_new_frame,
        .render                  = imgui_layer_render,
        .render_platform_windows = imgui_layer_render_platform_windows,
        .wants_keyboard          = imgui_layer_wants_keyboard,
        .wants_mouse             = imgui_layer_wants_mouse,
    };
    return &layer;
}
