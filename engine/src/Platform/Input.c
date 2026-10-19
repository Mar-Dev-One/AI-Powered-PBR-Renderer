#include "Input.h"

#include "GLFW/glfw3.h"

#include <math.h>
#include <string.h>

// input_key is GLFW's numbering; spot-check both ends of each range.
STATIC_ASSERT(INPUT_KEY_SPACE == GLFW_KEY_SPACE && INPUT_KEY_9 == GLFW_KEY_9 && INPUT_KEY_Z == GLFW_KEY_Z, "input_key != GLFW");
STATIC_ASSERT(INPUT_KEY_GRAVE_ACCENT == GLFW_KEY_GRAVE_ACCENT && INPUT_KEY_END == GLFW_KEY_END, "input_key != GLFW");
STATIC_ASSERT(INPUT_KEY_F12 == GLFW_KEY_F12 && INPUT_KEY_KP_9 == GLFW_KEY_KP_9, "input_key != GLFW");
STATIC_ASSERT(INPUT_KEY_RIGHT_SUPER == GLFW_KEY_RIGHT_SUPER && INPUT_KEY_COUNT == GLFW_KEY_LAST + 1, "input_key != GLFW");
STATIC_ASSERT(INPUT_MOUSE_MIDDLE == GLFW_MOUSE_BUTTON_MIDDLE && INPUT_MOUSE_BUTTON_COUNT == GLFW_MOUSE_BUTTON_LAST + 1, "input_mouse_button != GLFW");
STATIC_ASSERT(INPUT_PAD_GUIDE == GLFW_GAMEPAD_BUTTON_GUIDE && INPUT_PAD_BUTTON_COUNT == GLFW_GAMEPAD_BUTTON_LAST + 1, "input_pad_button != GLFW");
STATIC_ASSERT(INPUT_PAD_RIGHT_Y == GLFW_GAMEPAD_AXIS_RIGHT_Y && INPUT_PAD_AXIS_COUNT == GLFW_GAMEPAD_AXIS_LAST + 1, "input_pad_axis != GLFW");
STATIC_ASSERT(INPUT_MAX_PADS == GLFW_JOYSTICK_LAST + 1, "INPUT_MAX_PADS != GLFW");

#define DEFAULT_STICK_DEAD_ZONE 0.2f
#define TRIGGER_DEAD_ZONE       0.05f

enum
{
    STATE_DOWN     = 1 << 0,
    STATE_PRESSED  = 1 << 1,
    STATE_RELEASED = 1 << 2,
};

typedef struct pad_state
{
    b8    connected;
    b8    no_mapping_logged;   // a joystick GLFW has no gamepad mapping for: said once, then ignored
    char  name[64];
    uint8 buttons[INPUT_PAD_BUTTON_COUNT];
    f32   axes[INPUT_PAD_AXIS_COUNT];   // past the dead zones
} pad_state;

static struct
{
    uint8 keys[INPUT_KEY_COUNT];
    uint8 buttons[INPUT_MOUSE_BUTTON_COUNT];

    f64 cursor_x, cursor_y;
    b8  has_cursor;        // false until a position arrives (or after a mode change): the next one sets it without moving
    f32 delta_x, delta_y;
    f32 scroll_x, scroll_y;

    window_cursor_mode cursor_mode;
    b8                 unfocused;

    pad_state pads[INPUT_MAX_PADS];
    f32       stick_dead_zone;
} input = { .stick_dead_zone = DEFAULT_STICK_DEAD_ZONE };

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

static const pad_state*
pad_at(int pad)
{
    return pad >= 0 && pad < INPUT_MAX_PADS && input.pads[pad].connected ? &input.pads[pad] : NULL;
}

static uint8
pad_button_state(int pad, input_pad_button button)
{
    if ((int)button < 0 || button >= INPUT_PAD_BUTTON_COUNT)
        return 0;

    if (pad != INPUT_PAD_ANY)
    {
        const pad_state* p = pad_at(pad);
        return p ? p->buttons[button] : 0;
    }

    uint8 any = 0;
    for (int i = 0; i < INPUT_MAX_PADS; ++i)
        if (input.pads[i].connected)
            any |= input.pads[i].buttons[button];
    return any;
}

b8
input_pad_connected(int pad)
{
    if (pad != INPUT_PAD_ANY)
        return pad_at(pad) != NULL;

    for (int i = 0; i < INPUT_MAX_PADS; ++i)
        if (input.pads[i].connected)
            return true;
    return false;
}

const char*
input_pad_name(int pad)
{
    const pad_state* p = pad_at(pad);
    return p ? p->name : "";
}

b8 input_pad_down(int pad, input_pad_button button)     { return (pad_button_state(pad, button) & STATE_DOWN) != 0; }
b8 input_pad_pressed(int pad, input_pad_button button)  { return (pad_button_state(pad, button) & STATE_PRESSED) != 0; }
b8 input_pad_released(int pad, input_pad_button button) { return (pad_button_state(pad, button) & STATE_RELEASED) != 0; }

