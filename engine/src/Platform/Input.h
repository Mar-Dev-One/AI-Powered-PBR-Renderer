#pragma once

#include "../Core/Defines.h"
#include "Window.h"

// Keyboard and mouse state, one snapshot per frame.
//
// App.c feeds it from the window's events and calls input_begin_frame() before
// each frame's events are polled, so from App.on_frame on:
//   down      the key/button is held now
//   pressed   it went down during this frame (a tap shorter than a frame
//             still counts, along with `released`)
//   released  it came up during this frame
//
// With a UI layer, presses the UI claims (App.ui->wants_keyboard/wants_mouse)
// never arrive, the same rule as App.on_key; releases always do, so nothing
// stays held. Losing window focus releases everything.
//
// One instance for the process: the engine draws one window.

// Same values as GLFW's GLFW_KEY_* (Input.c checks), so a GLFW key code from
// App.on_key can be passed straight in.
typedef enum input_key
{
    INPUT_KEY_SPACE         = 32,
    INPUT_KEY_APOSTROPHE    = 39,
    INPUT_KEY_COMMA         = 44,
    INPUT_KEY_MINUS         = 45,
    INPUT_KEY_PERIOD        = 46,
    INPUT_KEY_SLASH         = 47,
    INPUT_KEY_0 = 48, INPUT_KEY_1, INPUT_KEY_2, INPUT_KEY_3, INPUT_KEY_4,
    INPUT_KEY_5, INPUT_KEY_6, INPUT_KEY_7, INPUT_KEY_8, INPUT_KEY_9,
    INPUT_KEY_SEMICOLON     = 59,
    INPUT_KEY_EQUAL         = 61,
    INPUT_KEY_A = 65, INPUT_KEY_B, INPUT_KEY_C, INPUT_KEY_D, INPUT_KEY_E, INPUT_KEY_F,
    INPUT_KEY_G, INPUT_KEY_H, INPUT_KEY_I, INPUT_KEY_J, INPUT_KEY_K, INPUT_KEY_L,
    INPUT_KEY_M, INPUT_KEY_N, INPUT_KEY_O, INPUT_KEY_P, INPUT_KEY_Q, INPUT_KEY_R,
    INPUT_KEY_S, INPUT_KEY_T, INPUT_KEY_U, INPUT_KEY_V, INPUT_KEY_W, INPUT_KEY_X,
    INPUT_KEY_Y, INPUT_KEY_Z,
    INPUT_KEY_LEFT_BRACKET  = 91,
    INPUT_KEY_BACKSLASH     = 92,
    INPUT_KEY_RIGHT_BRACKET = 93,
    INPUT_KEY_GRAVE_ACCENT  = 96,

    INPUT_KEY_ESCAPE        = 256,
    INPUT_KEY_ENTER, INPUT_KEY_TAB, INPUT_KEY_BACKSPACE, INPUT_KEY_INSERT, INPUT_KEY_DELETE,
    INPUT_KEY_RIGHT, INPUT_KEY_LEFT, INPUT_KEY_DOWN, INPUT_KEY_UP,
    INPUT_KEY_PAGE_UP, INPUT_KEY_PAGE_DOWN, INPUT_KEY_HOME, INPUT_KEY_END,
    INPUT_KEY_CAPS_LOCK     = 280,
    INPUT_KEY_F1 = 290, INPUT_KEY_F2, INPUT_KEY_F3, INPUT_KEY_F4, INPUT_KEY_F5, INPUT_KEY_F6,
    INPUT_KEY_F7, INPUT_KEY_F8, INPUT_KEY_F9, INPUT_KEY_F10, INPUT_KEY_F11, INPUT_KEY_F12,
    INPUT_KEY_KP_0 = 320, INPUT_KEY_KP_1, INPUT_KEY_KP_2, INPUT_KEY_KP_3, INPUT_KEY_KP_4,
    INPUT_KEY_KP_5, INPUT_KEY_KP_6, INPUT_KEY_KP_7, INPUT_KEY_KP_8, INPUT_KEY_KP_9,
    INPUT_KEY_LEFT_SHIFT    = 340,
    INPUT_KEY_LEFT_CONTROL, INPUT_KEY_LEFT_ALT, INPUT_KEY_LEFT_SUPER,
    INPUT_KEY_RIGHT_SHIFT, INPUT_KEY_RIGHT_CONTROL, INPUT_KEY_RIGHT_ALT, INPUT_KEY_RIGHT_SUPER,
    INPUT_KEY_MENU          = 348,

    INPUT_KEY_COUNT         = 349
} input_key;

// GLFW_MOUSE_BUTTON_* values.
typedef enum input_mouse_button
{
    INPUT_MOUSE_LEFT   = 0,
    INPUT_MOUSE_RIGHT  = 1,
    INPUT_MOUSE_MIDDLE = 2,
    INPUT_MOUSE_BUTTON_COUNT = 8   // 3-7: extra side buttons
} input_mouse_button;

// --- Reading (from App.on_frame) ---------------------------------------------------

// Unknown codes (GLFW's -1, anything out of range) read as never down.
b8 input_key_down(input_key key);
b8 input_key_pressed(input_key key);
b8 input_key_released(input_key key);

b8 input_mouse_down(input_mouse_button button);
b8 input_mouse_pressed(input_mouse_button button);
b8 input_mouse_released(input_mouse_button button);

// Cursor position in window pixels (top left = 0,0), and how far it moved this
// frame. While locked the position is virtual and unbounded; use the delta.
void input_mouse_position(f64* out_x, f64* out_y);
void input_mouse_delta(f32* out_dx, f32* out_dy);

// Wheel movement this frame: y > 0 away from the user.
void input_scroll(f32* out_dx, f32* out_dy);

// --- Cursor ----------------------------------------------------------------------

// Locked hides the cursor and keeps it in the window, for mouse-look; it uses
// raw (unaccelerated) motion where the system has it. Go through this rather
// than the window, so the jump the cursor makes on a mode change isn't read as
// movement.
void               input_set_cursor_mode(window* wind, window_cursor_mode mode);
window_cursor_mode input_cursor_mode(void);

// --- Feeding (App.c; tests) ------------------------------------------------------

// Before each frame's events are polled: clears last frame's pressed/released
// and its mouse and scroll movement.
void input_begin_frame(void);

void input_on_key(int key, b8 down);
void input_on_mouse_button(int button, b8 down);
void input_on_cursor(f64 x, f64 y, b8 counts_as_movement);   // false: moved over the UI
void input_on_scroll(f64 dx, f64 dy);
void input_on_focus(b8 focused);   // losing it releases every key and button

// Everything up, no movement, cursor mode back to normal (without touching a window).
void input_reset(void);
