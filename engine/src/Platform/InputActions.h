#pragma once

#include "../Core/Defines.h"
#include "Input.h"

// Named actions over Platform/Input.h, so gameplay code reads "jump" or
// "move_x" and never a key: each action has a few bindings (keys, mouse,
// pad), and any of them drives it.
//
//   button  down / pressed / released, like a key: down while any binding is
//           held (an axis binding counts from halfway). Pressed and released
//           follow the action as a whole, so a second binding going down while
//           the first is held is no new press.
//   axis    a value, the binding pushed furthest:
//             state  -1..1, where it's held now (move_x, move_y)
//             delta  how much this frame (look_x, look_y): mouse movement
//                    times its scale, plus key and stick rates times the
//                    frame time, so a camera adds it as it is
//
// App.c recomputes every action once a frame (input_actions_update(), after
// the pads are polled), and starts with input_actions_set_defaults().
// Pad bindings read INPUT_PAD_ANY: one player, any pad.

#define INPUT_ACTION_MAX           64
#define INPUT_ACTION_MAX_BINDINGS  8
#define INPUT_ACTION_NAME_MAX      32

// A handle from input_action_find() / input_action_add(); INPUT_ACTION_NONE
// (an unknown name) reads as never down and 0, so a missing action is harmless.
typedef int input_action;
#define INPUT_ACTION_NONE (-1)

typedef enum input_action_kind
{
    INPUT_ACTION_BUTTON,
    INPUT_ACTION_AXIS,         // state
    INPUT_ACTION_AXIS_DELTA,
} input_action_kind;

typedef enum input_binding_kind
{
    INPUT_BINDING_KEY,            // code: input_key
    INPUT_BINDING_MOUSE_BUTTON,   // code: input_mouse_button
    INPUT_BINDING_PAD_BUTTON,     // code: input_pad_button
    INPUT_BINDING_KEY_PAIR,       // code: the key for -1, code2: for +1
    INPUT_BINDING_PAD_AXIS,       // code: input_pad_axis
    INPUT_BINDING_MOUSE_MOTION,   // code: 0 = x, 1 = y; only for delta axes
} input_binding_kind;

// `scale` multiplies what the binding reads: -1 flips it. On a delta axis it's
// the rate: units per pixel for the mouse, per second for keys and sticks.
typedef struct input_binding
{
    input_binding_kind kind;
    int                code;
    int                code2;
    f32                scale;
} input_binding;

input_binding input_binding_key(input_key key);
input_binding input_binding_mouse_button(input_mouse_button button);
input_binding input_binding_pad_button(input_pad_button button);
input_binding input_binding_key_pair(input_key negative, input_key positive, f32 scale);
input_binding input_binding_pad_axis(input_pad_axis axis, f32 scale);
input_binding input_binding_mouse_motion(int axis, f32 scale);

// --- Reading (from App.on_frame) -------------------------------------------------

input_action input_action_find(const char* name);

b8  input_action_down(input_action action);
b8  input_action_pressed(input_action action);
b8  input_action_released(input_action action);
f32 input_action_value(input_action action);   // a button reads 0 or 1

// --- Setting up ------------------------------------------------------------------

// Adds an action, or returns the one already called `name` (which keeps its
// bindings, takes the new kind). NONE if the name is empty or too long, or
// all INPUT_ACTION_MAX are used.
input_action input_action_add(const char* name, input_action_kind kind);

// False (and nothing added) for NONE, an action with all its bindings used,
// or a binding that doesn't fit the action (mouse motion on anything but a
// delta axis).
b8   input_action_bind(input_action action, input_binding binding);
void input_action_unbind_all(input_action action);

void input_actions_clear(void);

// Walking, flying and looking around, for keyboard and mouse and a pad:
//   move_x  A/D, left stick           move_y  S/W, left stick (+1 forward)
//   look_x  mouse, right stick        look_y  mouse, right stick (+ down),
//           degrees: 0.1 per pixel, 150 per second
//   up_down Q/E, bumpers (+ up)       jump    Space, A
//   sprint  Shift, left stick click
// Replaces every action there was.
void input_actions_set_defaults(void);

int                  input_action_count(void);
const char*          input_action_name(input_action action);         // "" for NONE
input_action_kind    input_action_get_kind(input_action action);
const input_binding* input_action_bindings(input_action action, int* out_count);

// --- Each frame (App.c; tests) ---------------------------------------------------

// After Input has this frame's events and pads. dt in seconds, capped at 0.1
// so a stall doesn't spin the camera.
void input_actions_update(f32 dt);
