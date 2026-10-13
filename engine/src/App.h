#pragma once

#include "Core/Defines.h"

struct App;

// All hooks are optional (may be NULL). With use_imgui set, input hooks only
// fire for events Dear ImGui did not claim (typing in a text box, scrolling a
// panel), so an app never has to check ImGui's capture flags itself. Key and
// mouse codes are GLFW's for now.

// Once, after the window, GL context, RHI and ImGui exist -- create GPU
// resources here.
typedef void (*app_init_callback)(struct App* app);

// Once per frame, after the frame is cleared and before it's presented.
// This is the extension point application code (and tests) use to actually
// draw something through the RHI/Renderer. dt is seconds since the last frame.
typedef void (*app_frame_callback)(struct App* app, f32 dt);

// Once, from terminate(), while the GL context is still alive -- destroy
// GPU resources here.
typedef void (*app_shutdown_callback)(struct App* app);

typedef void (*app_key_callback)(struct App* app, int key, int action, int mods);
typedef void (*app_scroll_callback)(struct App* app, f64 dx, f64 dy);

// Framebuffer size in pixels. The viewport has already been updated by the
// time this runs; use it to resize FBOs / update camera aspect. Can report
// 0x0 while the window is minimized.
typedef void (*app_resize_callback)(struct App* app, uint16 width, uint16 height);

typedef struct App
{
    char* name;

    app_init_callback     on_init;
    app_frame_callback    on_frame;
    app_shutdown_callback on_shutdown;
    app_key_callback      on_key;
    app_scroll_callback   on_scroll;
    app_resize_callback   on_resize;

    void* user_data;             // optional, passed through to every hook via app->user_data
    b8 should_close;             // set this from a hook to exit run() without closing the window

    // Dear ImGui (UI/ImGuiLayer.h) for tools like the editor. Off for a game
    // runtime: ImGui is never initialised, so no ig*() / imgui_layer_*() calls.
    b8 use_imgui;
} App;


void run(App* app);
void terminate(App* app);