f32
input_pad_axis_value(int pad, input_pad_axis axis)
{
    if ((int)axis < 0 || axis >= INPUT_PAD_AXIS_COUNT)
        return 0.0f;

    if (pad != INPUT_PAD_ANY)
    {
        const pad_state* p = pad_at(pad);
        return p ? p->axes[axis] : 0.0f;
    }

    f32 furthest = 0.0f;
    for (int i = 0; i < INPUT_MAX_PADS; ++i)
        if (input.pads[i].connected && fabsf(input.pads[i].axes[axis]) > fabsf(furthest))
            furthest = input.pads[i].axes[axis];
    return furthest;
}

void
input_set_stick_dead_zone(f32 dead_zone)
{
    input.stick_dead_zone = fminf(fmaxf(dead_zone, 0.0f), 0.9f);
}

// Round, so a diagonal isn't cut more than a straight push, and rescaled so
// the stick still moves smoothly out of it.
static void
apply_stick_dead_zone(f32 x, f32 y, f32* out_x, f32* out_y)
{
    f32 length = sqrtf(x * x + y * y);
    if (length <= input.stick_dead_zone)
    {
        *out_x = *out_y = 0.0f;
        return;
    }

    f32 scale = fminf((length - input.stick_dead_zone) / (1.0f - input.stick_dead_zone), 1.0f) / length;
    *out_x = x * scale;
    *out_y = y * scale;
}

static f32
apply_trigger_dead_zone(f32 raw)
{
    f32 pulled = (raw + 1.0f) * 0.5f;   // GLFW: -1 at rest
    return pulled <= TRIGGER_DEAD_ZONE ? 0.0f : fminf((pulled - TRIGGER_DEAD_ZONE) / (1.0f - TRIGGER_DEAD_ZONE), 1.0f);
}

void
input_on_pad(int pad, const input_pad_raw* raw)
{
    if (pad < 0 || pad >= INPUT_MAX_PADS)
        return;
    pad_state* p = &input.pads[pad];

    if (!raw)
    {
        if (p->connected)
            LOG_INFO("input: gamepad %d disconnected (%s)\n", pad, p->name);
        for (int i = 0; i < INPUT_PAD_BUTTON_COUNT; ++i)
            set_state(&p->buttons[i], false);
        memset(p->axes, 0, sizeof(p->axes));
        p->connected = false;
        return;
    }

    if (!p->connected)
    {
        snprintf(p->name, sizeof(p->name), "%s", raw->name ? raw->name : "Gamepad");
        LOG_INFO("input: gamepad %d connected (%s)\n", pad, p->name);
        p->connected = true;
    }

    // In the background the pad still reads, but the game shouldn't move.
    for (int i = 0; i < INPUT_PAD_BUTTON_COUNT; ++i)
        set_state(&p->buttons[i], raw->buttons[i] && !input.unfocused);

    if (input.unfocused)
    {
        memset(p->axes, 0, sizeof(p->axes));
        return;
    }
    apply_stick_dead_zone(raw->axes[INPUT_PAD_LEFT_X], raw->axes[INPUT_PAD_LEFT_Y],
                          &p->axes[INPUT_PAD_LEFT_X], &p->axes[INPUT_PAD_LEFT_Y]);
    apply_stick_dead_zone(raw->axes[INPUT_PAD_RIGHT_X], raw->axes[INPUT_PAD_RIGHT_Y],
                          &p->axes[INPUT_PAD_RIGHT_X], &p->axes[INPUT_PAD_RIGHT_Y]);
    p->axes[INPUT_PAD_LEFT_TRIGGER]  = apply_trigger_dead_zone(raw->axes[INPUT_PAD_LEFT_TRIGGER]);
    p->axes[INPUT_PAD_RIGHT_TRIGGER] = apply_trigger_dead_zone(raw->axes[INPUT_PAD_RIGHT_TRIGGER]);
}

void
input_poll_pads(void)
{
    for (int jid = 0; jid < INPUT_MAX_PADS; ++jid)
    {
        // GLFW only polls joysticks it has seen connect, so the empty slots are cheap.
        GLFWgamepadstate state;
        if (glfwJoystickIsGamepad(jid) && glfwGetGamepadState(jid, &state))
        {
            input_pad_raw raw = { .name = glfwGetGamepadName(jid) };
            for (int i = 0; i < INPUT_PAD_BUTTON_COUNT; ++i)
                raw.buttons[i] = state.buttons[i] == GLFW_PRESS;
            memcpy(raw.axes, state.axes, sizeof(raw.axes));
            input_on_pad(jid, &raw);
            continue;
        }

        input_on_pad(jid, NULL);

        pad_state* p = &input.pads[jid];
        if (!glfwJoystickPresent(jid))
            p->no_mapping_logged = false;
        else if (!p->no_mapping_logged)
        {
            const char* name = glfwGetJoystickName(jid);
            LOG_WARN("input: joystick %d (%s) isn't a gamepad GLFW knows, so it's ignored\n", jid, name ? name : "no name");
            p->no_mapping_logged = true;
        }
    }
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
    for (int p = 0; p < INPUT_MAX_PADS; ++p)
        for (int i = 0; i < INPUT_PAD_BUTTON_COUNT; ++i)
            input.pads[p].buttons[i] &= STATE_DOWN;

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
    input.unfocused = !focused;
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
    input.stick_dead_zone = DEFAULT_STICK_DEAD_ZONE;
}
