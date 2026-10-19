// input_tests.c
//
// Headless tests for Platform/Input.h and InputActions.h: events fed in by hand, as App.c's
// window callbacks do, then read back frame by frame.

#include "Platform/Input.h"
#include "Platform/InputActions.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        ++g_checks;                                                             \
        if (!(condition))                                                       \
        {                                                                       \
            ++g_failures;                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (0)

static void
test_key_edges(void)
{
    input_reset();

    // Frame 1: W goes down.
    input_begin_frame();
    input_on_key(INPUT_KEY_W, true);
    CHECK(input_key_down(INPUT_KEY_W) && input_key_pressed(INPUT_KEY_W) && !input_key_released(INPUT_KEY_W));
    CHECK(!input_key_down(INPUT_KEY_S));

    // Frame 2: still held, no longer "pressed". A second down (no GLFW repeat
    // reaches here, but a stray one) changes nothing.
    input_begin_frame();
    input_on_key(INPUT_KEY_W, true);
    CHECK(input_key_down(INPUT_KEY_W) && !input_key_pressed(INPUT_KEY_W));

    // Frame 3: up.
    input_begin_frame();
    input_on_key(INPUT_KEY_W, false);
    CHECK(!input_key_down(INPUT_KEY_W) && input_key_released(INPUT_KEY_W) && !input_key_pressed(INPUT_KEY_W));

    // Frame 4: nothing left over.
    input_begin_frame();
    CHECK(!input_key_down(INPUT_KEY_W) && !input_key_pressed(INPUT_KEY_W) && !input_key_released(INPUT_KEY_W));

    // A tap shorter than a frame is still seen.
    input_begin_frame();
    input_on_key(INPUT_KEY_SPACE, true);
    input_on_key(INPUT_KEY_SPACE, false);
    CHECK(input_key_pressed(INPUT_KEY_SPACE) && input_key_released(INPUT_KEY_SPACE) && !input_key_down(INPUT_KEY_SPACE));

    // A release with no press (the press went to the UI) is no edge at all.
    input_begin_frame();
    input_on_key(INPUT_KEY_E, false);
    CHECK(!input_key_released(INPUT_KEY_E));

    // Unknown and out-of-range codes are ignored, and read as up.
    input_on_key(-1, true);
    input_on_key(INPUT_KEY_COUNT, true);
    CHECK(!input_key_down((input_key)-1) && !input_key_down(INPUT_KEY_COUNT));
}

static void
test_mouse_buttons_and_focus(void)
{
    input_reset();

    input_begin_frame();
    input_on_mouse_button(INPUT_MOUSE_RIGHT, true);
    input_on_key(INPUT_KEY_LEFT_SHIFT, true);
    CHECK(input_mouse_down(INPUT_MOUSE_RIGHT) && input_mouse_pressed(INPUT_MOUSE_RIGHT));
    CHECK(!input_mouse_down(INPUT_MOUSE_LEFT));
    input_on_mouse_button(INPUT_MOUSE_BUTTON_COUNT, true);   // ignored
    CHECK(!input_mouse_down(INPUT_MOUSE_BUTTON_COUNT));

    // Alt-tab away: the releases never come, so focus loss stands in for them.
    input_begin_frame();
    input_on_focus(false);
    CHECK(!input_mouse_down(INPUT_MOUSE_RIGHT) && input_mouse_released(INPUT_MOUSE_RIGHT));
    CHECK(!input_key_down(INPUT_KEY_LEFT_SHIFT) && input_key_released(INPUT_KEY_LEFT_SHIFT));
    CHECK(!input_key_released(INPUT_KEY_A));   // only what was down comes up

    input_begin_frame();
    input_on_focus(true);
    CHECK(!input_mouse_released(INPUT_MOUSE_RIGHT));
}

static void
test_mouse_movement(void)
{
    input_reset();
    f32 dx, dy;

    // The first position only sets where the cursor is: no jump from 0,0.
    input_begin_frame();
    input_on_cursor(500.0, 300.0, true);
    input_mouse_delta(&dx, &dy);
    CHECK(dx == 0.0f && dy == 0.0f);

    // Movement adds up over a frame's events...
    input_begin_frame();
    input_on_cursor(510.0, 295.0, true);
    input_on_cursor(512.0, 290.0, true);
    input_mouse_delta(&dx, &dy);
    CHECK(dx == 12.0f && dy == -10.0f);
    f64 x, y;
    input_mouse_position(&x, &y);
    CHECK(x == 512.0 && y == 290.0);

    // ...and starts again next frame.
    input_begin_frame();
    input_mouse_delta(&dx, &dy);
    CHECK(dx == 0.0f && dy == 0.0f);

    // Movement over the UI keeps the position but adds no movement, so
    // leaving a panel isn't a jump either.
    input_on_cursor(700.0, 290.0, false);
    input_on_cursor(705.0, 290.0, true);
    input_mouse_delta(&dx, &dy);
    CHECK(dx == 5.0f && dy == 0.0f);

    // After focus comes back the cursor may be anywhere: the next position resyncs.
    input_begin_frame();
    input_on_focus(false);
    input_on_cursor(100.0, 100.0, true);
    input_mouse_delta(&dx, &dy);
    CHECK(dx == 0.0f && dy == 0.0f);

    // Wheel.
    input_begin_frame();
    input_on_scroll(0.0, 1.0);
    input_on_scroll(0.0, 2.0);
    input_scroll(&dx, &dy);
    CHECK(dx == 0.0f && dy == 3.0f);
    input_begin_frame();
    input_scroll(&dx, &dy);
    CHECK(dy == 0.0f);

    CHECK(input_cursor_mode() == WINDOW_CURSOR_NORMAL);
}

// A pad at rest, as GLFW reports it: triggers -1.
static input_pad_raw
pad_at_rest(void)
{
    input_pad_raw raw = { .name = "Test Pad" };
    raw.axes[INPUT_PAD_LEFT_TRIGGER] = raw.axes[INPUT_PAD_RIGHT_TRIGGER] = -1.0f;
    return raw;
}

static b8
near(f32 a, f32 b)
{
    return fabsf(a - b) < 1e-4f;
}

static void
test_pads(void)
{
    input_reset();

    CHECK(!input_pad_connected(0) && !input_pad_connected(INPUT_PAD_ANY));
    CHECK(!input_pad_down(0, INPUT_PAD_A) && input_pad_axis_value(0, INPUT_PAD_LEFT_X) == 0.0f);
    CHECK(input_pad_name(0)[0] == '\0');

    // Connect pad 1 with A held: pressed on the first frame, down after.
    input_pad_raw raw = pad_at_rest();
    raw.buttons[INPUT_PAD_A] = true;
    input_begin_frame();
    input_on_pad(1, &raw);
    CHECK(input_pad_connected(1) && input_pad_connected(INPUT_PAD_ANY) && !input_pad_connected(0));
    CHECK(strcmp(input_pad_name(1), "Test Pad") == 0);
    CHECK(input_pad_pressed(1, INPUT_PAD_A) && input_pad_pressed(INPUT_PAD_ANY, INPUT_PAD_A));
    CHECK(!input_pad_down(0, INPUT_PAD_A) && !input_pad_down(1, INPUT_PAD_B));

    input_begin_frame();
    input_on_pad(1, &raw);
    CHECK(input_pad_down(1, INPUT_PAD_A) && !input_pad_pressed(1, INPUT_PAD_A));

    // Triggers at rest read 0, fully pulled 1.
    CHECK(input_pad_axis_value(1, INPUT_PAD_LEFT_TRIGGER) == 0.0f);
    raw.axes[INPUT_PAD_RIGHT_TRIGGER] = 1.0f;
    input_on_pad(1, &raw);
    CHECK(near(input_pad_axis_value(1, INPUT_PAD_RIGHT_TRIGGER), 1.0f));

    // Sticks: drift inside the dead zone is nothing...
    raw.axes[INPUT_PAD_LEFT_X] = 0.1f;
    raw.axes[INPUT_PAD_LEFT_Y] = -0.1f;
    input_on_pad(1, &raw);
    CHECK(input_pad_axis_value(1, INPUT_PAD_LEFT_X) == 0.0f && input_pad_axis_value(1, INPUT_PAD_LEFT_Y) == 0.0f);

    // ...just past it is a little, not a jump to 0.2...
    raw.axes[INPUT_PAD_LEFT_X] = 0.3f;
    raw.axes[INPUT_PAD_LEFT_Y] = 0.0f;
    input_on_pad(1, &raw);
    CHECK(near(input_pad_axis_value(1, INPUT_PAD_LEFT_X), 0.125f));   // (0.3 - 0.2) / 0.8

    // ...a full push is 1, and a diagonal keeps its direction.
    raw.axes[INPUT_PAD_LEFT_X] = 1.0f;
    input_on_pad(1, &raw);
    CHECK(near(input_pad_axis_value(1, INPUT_PAD_LEFT_X), 1.0f));
    raw.axes[INPUT_PAD_LEFT_X] = raw.axes[INPUT_PAD_LEFT_Y] = 0.6f;
    input_on_pad(1, &raw);
    CHECK(near(input_pad_axis_value(1, INPUT_PAD_LEFT_X), input_pad_axis_value(1, INPUT_PAD_LEFT_Y)));
    CHECK(input_pad_axis_value(1, INPUT_PAD_LEFT_X) > 0.5f);

    // A wider dead zone swallows more.
    input_set_stick_dead_zone(0.5f);
    raw.axes[INPUT_PAD_LEFT_X] = 0.45f;
    raw.axes[INPUT_PAD_LEFT_Y] = 0.0f;
    input_on_pad(1, &raw);
    CHECK(input_pad_axis_value(1, INPUT_PAD_LEFT_X) == 0.0f);
    input_set_stick_dead_zone(0.2f);

    // Any pad: the one pushing furthest, either way.
    input_pad_raw other = pad_at_rest();
    other.axes[INPUT_PAD_RIGHT_X] = -0.9f;
    raw.axes[INPUT_PAD_RIGHT_X]   = 0.5f;
    input_on_pad(1, &raw);
    input_on_pad(3, &other);
    CHECK(input_pad_axis_value(INPUT_PAD_ANY, INPUT_PAD_RIGHT_X) == input_pad_axis_value(3, INPUT_PAD_RIGHT_X));
    CHECK(input_pad_axis_value(INPUT_PAD_ANY, INPUT_PAD_RIGHT_X) < 0.0f);

    // In the background the pad reads as let go, though still connected.
    input_begin_frame();
    input_on_focus(false);
    input_on_pad(1, &raw);
    CHECK(input_pad_connected(1) && !input_pad_down(1, INPUT_PAD_A) && input_pad_released(1, INPUT_PAD_A));
    CHECK(input_pad_axis_value(1, INPUT_PAD_RIGHT_TRIGGER) == 0.0f);
    input_begin_frame();
    input_on_focus(true);
    input_on_pad(1, &raw);
    CHECK(input_pad_pressed(1, INPUT_PAD_A));

    // Unplugged with A held: released, and gone.
    input_begin_frame();
    input_on_pad(1, NULL);
    CHECK(!input_pad_connected(1) && !input_pad_down(1, INPUT_PAD_A));
    CHECK(input_pad_connected(INPUT_PAD_ANY));   // pad 3 still is
    CHECK(input_pad_released(INPUT_PAD_ANY, INPUT_PAD_A) == false);   // pad 1 no longer counts towards "any"

    // Out-of-range pads and codes are ignored.
    input_on_pad(INPUT_MAX_PADS, &raw);
    CHECK(!input_pad_connected(INPUT_MAX_PADS) && !input_pad_down(3, INPUT_PAD_BUTTON_COUNT));
    CHECK(input_pad_axis_value(3, INPUT_PAD_AXIS_COUNT) == 0.0f);
}

// --- Actions --------------------------------------------------------------------

// One frame: the events run between begin and update, as in App.c.
#define FRAME(dt, events)          \
    do                             \
    {                              \
        input_begin_frame();       \
        events;                    \
        input_actions_update(dt);  \
    } while (0)

static void
test_action_buttons(void)
{
    input_reset();
    input_actions_set_defaults();
    input_action jump = input_action_find("jump");
    CHECK(jump != INPUT_ACTION_NONE);

    FRAME(0.016f, input_on_key(INPUT_KEY_SPACE, true));
    CHECK(input_action_down(jump) && input_action_pressed(jump) && input_action_value(jump) == 1.0f);

    // A second binding going down while the first is held is no new press...
    input_pad_raw pad = pad_at_rest();
    pad.buttons[INPUT_PAD_A] = true;
    FRAME(0.016f, input_on_pad(0, &pad));
    CHECK(input_action_down(jump) && !input_action_pressed(jump));

    // ...and letting go of one of them is no release.
    FRAME(0.016f, input_on_key(INPUT_KEY_SPACE, false); input_on_pad(0, &pad));
    CHECK(input_action_down(jump) && !input_action_released(jump));

    pad.buttons[INPUT_PAD_A] = false;
    FRAME(0.016f, input_on_pad(0, &pad));
    CHECK(!input_action_down(jump) && input_action_released(jump) && input_action_value(jump) == 0.0f);

    // A tap inside one frame: pressed and released together.
    FRAME(0.016f, input_on_key(INPUT_KEY_SPACE, true); input_on_key(INPUT_KEY_SPACE, false); input_on_pad(0, &pad));
    CHECK(input_action_pressed(jump) && input_action_released(jump) && !input_action_down(jump));
    FRAME(0.016f, input_on_pad(0, &pad));
    CHECK(!input_action_pressed(jump) && !input_action_released(jump));

    // A trigger holds a button action from halfway.
    input_action fire = input_action_add("fire", INPUT_ACTION_BUTTON);
    CHECK(input_action_bind(fire, input_binding_pad_axis(INPUT_PAD_RIGHT_TRIGGER, 1.0f)));
    pad.axes[INPUT_PAD_RIGHT_TRIGGER] = -0.2f;   // 40% pulled
    FRAME(0.016f, input_on_pad(0, &pad));
    CHECK(!input_action_down(fire));
    pad.axes[INPUT_PAD_RIGHT_TRIGGER] = 0.5f;    // 75%
    FRAME(0.016f, input_on_pad(0, &pad));
    CHECK(input_action_pressed(fire));

    // An unknown name is harmless.
    CHECK(input_action_find("no_such_action") == INPUT_ACTION_NONE);
    CHECK(!input_action_down(INPUT_ACTION_NONE) && input_action_value(INPUT_ACTION_NONE) == 0.0f);
    CHECK(input_action_name(INPUT_ACTION_NONE)[0] == '\0');
}

static void
test_action_axes(void)
{
    input_reset();
    input_actions_set_defaults();
    input_action move_x = input_action_find("move_x");
    input_action move_y = input_action_find("move_y");
    input_action up     = input_action_find("up_down");

    // Keys: W forward, A left, both A and D cancel out.
    FRAME(0.016f, input_on_key(INPUT_KEY_W, true); input_on_key(INPUT_KEY_A, true));
    CHECK(input_action_value(move_y) == 1.0f && input_action_value(move_x) == -1.0f);
    FRAME(0.016f, input_on_key(INPUT_KEY_D, true));
    CHECK(input_action_value(move_x) == 0.0f);
    FRAME(0.016f, input_on_key(INPUT_KEY_A, false); input_on_key(INPUT_KEY_D, false); input_on_key(INPUT_KEY_W, false));

    // The stick: pushed up is forward. With a key held too, the one pushed further wins.
    input_pad_raw pad = pad_at_rest();
    pad.axes[INPUT_PAD_LEFT_Y] = -1.0f;
    FRAME(0.016f, input_on_pad(0, &pad));
    CHECK(near(input_action_value(move_y), 1.0f));
    pad.axes[INPUT_PAD_LEFT_Y] = 0.6f;   // half back
    FRAME(0.016f, input_on_pad(0, &pad); input_on_key(INPUT_KEY_W, true));
    CHECK(input_action_value(move_y) == 1.0f);
    FRAME(0.016f, input_on_pad(0, &pad); input_on_key(INPUT_KEY_W, false));
    CHECK(near(input_action_value(move_y), -0.5f));   // (0.6 - 0.2) / 0.8, flipped

    // Bumpers fly up and down.
    pad = pad_at_rest();
    pad.buttons[INPUT_PAD_LEFT_BUMPER] = true;
    FRAME(0.016f, input_on_pad(0, &pad));
    CHECK(input_action_value(up) == -1.0f);
    pad.buttons[INPUT_PAD_LEFT_BUMPER] = false;
    FRAME(0.016f, input_on_pad(0, &pad); input_on_key(INPUT_KEY_E, true));
    CHECK(input_action_value(up) == 1.0f);
}

static void
test_action_look(void)
{
    input_reset();
    input_actions_set_defaults();
    input_action look_x = input_action_find("look_x");
    input_action look_y = input_action_find("look_y");

    // Mouse: 0.1 degree a pixel, whatever the frame time.
    FRAME(0.016f, input_on_cursor(100.0, 100.0, true));
    FRAME(0.5f,   input_on_cursor(130.0, 90.0, true));
    CHECK(near(input_action_value(look_x), 3.0f) && near(input_action_value(look_y), -1.0f));
    FRAME(0.016f, (void)0);
    CHECK(input_action_value(look_x) == 0.0f);

    // Stick: 150 degrees a second, times the frame.
    input_pad_raw pad = pad_at_rest();
    pad.axes[INPUT_PAD_RIGHT_X] = 1.0f;
    FRAME(0.02f, input_on_pad(0, &pad));
    CHECK(near(input_action_value(look_x), 3.0f));

    // Both at once add up.
    FRAME(0.02f, input_on_pad(0, &pad); input_on_cursor(140.0, 90.0, true));
    CHECK(near(input_action_value(look_x), 4.0f));

    // A long stall counts as 0.1 s.
    FRAME(2.0f, input_on_pad(0, &pad));
    CHECK(near(input_action_value(look_x), 15.0f));
}

static void
test_action_setup(void)
{
    input_actions_clear();
    CHECK(input_action_count() == 0);

    input_action a = input_action_add("walk", INPUT_ACTION_AXIS);
    CHECK(a != INPUT_ACTION_NONE && input_action_count() == 1);
    CHECK(input_action_add("walk", INPUT_ACTION_AXIS) == a && input_action_count() == 1);   // same one again
    CHECK(input_action_add("", INPUT_ACTION_BUTTON) == INPUT_ACTION_NONE);
    CHECK(input_action_add("a_name_far_too_long_for_an_action_slot", INPUT_ACTION_BUTTON) == INPUT_ACTION_NONE);

    // Mouse motion only fits a delta axis.
    CHECK(!input_action_bind(a, input_binding_mouse_motion(0, 1.0f)));
    CHECK(!input_action_bind(INPUT_ACTION_NONE, input_binding_key(INPUT_KEY_W)));

    for (int i = 0; i < INPUT_ACTION_MAX_BINDINGS; ++i)
        CHECK(input_action_bind(a, input_binding_key(INPUT_KEY_W)));
    CHECK(!input_action_bind(a, input_binding_key(INPUT_KEY_W)));   // full

    int count = 0;
    const input_binding* bindings = input_action_bindings(a, &count);
    CHECK(count == INPUT_ACTION_MAX_BINDINGS && bindings[0].kind == INPUT_BINDING_KEY && bindings[0].code == INPUT_KEY_W);
    input_action_unbind_all(a);
    input_action_bindings(a, &count);
    CHECK(count == 0);

    for (int i = 1; i < INPUT_ACTION_MAX; ++i)
    {
        char name[16];
        snprintf(name, sizeof(name), "action_%d", i);
        CHECK(input_action_add(name, INPUT_ACTION_BUTTON) != INPUT_ACTION_NONE);
    }
    CHECK(input_action_add("one_too_many", INPUT_ACTION_BUTTON) == INPUT_ACTION_NONE);

    input_actions_set_defaults();
    CHECK(input_action_count() == 7 && input_action_find("action_1") == INPUT_ACTION_NONE);
    CHECK(input_action_get_kind(input_action_find("look_y")) == INPUT_ACTION_AXIS_DELTA);
}

int
main(void)
{
    test_action_buttons();
    test_action_axes();
    test_action_look();
    test_action_setup();
    test_key_edges();
    test_mouse_buttons_and_focus();
    test_mouse_movement();
    test_pads();

    printf("input_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
