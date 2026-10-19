#include "InputActions.h"

#include <math.h>
#include <string.h>

#define MAX_DT              0.1f
#define AXIS_AS_BUTTON_AT   0.5f   // an axis bound to a button action holds it from here

typedef struct action_slot
{
    char              name[INPUT_ACTION_NAME_MAX];
    input_action_kind kind;
    input_binding     bindings[INPUT_ACTION_MAX_BINDINGS];
    int               binding_count;

    // input_actions_update()
    b8  down, pressed, released;
    f32 value;
} action_slot;

static struct
{
    action_slot actions[INPUT_ACTION_MAX];
    int         count;
} actions;

static action_slot*
slot_at(input_action action)
{
    return action >= 0 && action < actions.count ? &actions.actions[action] : NULL;
}

// --- Bindings -------------------------------------------------------------------

input_binding input_binding_key(input_key key)                    { return (input_binding){ INPUT_BINDING_KEY, key, 0, 1.0f }; }
input_binding input_binding_mouse_button(input_mouse_button button) { return (input_binding){ INPUT_BINDING_MOUSE_BUTTON, button, 0, 1.0f }; }
input_binding input_binding_pad_button(input_pad_button button)   { return (input_binding){ INPUT_BINDING_PAD_BUTTON, button, 0, 1.0f }; }
input_binding input_binding_pad_axis(input_pad_axis axis, f32 scale) { return (input_binding){ INPUT_BINDING_PAD_AXIS, axis, 0, scale }; }
input_binding input_binding_mouse_motion(int axis, f32 scale)     { return (input_binding){ INPUT_BINDING_MOUSE_MOTION, axis, 0, scale }; }

input_binding
input_binding_key_pair(input_key negative, input_key positive, f32 scale)
{
    return (input_binding){ INPUT_BINDING_KEY_PAIR, negative, positive, scale };
}

// Held now, -1..1 before its scale (mouse motion: none, it has no "held").
static f32
binding_held(const input_binding* b)
{
    switch (b->kind)
    {
        case INPUT_BINDING_KEY:          return input_key_down((input_key)b->code) ? 1.0f : 0.0f;
        case INPUT_BINDING_MOUSE_BUTTON: return input_mouse_down((input_mouse_button)b->code) ? 1.0f : 0.0f;
        case INPUT_BINDING_PAD_BUTTON:   return input_pad_down(INPUT_PAD_ANY, (input_pad_button)b->code) ? 1.0f : 0.0f;
        case INPUT_BINDING_KEY_PAIR:
            return (input_key_down((input_key)b->code2) ? 1.0f : 0.0f) - (input_key_down((input_key)b->code) ? 1.0f : 0.0f);
        case INPUT_BINDING_PAD_AXIS:     return input_pad_axis_value(INPUT_PAD_ANY, (input_pad_axis)b->code);
        case INPUT_BINDING_MOUSE_MOTION: return 0.0f;
    }
    return 0.0f;
}

// Went down this frame, even if it's up again already (a tap shorter than a frame).
static b8
binding_pressed(const input_binding* b)
{
    switch (b->kind)
    {
        case INPUT_BINDING_KEY:          return input_key_pressed((input_key)b->code);
        case INPUT_BINDING_MOUSE_BUTTON: return input_mouse_pressed((input_mouse_button)b->code);
        case INPUT_BINDING_PAD_BUTTON:   return input_pad_pressed(INPUT_PAD_ANY, (input_pad_button)b->code);
        case INPUT_BINDING_KEY_PAIR:
            return input_key_pressed((input_key)b->code) || input_key_pressed((input_key)b->code2);
        default:                         return false;
    }
}

static f32
mouse_motion(const input_binding* b)
{
    f32 dx, dy;
    input_mouse_delta(&dx, &dy);
    return (b->code == 0 ? dx : dy) * b->scale;
}

// --- Reading --------------------------------------------------------------------

input_action
input_action_find(const char* name)
{
    if (!name)
        return INPUT_ACTION_NONE;

    for (int i = 0; i < actions.count; ++i)
        if (strcmp(actions.actions[i].name, name) == 0)
            return i;
    return INPUT_ACTION_NONE;
}

b8 input_action_down(input_action action)     { const action_slot* a = slot_at(action); return a && a->down; }
b8 input_action_pressed(input_action action)  { const action_slot* a = slot_at(action); return a && a->pressed; }
b8 input_action_released(input_action action) { const action_slot* a = slot_at(action); return a && a->released; }
f32 input_action_value(input_action action)   { const action_slot* a = slot_at(action); return a ? a->value : 0.0f; }

// --- Setting up -----------------------------------------------------------------

input_action
input_action_add(const char* name, input_action_kind kind)
{
    if (!name || !name[0] || strlen(name) >= INPUT_ACTION_NAME_MAX)
        return INPUT_ACTION_NONE;

    input_action existing = input_action_find(name);
    if (existing != INPUT_ACTION_NONE)
    {
        actions.actions[existing].kind = kind;
        return existing;
    }

    if (actions.count == INPUT_ACTION_MAX)
        return INPUT_ACTION_NONE;

    action_slot* a = &actions.actions[actions.count];
    memset(a, 0, sizeof(*a));
    strcpy(a->name, name);
    a->kind = kind;
    return actions.count++;
}

