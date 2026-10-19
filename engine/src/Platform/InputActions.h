#pragma once

#include "../Core/Defines.h"
#include "Input.h"

#include "cJSON.h"

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
//                    times its scale (only while the cursor is locked: a
//                    free one is pointing, not looking), plus key and stick
//                    rates times the frame time, so a camera adds it as it is
//
// The actions are an input_action_map: plain data a project keeps
// (Project/Project.h, project.json's "input") and loads into the live table
// with input_actions_load(). App.c loads the defaults before App.on_init and
// recomputes every action once a frame (input_actions_update(), after the
// pads are polled). Pad bindings read INPUT_PAD_ANY: one player, any pad.

#define INPUT_ACTION_MAX           64
#define INPUT_ACTION_MAX_BINDINGS  8
#define INPUT_ACTION_NAME_MAX      32   // letters, digits and '_', 1-31 of them
#define INPUT_BINDING_TEXT_MAX     48

// A handle from input_action_find(); INPUT_ACTION_NONE (an unknown name)
// reads as never down and 0, so a missing action is harmless.
typedef int input_action;
#define INPUT_ACTION_NONE (-1)

typedef enum input_action_kind
{
    INPUT_ACTION_BUTTON,
    INPUT_ACTION_AXIS,         // state
    INPUT_ACTION_AXIS_DELTA,
    INPUT_ACTION_KIND_COUNT
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

// As text, the way project.json, the editor and the agent write it:
//   key:w   keys:a/d   mouse:right   pad:a   pad_axis:left_y   mouse_motion:x
// with "*<scale>" after it when the scale isn't 1 ("pad_axis:left_y*-1").
// Names are Platform/Input.h's (input_key_name() etc.); case doesn't matter.
void input_binding_to_text(const input_binding* binding, char* out, size_t size);
b8   input_binding_from_text(const char* text, input_binding* out, char* error, size_t error_size);

// "button", "axis", "delta".
const char* input_action_kind_name(input_action_kind kind);
b8          input_action_kind_from_name(const char* name, input_action_kind* out);

// --- Action maps ------------------------------------------------------------------

typedef struct input_action_def
{
    char              name[INPUT_ACTION_NAME_MAX];
    input_action_kind kind;
    input_binding     bindings[INPUT_ACTION_MAX_BINDINGS];
    int               binding_count;
} input_action_def;

typedef struct input_action_map
{
    b8               custom;            // false: the defaults, which a project file doesn't store
    f32              stick_dead_zone;   // Input.h input_set_stick_dead_zone()
    int              count;
    input_action_def actions[INPUT_ACTION_MAX];
} input_action_map;

// Walking, flying and looking around, for keyboard and mouse and a pad:
//   move_x  A/D, left stick           move_y  S/W, left stick (+1 forward)
//   look_x  mouse, right stick        look_y  mouse, right stick (+ down),
//           degrees: 0.1 per pixel, 150 per second
//   up_down Q/E, bumpers (+ up)       jump    Space, A
//   sprint  Shift, left stick click
// Dead zone 0.2.
void input_action_map_defaults(input_action_map* out);

b8 input_action_name_valid(const char* name);

// The action called `name`, or -1.
int input_action_map_find(const input_action_map* map, const char* name);

// Adds an action, or returns the one already called `name` (which keeps its
// bindings, takes the new kind). -1 if the name isn't valid or all
// INPUT_ACTION_MAX are used.
int input_action_map_add(input_action_map* map, const char* name, input_action_kind kind);

// False (and nothing added) for an index out of range, an action with all its
// bindings used, or a binding that doesn't fit it (mouse motion on anything
// but a delta axis); `error` (may be NULL) says which.
b8 input_action_map_bind(input_action_map* map, int index, input_binding binding, char* error, size_t error_size);

void input_action_map_remove(input_action_map* map, int index);

// project.json's "input":
//   { "stick_dead_zone": 0.2,
//     "actions": { "jump": { "type": "button", "bindings": ["key:space", "pad:a"] }, ... } }
// Reading a missing section (NULL) gives the defaults; a present one gives
// exactly its actions, `custom` set. False, with why in `error`, on a bad
// value (nothing in `out` is touched then).
cJSON* input_action_map_to_json(const input_action_map* map);
b8     input_action_map_from_json(const cJSON* json, input_action_map* out, char* error, size_t error_size);

// --- The live actions ---------------------------------------------------------------

// Replaces every action (and sets the stick dead zone). Handles from before
// may now name another action or none: find them again.
void input_actions_load(const input_action_map* map);
void input_actions_set_defaults(void);
void input_actions_clear(void);

input_action input_action_find(const char* name);

b8  input_action_down(input_action action);
b8  input_action_pressed(input_action action);
b8  input_action_released(input_action action);
f32 input_action_value(input_action action);   // a button reads 0 or 1

int         input_action_count(void);
const char* input_action_name(input_action action);   // "" for NONE

// --- Each frame (App.c; tests) ---------------------------------------------------

// After Input has this frame's events and pads. dt in seconds, capped at 0.1
// so a stall doesn't spin the camera.
void input_actions_update(f32 dt);
