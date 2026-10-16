#include "Window.h"

#include "GLFW/glfw3.h"

#include <string.h>

#define WINDOW_TITLE_MAX_LEN 128

struct window {
    GLFWwindow* handle;
    char        title[WINDOW_TITLE_MAX_LEN];
    uint16      width;
    uint16      height;
    b8          vsync;
};

window* window_create(window_descriptor desc)
{
    if (!glfwInit())
        FATAL("GLFW has not been initialized properly");

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

    GLFWmonitor* monitor = NULL;
    if (desc.fullscreen)
    {
        monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : NULL;
        if (mode && (desc.width == 0 || desc.height == 0))
        {
            desc.width  = (uint16)mode->width;
            desc.height = (uint16)mode->height;
        }
    }
    if (desc.width == 0 || desc.height == 0)
    {
        desc.width  = 800;
        desc.height = 600;
    }

    GLFWwindow* handle = glfwCreateWindow(desc.width, desc.height, desc.title, monitor, NULL);

    if (!handle) {
        glfwTerminate();
        FATAL("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(handle);
    glfwSwapInterval(desc.vsync ? 1 : 0);

    window* wind = malloc(sizeof(window));

    // The framebuffer's size, as the resize callback reports it: it differs
    // from the requested window size on high-DPI screens and in fullscreen.
    int framebuffer_width, framebuffer_height;
    glfwGetFramebufferSize(handle, &framebuffer_width, &framebuffer_height);

    wind->handle = handle;
    wind->width = (uint16)framebuffer_width;
    wind->height = (uint16)framebuffer_height;
    wind->vsync = desc.vsync;
    strncpy(wind->title, desc.title, WINDOW_TITLE_MAX_LEN - 1);
    wind->title[WINDOW_TITLE_MAX_LEN - 1] = '\0';

    return wind;
}


gl_proc_loader window_get_gl_loader(void)
{
    return (gl_proc_loader)glfwGetProcAddress;
}

GLFWwindow* window_get_native_handle(window* wind)
{
    return wind->handle;
}

void window_poll_events(void)
{
    glfwPollEvents();
}

f64 window_get_time(void)
{
    return glfwGetTime();
}

void window_swap_buffers(window* wind)
{
    glfwSwapBuffers(wind->handle);
}

b8 window_should_close(window* wind)
{
    return glfwWindowShouldClose(wind->handle);
}

void window_close(window* wind)
{
    glfwSetWindowShouldClose(wind->handle, GLFW_TRUE);
}

void window_cancel_close(window* wind)
{
    glfwSetWindowShouldClose(wind->handle, GLFW_FALSE);
}

window_size window_get_size(window* wind)
{
    window_size size = {
        .width = wind->width,
        .height = wind->height
    };

    return size;
}


void window_set_size(window* wind, uint16 width, uint16 height)
{
    glfwSetWindowSize(wind->handle, width, height);
    wind->width = width;
    wind->height = height;
}

void window_on_resized(window* wind, uint16 width, uint16 height)
{
    wind->width = width;
    wind->height = height;
}

void window_set_title(window* wind, const char* title)
{
    strncpy(wind->title, title, WINDOW_TITLE_MAX_LEN - 1);
    wind->title[WINDOW_TITLE_MAX_LEN - 1] = '\0';
    glfwSetWindowTitle(wind->handle, wind->title);
}



void window_set_key_callback(window* wind, key_callback callback)
{
    glfwSetKeyCallback(wind->handle, callback);
}

void window_set_resize_callback(window* wind, resize_callback callback)
{
    glfwSetFramebufferSizeCallback(wind->handle, callback);
}

void window_set_scroll_callback(window* wind, scroll_callback callback)
{
    glfwSetScrollCallback(wind->handle, callback);
}

void window_get_cursor_pos(window* wind, f64* out_x, f64* out_y)
{
    glfwGetCursorPos(wind->handle, out_x, out_y);
}

b8 window_is_mouse_button_down(window* wind, int button)
{
    return glfwGetMouseButton(wind->handle, button) == GLFW_PRESS;
}



void window_destroy(window* wind)
{
    glfwDestroyWindow(wind->handle);
    free(wind);
    LOG_INFO("Window destroyed!");

    //TODO : add GLFW Terminate; not now because we may have multiple windows
}