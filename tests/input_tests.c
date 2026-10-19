// input_tests.c
//
// Headless tests for Platform/Input.h: events fed in by hand, as App.c's
// window callbacks do, then read back frame by frame.

#include "Platform/Input.h"

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

int
main(void)
{
    test_key_edges();
    test_mouse_buttons_and_focus();
    test_mouse_movement();
    test_pads();

    printf("input_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
