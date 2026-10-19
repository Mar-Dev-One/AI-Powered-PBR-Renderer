#include "Input.h"

#include "GLFW/glfw3.h"

#include <ctype.h>
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

    int raw_key_pressed;     // this frame, whatever the UI said; -1 none
    int raw_mouse_pressed;
} input = { .stick_dead_zone = DEFAULT_STICK_DEAD_ZONE, .raw_key_pressed = -1, .raw_mouse_pressed = -1 };

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

    if (wind)
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
    input.raw_key_pressed = input.raw_mouse_pressed = -1;
}

void
input_on_raw_press(int key, int mouse_button)
{
    if (key >= 0 && key < INPUT_KEY_COUNT)
        input.raw_key_pressed = key;
    if (mouse_button >= 0 && mouse_button < INPUT_MOUSE_BUTTON_COUNT)
        input.raw_mouse_pressed = mouse_button;
}

int input_raw_key_pressed(void)   { return input.raw_key_pressed; }
int input_raw_mouse_pressed(void) { return input.raw_mouse_pressed; }

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
    input.raw_key_pressed = input.raw_mouse_pressed = -1;
}

// --- Names -----------------------------------------------------------------------

typedef struct code_name
{
    int         code;
    const char* name;
} code_name;

// Letters, digits, F-keys and the keypad's digits are made up as they're asked for.
static const code_name KEY_NAMES[] = {
    { INPUT_KEY_SPACE, "space" }, { INPUT_KEY_APOSTROPHE, "apostrophe" }, { INPUT_KEY_COMMA, "comma" },
    { INPUT_KEY_MINUS, "minus" }, { INPUT_KEY_PERIOD, "period" }, { INPUT_KEY_SLASH, "slash" },
    { INPUT_KEY_SEMICOLON, "semicolon" }, { INPUT_KEY_EQUAL, "equal" },
    { INPUT_KEY_LEFT_BRACKET, "left_bracket" }, { INPUT_KEY_BACKSLASH, "backslash" },
    { INPUT_KEY_RIGHT_BRACKET, "right_bracket" }, { INPUT_KEY_GRAVE_ACCENT, "grave_accent" },
    { INPUT_KEY_ESCAPE, "escape" }, { INPUT_KEY_ENTER, "enter" }, { INPUT_KEY_TAB, "tab" },
    { INPUT_KEY_BACKSPACE, "backspace" }, { INPUT_KEY_INSERT, "insert" }, { INPUT_KEY_DELETE, "delete" },
    { INPUT_KEY_RIGHT, "right" }, { INPUT_KEY_LEFT, "left" }, { INPUT_KEY_DOWN, "down" }, { INPUT_KEY_UP, "up" },
    { INPUT_KEY_PAGE_UP, "page_up" }, { INPUT_KEY_PAGE_DOWN, "page_down" }, { INPUT_KEY_HOME, "home" },
    { INPUT_KEY_END, "end" }, { INPUT_KEY_CAPS_LOCK, "caps_lock" },
    { INPUT_KEY_LEFT_SHIFT, "left_shift" }, { INPUT_KEY_LEFT_CONTROL, "left_control" },
    { INPUT_KEY_LEFT_ALT, "left_alt" }, { INPUT_KEY_LEFT_SUPER, "left_super" },
    { INPUT_KEY_RIGHT_SHIFT, "right_shift" }, { INPUT_KEY_RIGHT_CONTROL, "right_control" },
    { INPUT_KEY_RIGHT_ALT, "right_alt" }, { INPUT_KEY_RIGHT_SUPER, "right_super" }, { INPUT_KEY_MENU, "menu" },
};

static const char* const MOUSE_BUTTON_NAMES[INPUT_MOUSE_BUTTON_COUNT] = {
    "left", "right", "middle", "button4", "button5", "button6", "button7", "button8",
};

static const char* const PAD_BUTTON_NAMES[INPUT_PAD_BUTTON_COUNT] = {
    "a", "b", "x", "y", "left_bumper", "right_bumper", "back", "start", "guide",
    "left_thumb", "right_thumb", "dpad_up", "dpad_right", "dpad_down", "dpad_left",
};

static const char* const PAD_AXIS_NAMES[INPUT_PAD_AXIS_COUNT] = {
    "left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger",
};

static b8
same_name(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

const char*
input_key_name(input_key key)
{
    // Each made-up name has its own slot, so what's returned stays valid.
    static char made[INPUT_KEY_COUNT][6];

    if ((key >= INPUT_KEY_A && key <= INPUT_KEY_Z) || (key >= INPUT_KEY_0 && key <= INPUT_KEY_9))
    {
        made[key][0] = (char)tolower(key);
        return made[key];
    }
    if (key >= INPUT_KEY_F1 && key <= INPUT_KEY_F12)
    {
        snprintf(made[key], sizeof(made[key]), "f%d", key - INPUT_KEY_F1 + 1);
        return made[key];
    }
    if (key >= INPUT_KEY_KP_0 && key <= INPUT_KEY_KP_9)
    {
        snprintf(made[key], sizeof(made[key]), "kp_%d", key - INPUT_KEY_KP_0);
        return made[key];
    }
    for (size_t i = 0; i < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); ++i)
        if (KEY_NAMES[i].code == (int)key)
            return KEY_NAMES[i].name;
    return "";
}

b8
input_key_from_name(const char* name, input_key* out)
{
    if (!name || !name[0])
        return false;
    for (int key = 0; key < INPUT_KEY_COUNT; ++key)
    {
        const char* known = input_key_name((input_key)key);
        if (known[0] && same_name(known, name))
        {
            *out = (input_key)key;
            return true;
        }
    }
    return false;
}

static b8
index_from_name(const char* const* names, int count, const char* name, int* out)
{
    for (int i = 0; name && i < count; ++i)
        if (same_name(names[i], name))
        {
            *out = i;
            return true;
        }
    return false;
}

const char*
input_mouse_button_name(input_mouse_button button)
{
    return (int)button >= 0 && button < INPUT_MOUSE_BUTTON_COUNT ? MOUSE_BUTTON_NAMES[button] : "";
}

const char*
input_pad_button_name(input_pad_button button)
{
    return (int)button >= 0 && button < INPUT_PAD_BUTTON_COUNT ? PAD_BUTTON_NAMES[button] : "";
}

const char*
input_pad_axis_name(input_pad_axis axis)
{
    return (int)axis >= 0 && axis < INPUT_PAD_AXIS_COUNT ? PAD_AXIS_NAMES[axis] : "";
}

b8
input_mouse_button_from_name(const char* name, input_mouse_button* out)
{
    int i;
    if (!index_from_name(MOUSE_BUTTON_NAMES, INPUT_MOUSE_BUTTON_COUNT, name, &i))
        return false;
    *out = (input_mouse_button)i;
    return true;
}

b8
input_pad_button_from_name(const char* name, input_pad_button* out)
{
    int i;
    if (!index_from_name(PAD_BUTTON_NAMES, INPUT_PAD_BUTTON_COUNT, name, &i))
        return false;
    *out = (input_pad_button)i;
    return true;
}

b8
input_pad_axis_from_name(const char* name, input_pad_axis* out)
{
    int i;
    if (!index_from_name(PAD_AXIS_NAMES, INPUT_PAD_AXIS_COUNT, name, &i))
        return false;
    *out = (input_pad_axis)i;
    return true;
}
