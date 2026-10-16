#include "App.h"

#include "Renderer/Renderer.h"

#include <GLFW/glfw3.h>

// One App per process (there is one renderer and one window), and the
// window's GLFW callbacks carry no user pointer, so they reach the App
// through this. Owned here so applications never touch GLFW callbacks.
static App* g_app = NULL;

static void glfw_key_cb(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (!g_app || !g_app->on_key)
        return;

    // Always forward releases: if the UI grabbed focus between press and
    // release, swallowing the release would leave the app thinking the key
    // is still held.
    if (action != GLFW_RELEASE && g_app->ui && g_app->ui->wants_keyboard())
        return;

    g_app->on_key(g_app, key, action, mods);
}

static void glfw_scroll_cb(GLFWwindow* window, double xoffset, double yoffset)
{
    if (!g_app || !g_app->on_scroll)
        return;

    if (g_app->ui && g_app->ui->wants_mouse())
        return;

    g_app->on_scroll(g_app, xoffset, yoffset);
}

static void glfw_resize_cb(GLFWwindow* window, int width, int height)
{
    renderer rend = get_renderer();
    // GLFW already resized the window (this callback IS the notification) —
    // just record the new size and update the viewport. Calling
    // window_set_size() here would call glfwSetWindowSize() again from
    // inside GLFW's own resize callback.
    window_on_resized(rend->drawing_window, (uint16)width, (uint16)height);
    renderer_set_viewport(0, 0, (uint16)width, (uint16)height);

    if (g_app && g_app->on_resize)
        g_app->on_resize(g_app, (uint16)width, (uint16)height);
}

void run(App* app)
{
    g_app = app;

    window_descriptor desc = {
        .title = app->name,
        .width = app->width,
        .height = app->height,
        .resizable = true,
        .vsync = true,
        .fullscreen = app->fullscreen,
        .hidden = app->hidden
    };

    init_renderer(desc);

    renderer rend = get_renderer();

    // These MUST be registered before the UI layer's init(). Dear ImGui's
    // GLFW backend saves whatever callback is already installed and chains
    // to it (the UI sees the event first, then ours). Registering after
    // would replace its callback and break UI input.
    window_set_key_callback(rend->drawing_window, glfw_key_cb);
    window_set_scroll_callback(rend->drawing_window, glfw_scroll_cb);
    window_set_resize_callback(rend->drawing_window, glfw_resize_cb);

    // The UI needs a live GL context + window, so it can only be set up
    // after init_renderer() above has created both.
    if (app->ui)
        app->ui->init(rend->drawing_window);

    if (app->on_init)
        app->on_init(app);

    f64 last_time = window_get_time();

    while (!app->should_close) {
        if (window_should_close(rend->drawing_window)) {
            if (app->on_close_request && !app->on_close_request(app))
                window_cancel_close(rend->drawing_window);
            else
                break;
        }

        f64 now = window_get_time();
        f32 dt = (f32)(now - last_time);
        last_time = now;

        renderer_begin_frame();

        renderer_clear(0.4f, 0.1f, 0.12f, 1.0f);

        if (app->ui)
            app->ui->new_frame();

        if (app->on_frame)
            app->on_frame(app, dt);

        // Draws on top of whatever on_frame just rendered, still before
        // the swap so it actually shows up on screen.
        if (app->ui)
            app->ui->render();

        renderer_end_frame();

        // Must come after the swap above -- it makes the popped-out
        // windows' own GL contexts current to draw them.
        if (app->ui)
            app->ui->render_platform_windows();
    }
}

void terminate(App* app)
{
    // The GL context is still alive here, so this is where the app frees
    // its GPU resources.
    if (app && app->on_shutdown)
        app->on_shutdown(app);

    if (app && app->ui)
        app->ui->shutdown();
    terminate_renderer();

    g_app = NULL;
}