b8
input_action_bind(input_action action, input_binding binding)
{
    action_slot* a = slot_at(action);
    if (!a || a->binding_count == INPUT_ACTION_MAX_BINDINGS)
        return false;
    if (binding.kind == INPUT_BINDING_MOUSE_MOTION && a->kind != INPUT_ACTION_AXIS_DELTA)
        return false;

    a->bindings[a->binding_count++] = binding;
    return true;
}

void
input_action_unbind_all(input_action action)
{
    action_slot* a = slot_at(action);
    if (a)
        a->binding_count = 0;
}

void
input_actions_clear(void)
{
    memset(&actions, 0, sizeof(actions));
}

void
input_actions_set_defaults(void)
{
    input_actions_clear();

    const f32 look_degrees_per_pixel  = 0.1f;
    const f32 look_degrees_per_second = 150.0f;

    input_action a = input_action_add("move_x", INPUT_ACTION_AXIS);
    input_action_bind(a, input_binding_key_pair(INPUT_KEY_A, INPUT_KEY_D, 1.0f));
    input_action_bind(a, input_binding_pad_axis(INPUT_PAD_LEFT_X, 1.0f));

    a = input_action_add("move_y", INPUT_ACTION_AXIS);
    input_action_bind(a, input_binding_key_pair(INPUT_KEY_S, INPUT_KEY_W, 1.0f));
    input_action_bind(a, input_binding_pad_axis(INPUT_PAD_LEFT_Y, -1.0f));   // the stick is + down

    a = input_action_add("look_x", INPUT_ACTION_AXIS_DELTA);
    input_action_bind(a, input_binding_mouse_motion(0, look_degrees_per_pixel));
    input_action_bind(a, input_binding_pad_axis(INPUT_PAD_RIGHT_X, look_degrees_per_second));

    a = input_action_add("look_y", INPUT_ACTION_AXIS_DELTA);
    input_action_bind(a, input_binding_mouse_motion(1, look_degrees_per_pixel));
    input_action_bind(a, input_binding_pad_axis(INPUT_PAD_RIGHT_Y, look_degrees_per_second));

    a = input_action_add("up_down", INPUT_ACTION_AXIS);
    input_action_bind(a, input_binding_key_pair(INPUT_KEY_Q, INPUT_KEY_E, 1.0f));
    input_action_bind(a, input_binding_pad_button(INPUT_PAD_RIGHT_BUMPER));
    {
        input_binding down = input_binding_pad_button(INPUT_PAD_LEFT_BUMPER);
        down.scale = -1.0f;
        input_action_bind(a, down);
    }

    a = input_action_add("jump", INPUT_ACTION_BUTTON);
    input_action_bind(a, input_binding_key(INPUT_KEY_SPACE));
    input_action_bind(a, input_binding_pad_button(INPUT_PAD_A));

    a = input_action_add("sprint", INPUT_ACTION_BUTTON);
    input_action_bind(a, input_binding_key(INPUT_KEY_LEFT_SHIFT));
    input_action_bind(a, input_binding_pad_button(INPUT_PAD_LEFT_THUMB));
}

int
input_action_count(void)
{
    return actions.count;
}

const char*
input_action_name(input_action action)
{
    const action_slot* a = slot_at(action);
    return a ? a->name : "";
}

input_action_kind
input_action_get_kind(input_action action)
{
    const action_slot* a = slot_at(action);
    return a ? a->kind : INPUT_ACTION_BUTTON;
}

const input_binding*
input_action_bindings(input_action action, int* out_count)
{
    const action_slot* a = slot_at(action);
    *out_count = a ? a->binding_count : 0;
    return a ? a->bindings : NULL;
}

// --- Each frame -----------------------------------------------------------------

static void
update_button(action_slot* a)
{
    b8 was_down = a->down;
    b8 down     = false;
    b8 tapped   = false;

    for (int i = 0; i < a->binding_count; ++i)
    {
        const input_binding* b = &a->bindings[i];
        down   = down || fabsf(binding_held(b) * b->scale) >= AXIS_AS_BUTTON_AT;
        tapped = tapped || binding_pressed(b);
    }

    a->down     = down;
    a->pressed  = !was_down && (down || tapped);
    a->released = (was_down && !down) || (a->pressed && !down);
    a->value    = down ? 1.0f : 0.0f;
}

static void
update_axis(action_slot* a, f32 dt)
{
    f32 furthest = 0.0f;
    f32 mouse    = 0.0f;

    for (int i = 0; i < a->binding_count; ++i)
    {
        const input_binding* b = &a->bindings[i];
        if (b->kind == INPUT_BINDING_MOUSE_MOTION)
        {
            mouse += mouse_motion(b);
            continue;
        }

        f32 v = binding_held(b) * b->scale;
        if (fabsf(v) > fabsf(furthest))
            furthest = v;
    }

    if (a->kind == INPUT_ACTION_AXIS_DELTA)
        a->value = furthest * dt + mouse;
    else
        a->value = fminf(fmaxf(furthest, -1.0f), 1.0f);

    a->down     = a->value != 0.0f;
    a->pressed  = a->released = false;
}

void
input_actions_update(f32 dt)
{
    dt = fminf(fmaxf(dt, 0.0f), MAX_DT);

    for (int i = 0; i < actions.count; ++i)
    {
        action_slot* a = &actions.actions[i];
        if (a->kind == INPUT_ACTION_BUTTON)
            update_button(a);
        else
            update_axis(a, dt);
    }
}
