#pragma once

#include "../Core/Defines.h"

typedef struct window window;

typedef struct window_descriptor {
    const char* title;
    uint16      width;
    uint16      height;
    b8          resizable;
    b8          vsync;
    b8          fullscreen;   // on the primary monitor; width/height 0 = its current video mode
} window_descriptor;

typedef struct window_size {
    uint16 width;
    uint16 height;
} window_size;


//TODO : Not expose the GLFW types like GLFWwindow
typedef struct GLFWwindow GLFWwindow;
typedef void (*key_callback)(GLFWwindow* window, int key, int scancode, int action, int mods);
typedef void (*resize_callback)(GLFWwindow* window, int width, int height);
typedef void (*scroll_callback)(GLFWwindow* window, double xoffset, double yoffset);

// Generic GL proc-address loader, handed to the RHI backend so it can
// resolve GL functions without RHI having to include GLFW itself.
typedef void* (*gl_proc_loader)(const char* name);


window* window_create(window_descriptor desc);

gl_proc_loader window_get_gl_loader(void);

// Raw GLFWwindow* handle. Needed by things that sit outside the RHI and
// have to talk to GLFW directly (e.g. the ImGui GLFW backend). Everything
// else should keep going through the window* API above.
GLFWwindow* window_get_native_handle(window* wind);

void window_poll_events(void);
void window_swap_buffers(window* wind);

// Seconds elapsed since the first call to window_create() in this process.
// Used for animation/timing (e.g. rotating something based on elapsed time)
// without callers having to include GLFW directly.
f64  window_get_time(void);
b8   window_should_close(window* wind);
void window_close(window* wind);
// Clears a pending close request (window_should_close() is false again).
void window_cancel_close(window* wind);

void window_set_size(window* wind, uint16 width, uint16 height);
void window_set_title(window* wind, const char* title);

// Updates the window's cached width/height WITHOUT calling glfwSetWindowSize.
// Use this from a resize/framebuffer-size callback, where GLFW has already
// resized the window and you just need to record the new size — calling
// window_set_size() there would issue a redundant resize request back at
// GLFW from inside its own callback.
void window_on_resized(window* wind, uint16 width, uint16 height);

void window_set_key_callback(window* wind, key_callback callback);
void window_set_resize_callback(window* wind, resize_callback callback);

// NOTE: like the other setters, this replaces whatever callback is already
// installed. Register before imgui_layer_init() so ImGui chains to it (see
// App.c) rather than being replaced by it.
void window_set_scroll_callback(window* wind, scroll_callback callback);

// Polled input, for continuous things like drag-to-orbit where a callback
// would just be re-accumulating state. `button` is a GLFW_MOUSE_BUTTON_* code.
void window_get_cursor_pos(window* wind, f64* out_x, f64* out_y);
b8   window_is_mouse_button_down(window* wind, int button);

window_size window_get_size(window* wind);

void window_destroy(window* wind);