#pragma once

#include "../App.h"
#include "../Core/Defines.h"
#include "../Platform/Window.h"

// Thin wrapper around cimgui (Dear ImGui's C API) + its GLFW/OpenGL3
// backends. Keeps every ImGui/cimgui header out of the rest of the
// codebase -- callers only ever see this file.

void imgui_layer_init(window* wind);
void imgui_layer_shutdown(void);

// Call once per frame, after renderer_begin_frame() and before any ImGui
// ig*() calls (including app->on_frame, if it draws UI).
void imgui_layer_new_frame(void);

// Call once per frame, after app->on_frame and before renderer_end_frame()
// / the buffer swap -- this is what actually draws the UI on top of
// whatever the renderer already drew.
void imgui_layer_render(void);

// Call once per frame, AFTER renderer_end_frame() (i.e. after the main
// window's buffer swap). Draws any panels that got dragged out into their
// own OS windows -- only does anything when ImGuiConfigFlags_ViewportsEnable
// is set (see imgui_layer_init).
void imgui_layer_render_platform_windows(void);

// True when ImGui wants the keyboard / mouse this frame (a text box has
// focus, the cursor is over a panel, ...). App.c uses these to keep
// ImGui-claimed events away from the application's input hooks; polled
// input (e.g. drag-to-orbit) should check wants_mouse itself.
b8 imgui_layer_wants_keyboard(void);
b8 imgui_layer_wants_mouse(void);

// All of the above as App's UI layer: set App.ui to this and run() calls
// them at the right points of the frame.
const app_ui_layer* imgui_layer(void);
