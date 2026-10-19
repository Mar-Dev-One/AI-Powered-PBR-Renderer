#pragma once

#include "../Core/Defines.h"
#include "World.h"

// Moves every PlayerController entity (Components.h) from the player's
// input: the game runtime and the editor's Play call player_controller_tick()
// once a frame, before world_tick() and world_update().
//
// Looking turns Transform.rotation_deg: yaw (y) by look_x, pitch (x) by
// look_y, held within +-89 degrees so the view never flips over. Moving
// follows where it faces -- flat for walk, along the view and up_down for fly
// -- in its parent's space if it has one. Diagonals are no faster.

// What the player is doing this frame, as the input actions read
// (Platform/InputActions.h): move -1..1 (+y forward, +x right, +up_down up),
// look in degrees this frame (+x right, +y down).
typedef struct player_input
{
    f32 move_x, move_y, up_down;
    f32 look_x, look_y;
    b8  sprint;
} player_input;

// The live actions move_x, move_y, up_down, look_x, look_y and sprint; one
// a project doesn't have reads 0.
player_input player_input_from_actions(void);

void player_controller_tick(world* w, f32 dt, const player_input* input);
