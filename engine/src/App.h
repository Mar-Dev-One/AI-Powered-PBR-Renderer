#pragma once

#include "Core/Defines.h"
#include "Platform/Window.h"

struct App;

// All hooks are optional (may be NULL). With a UI layer (App.ui), input
// hooks only fire for events the UI did not claim (typing in a text box,
// scrolling a panel), so an app never has to check its capture flags
// itself. Key and mouse codes are GLFW's for now.

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

// The window's close button (or the OS) asked to quit. Return true to let it
// close, false to keep running -- e.g. to ask about unsaved changes first and
// set should_close later. Without this hook the window just closes.
typedef b8 (*app_close_request_callback)(struct App* app);

// An immediate-mode UI drawn over every frame -- the editor's Dear ImGui
// (UI/ImGuiLayer.h imgui_layer()). The runtime only calls these, so a game
// built without one links no UI library at all. Every function is required.
typedef struct app_ui_layer
{
    void (*init)(window* wind);           // after the window and GL context exist, before App.on_init
    void (*shutdown)(void);               // after App.on_shutdown
    void (*new_frame)(void);              // before App.on_frame
    void (*render)(void);                 // after App.on_frame, before the buffer swap
    void (*render_platform_windows)(void);   // after the swap: panels dragged out into their own OS windows
    b8   (*wants_keyboard)(void);         // the UI claims this frame's keyboard / mouse events
    b8   (*wants_mouse)(void);
} app_ui_layer;

typedef struct App
{
    char* name;

    app_init_callback     on_init;
    app_frame_callback    on_frame;
    app_shutdown_callback on_shutdown;
    app_key_callback      on_key;
    app_scroll_callback   on_scroll;
    app_resize_callback   on_resize;
    app_close_request_callback on_close_request;

    void* user_data;             // optional, passed through to every hook via app->user_data
    b8 should_close;             // set this from a hook to exit run() without closing the window

    // Window size in pixels; 0 = 800 x 600, or the monitor's own resolution
    // when fullscreen.
    uint16 width;
    uint16 height;
    b8     fullscreen;
    b8     hidden;    // a window that's never shown, for rendering offscreen only
    b8     maximized; // fills the screen but for the taskbar; width/height is the restored size

    // NULL for a game runtime; the editor's is UI/ImGuiLayer.h imgui_layer().
    const app_ui_layer* ui;
} App;


void run(App* app);
void terminate(App* app);