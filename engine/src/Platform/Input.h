#pragma once

#include "../Core/Defines.h"
#include "Window.h"

// Keyboard, mouse and gamepad state, one snapshot per frame.
//
// App.c feeds it from the window's events, calls input_begin_frame() before
// each frame's events are polled and input_poll_pads() after, so from
// App.on_frame on (keys, mouse buttons and pad buttons alike):
//   down      the key/button is held now
//   pressed   it went down during this frame (a tap shorter than a frame
//             still counts, along with `released`)
//   released  it came up during this frame
//
// With a UI layer, presses the UI claims (App.ui->wants_keyboard/wants_mouse)
// never arrive, the same rule as App.on_key; releases always do, so nothing
// stays held. Losing window focus releases everything, pads included.
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

// Gamepads: anything GLFW has a mapping for (Xbox, PlayStation, Switch Pro and
// most others), laid out as an Xbox pad. A pad's number is its GLFW joystick
// slot, so it keeps it while connected; INPUT_PAD_ANY reads all of them.
#define INPUT_MAX_PADS 16
#define INPUT_PAD_ANY  (-1)

// GLFW_GAMEPAD_BUTTON_* values. A/B/X/Y by position: A is Cross on a PlayStation pad.
typedef enum input_pad_button
{
    INPUT_PAD_A, INPUT_PAD_B, INPUT_PAD_X, INPUT_PAD_Y,
    INPUT_PAD_LEFT_BUMPER, INPUT_PAD_RIGHT_BUMPER,
    INPUT_PAD_BACK, INPUT_PAD_START, INPUT_PAD_GUIDE,
    INPUT_PAD_LEFT_THUMB, INPUT_PAD_RIGHT_THUMB,   // the sticks, clicked
    INPUT_PAD_DPAD_UP, INPUT_PAD_DPAD_RIGHT, INPUT_PAD_DPAD_DOWN, INPUT_PAD_DPAD_LEFT,
    INPUT_PAD_BUTTON_COUNT
} input_pad_button;

// GLFW_GAMEPAD_AXIS_* values.
typedef enum input_pad_axis
{
    INPUT_PAD_LEFT_X, INPUT_PAD_LEFT_Y,
    INPUT_PAD_RIGHT_X, INPUT_PAD_RIGHT_Y,
    INPUT_PAD_LEFT_TRIGGER, INPUT_PAD_RIGHT_TRIGGER,
    INPUT_PAD_AXIS_COUNT
} input_pad_axis;

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

// A pad number out of range, or one not connected, reads as nothing pressed.
b8          input_pad_connected(int pad);    // INPUT_PAD_ANY: is any
const char* input_pad_name(int pad);         // "" if not connected
b8          input_pad_down(int pad, input_pad_button button);
b8          input_pad_pressed(int pad, input_pad_button button);
b8          input_pad_released(int pad, input_pad_button button);

// Sticks -1..1, past a round dead zone and rescaled so they still start from 0
// at its edge; y is +1 pushed down (toward the player), as GLFW reports it.
// Triggers 0 (at rest) .. 1. INPUT_PAD_ANY: the pad pushing that axis furthest.
f32 input_pad_axis_value(int pad, input_pad_axis axis);

// How far a stick moves before it counts, 0..0.9 of its travel (default 0.2;
// worn sticks drift more). Triggers use a fixed 0.05.
void input_set_stick_dead_zone(f32 dead_zone);

// --- Names -------------------------------------------------------------------------
// Lower case, as bindings are written (InputActions.h): "w", "space",
// "left_shift", "f1", "kp_0"; "left", "right", "middle", "button4".."button8";
// "a", "left_bumper", "dpad_up"; "left_x", "right_trigger". Unknown codes give
// ""; reading ignores case.

const char* input_key_name(input_key key);
const char* input_mouse_button_name(input_mouse_button button);
const char* input_pad_button_name(input_pad_button button);
const char* input_pad_axis_name(input_pad_axis axis);
b8          input_key_from_name(const char* name, input_key* out);
b8          input_mouse_button_from_name(const char* name, input_mouse_button* out);
b8          input_pad_button_from_name(const char* name, input_pad_button* out);
b8          input_pad_axis_from_name(const char* name, input_pad_axis* out);

// The key / mouse button that went down this frame even if the UI took it, or
// -1: for "press a key to bind it" in a dialog that has the keyboard.
int input_raw_key_pressed(void);
int input_raw_mouse_pressed(void);

// --- Cursor ----------------------------------------------------------------------

// Locked hides the cursor and keeps it in the window, for mouse-look; it uses
// raw (unaccelerated) motion where the system has it. Go through this rather
// than the window, so the jump the cursor makes on a mode change isn't read as
// movement. Mouse-motion action bindings only read while it's locked. `wind`
// NULL changes only what Input believes (tests).
void               input_set_cursor_mode(window* wind, window_cursor_mode mode);
window_cursor_mode input_cursor_mode(void);

// --- Feeding (App.c; tests) ------------------------------------------------------

// Before each frame's events are polled: clears last frame's pressed/released
// and its mouse and scroll movement.
void input_begin_frame(void);

void input_on_key(int key, b8 down);
void input_on_mouse_button(int button, b8 down);
void input_on_raw_press(int key, int mouse_button);   // before the UI's say; -1 for neither
void input_on_cursor(f64 x, f64 y, b8 counts_as_movement);   // false: moved over the UI
void input_on_scroll(f64 dx, f64 dy);
void input_on_focus(b8 focused);   // losing it releases every key and button, and pads read as at rest

// After the frame's events are polled: reads every pad from GLFW into input_on_pad().
void input_poll_pads(void);

// One pad's raw state, as GLFW reports it: triggers -1 at rest .. 1.
typedef struct input_pad_raw
{
    const char* name;
    b8          buttons[INPUT_PAD_BUTTON_COUNT];
    f32         axes[INPUT_PAD_AXIS_COUNT];
} input_pad_raw;

// NULL: not connected. Connecting and disconnecting are logged; a pad that
// disconnects releases its buttons.
void input_on_pad(int pad, const input_pad_raw* raw);

// Everything up, no movement, cursor mode back to normal (without touching a window).
void input_reset(void);
