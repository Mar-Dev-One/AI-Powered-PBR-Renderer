#include "PlayerController.h"

#include "../Platform/InputActions.h"

#include <math.h>

#define PITCH_LIMIT 89.0f

player_input
player_input_from_actions(void)
{
    return (player_input){
        .move_x  = input_action_value(input_action_find("move_x")),
        .move_y  = input_action_value(input_action_find("move_y")),
        .up_down = input_action_value(input_action_find("up_down")),
        .look_x  = input_action_value(input_action_find("look_x")),
        .look_y  = input_action_value(input_action_find("look_y")),
        .sprint  = input_action_down(input_action_find("sprint")),
    };
}

static void
tick_one(c_transform* t, const c_player_controller* pc, f32 dt, const player_input* in)
{
    // Look. Yaw turns left as it grows (about +Y), pitch looks up (about +X).
    f32 look_y = pc->invert_look_y ? -in->look_y : in->look_y;
    f32 yaw    = fmodf(t->rotation_deg[1] - in->look_x * pc->look_sensitivity, 360.0f);
    f32 pitch  = t->rotation_deg[0] - look_y * pc->look_sensitivity;
    pitch      = fminf(fmaxf(pitch, -PITCH_LIMIT), PITCH_LIMIT);
    t->rotation_deg[1] = yaw;
    t->rotation_deg[0] = pitch;

    // Move. A camera looks down its local -Z: turned by yaw, then pitch.
    f32 move_x = in->move_x, move_y = in->move_y;
    f32 length = sqrtf(move_x * move_x + move_y * move_y);
    if (length > 1.0f)
    {
        move_x /= length;
        move_y /= length;
    }

    f32 sy = sinf(glm_rad(yaw)), cy = cosf(glm_rad(yaw));
    f32 sp = sinf(glm_rad(pitch)), cp = cosf(glm_rad(pitch));
    vec3 right   = { cy, 0.0f, -sy };
    vec3 forward = { -sy, 0.0f, -cy };   // walking: along the ground
    f32  up      = 0.0f;
    if (pc->mode == PLAYER_FLY)
    {
        forward[0] = -sy * cp;
        forward[1] = sp;
        forward[2] = -cy * cp;
        up         = fminf(fmaxf(in->up_down, -1.0f), 1.0f);
    }

    f32 speed = pc->move_speed * (in->sprint ? pc->sprint_multiplier : 1.0f) * dt;
    for (int i = 0; i < 3; ++i)
        t->position[i] += (forward[i] * move_y + right[i] * move_x) * speed;
    t->position[1] += up * speed;
}

void
player_controller_tick(world* w, f32 dt, const player_input* input)
{
    const uint32 mask = COMPONENT_BIT(COMPONENT_PLAYER_CONTROLLER);
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
        tick_one(world_transform(w, e), world_player_controller(w, e), dt, input);
}
