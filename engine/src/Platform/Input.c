#include "Input.h"

#include "GLFW/glfw3.h"

#include <string.h>

// input_key is GLFW's numbering; spot-check both ends of each range.
STATIC_ASSERT(INPUT_KEY_SPACE == GLFW_KEY_SPACE && INPUT_KEY_9 == GLFW_KEY_9 && INPUT_KEY_Z == GLFW_KEY_Z, "input_key != GLFW");
STATIC_ASSERT(INPUT_KEY_GRAVE_ACCENT == GLFW_KEY_GRAVE_ACCENT && INPUT_KEY_END == GLFW_KEY_END, "input_key != GLFW");
STATIC_ASSERT(INPUT_KEY_F12 == GLFW_KEY_F12 && INPUT_KEY_KP_9 == GLFW_KEY_KP_9, "input_key != GLFW");
STATIC_ASSERT(INPUT_KEY_RIGHT_SUPER == GLFW_KEY_RIGHT_SUPER && INPUT_KEY_COUNT == GLFW_KEY_LAST + 1, "input_key != GLFW");
STATIC_ASSERT(INPUT_MOUSE_MIDDLE == GLFW_MOUSE_BUTTON_MIDDLE && INPUT_MOUSE_BUTTON_COUNT == GLFW_MOUSE_BUTTON_LAST + 1, "input_mouse_button != GLFW");

enum
{
    STATE_DOWN     = 1 << 0,
    STATE_PRESSED  = 1 << 1,
    STATE_RELEASED = 1 << 2,
};

static struct
{
    uint8 keys[INPUT_KEY_COUNT];
    uint8 buttons[INPUT_MOUSE_BUTTON_COUNT];

    f64 cursor_x, cursor_y;
    b8  has_cursor;        // false until a position arrives (or after a mode change): the next one sets it without moving
    f32 delta_x, delta_y;
    f32 scroll_x, scroll_y;

    window_cursor_mode cursor_mode;
} input;

static void
set_state(uint8* state, b8 down)
{
    if (down && !(*state & STATE_DOWN))
        *state |= STATE_DOWN | STATE_PRESSED;
    else if (!down && (*state & STATE_DOWN))
        *state = (uint8)((*state & ~STATE_DOWN) | STATE_RELEASED);
}

static uint8
key_state(input_key key)
{
    return (int)key >= 0 && key < INPUT_KEY_COUNT ? input.keys[key] : 0;
}

static uint8
button_state(input_mouse_button button)
{
    return (int)button >= 0 && button < INPUT_MOUSE_BUTTON_COUNT ? input.buttons[button] : 0;
}

b8 input_key_down(input_key key)     { return (key_state(key) & STATE_DOWN) != 0; }
b8 input_key_pressed(input_key key)  { return (key_state(key) & STATE_PRESSED) != 0; }
b8 input_key_released(input_key key) { return (key_state(key) & STATE_RELEASED) != 0; }

b8 input_mouse_down(input_mouse_button button)     { return (button_state(button) & STATE_DOWN) != 0; }
b8 input_mouse_pressed(input_mouse_button button)  { return (button_state(button) & STATE_PRESSED) != 0; }
b8 input_mouse_released(input_mouse_button button) { return (button_state(button) & STATE_RELEASED) != 0; }

void
input_mouse_position(f64* out_x, f64* out_y)
{
    *out_x = input.cursor_x;
    *out_y = input.cursor_y;
}

void
input_mouse_delta(f32* out_dx, f32* out_dy)
{
    *out_dx = input.delta_x;
    *out_dy = input.delta_y;
}

void
input_scroll(f32* out_dx, f32* out_dy)
{
    *out_dx = input.scroll_x;
    *out_dy = input.scroll_y;
}

void
input_set_cursor_mode(window* wind, window_cursor_mode mode)
{
    if (mode == input.cursor_mode)
        return;

    window_set_cursor_mode(wind, mode);
    input.cursor_mode = mode;
    input.has_cursor  = false;
}

window_cursor_mode
input_cursor_mode(void)
{
    return input.cursor_mode;
}

void
input_begin_frame(void)
{
    for (int i = 0; i < INPUT_KEY_COUNT; ++i)
        input.keys[i] &= STATE_DOWN;
    for (int i = 0; i < INPUT_MOUSE_BUTTON_COUNT; ++i)
        input.buttons[i] &= STATE_DOWN;

    input.delta_x = input.delta_y = 0.0f;
    input.scroll_x = input.scroll_y = 0.0f;
}

void
input_on_key(int key, b8 down)
{
    if (key >= 0 && key < INPUT_KEY_COUNT)
        set_state(&input.keys[key], down);
}

void
input_on_mouse_button(int button, b8 down)
{
    if (button >= 0 && button < INPUT_MOUSE_BUTTON_COUNT)
        set_state(&input.buttons[button], down);
}

void
input_on_cursor(f64 x, f64 y, b8 counts_as_movement)
{
    if (input.has_cursor && counts_as_movement)
    {
        input.delta_x += (f32)(x - input.cursor_x);
        input.delta_y += (f32)(y - input.cursor_y);
    }
    input.cursor_x   = x;
    input.cursor_y   = y;
    input.has_cursor = true;
}

void
input_on_scroll(f64 dx, f64 dy)
{
    input.scroll_x += (f32)dx;
    input.scroll_y += (f32)dy;
}

void
input_on_focus(b8 focused)
{
    if (focused)
        return;

    for (int i = 0; i < INPUT_KEY_COUNT; ++i)
        set_state(&input.keys[i], false);
    for (int i = 0; i < INPUT_MOUSE_BUTTON_COUNT; ++i)
        set_state(&input.buttons[i], false);

    // Coming back, the cursor may be anywhere.
    input.has_cursor = false;
}

void
input_reset(void)
{
    memset(&input, 0, sizeof(input));
}
