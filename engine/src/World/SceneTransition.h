#pragma once

#include "../Core/Defines.h"
#include "World.h"

// Going from scene to scene through SceneTrigger boxes (Components.h):
//
//   idle       a PlayerController entity walking into a trigger with a scene
//              starts the fade out (one already inside when the scene began
//              has to step out and back in, so arriving in a doorway doesn't
//              bounce straight back)
//   fading out the view darkens over the trigger's fade_seconds
//   load       scene_transition_update() hands back the scene to load, once;
//              the caller loads it its own way (the game: its scenes/ file;
//              the editor's Play: into its copy) and reports back with
//              scene_transition_loaded(), which puts the player at the spawn
//   fading in  the view brightens again
//
// While it runs, the player can't move (scene_transition_busy()), and the
// caller darkens the picture by scene_transition_fade().

typedef enum scene_transition_phase
{
    SCENE_TRANSITION_IDLE,
    SCENE_TRANSITION_FADING_OUT,
    SCENE_TRANSITION_LOAD,        // waiting for scene_transition_loaded()
    SCENE_TRANSITION_FADING_IN,
} scene_transition_phase;

typedef struct scene_transition
{
    scene_transition_phase phase;
    f32       time;                    // into this phase
    f32       seconds;                 // each fade's length
    char      scene[ENTITY_NAME_MAX];  // where it's going
    char      spawn[ENTITY_NAME_MAX];
    entity_id inside;                  // the trigger a player stood in last frame
} scene_transition;

// Idle, for `w` as it starts: a trigger a player already stands in doesn't
// fire until they leave it. Needs world_update() first.
void scene_transition_reset(scene_transition* t, const world* w);

// Once a frame, after world_update(). Returns the scene to load now (until
// the next call), or NULL.
const char* scene_transition_update(scene_transition* t, const world* w, f32 dt);

// The scene asked for has replaced `w` (`loaded`), or couldn't be (the old
// one stays). Puts its player at the spawn entity, if there's one by that
// name, and fades back in.
void scene_transition_loaded(scene_transition* t, world* w, b8 loaded);

// How dark the view is: 0 (as it is) .. 1 (black).
f32 scene_transition_fade(const scene_transition* t);

b8 scene_transition_busy(const scene_transition* t);

// The SceneTrigger whose box holds a PlayerController entity's position,
// or ENTITY_NULL (exposed for tests).
entity_id scene_trigger_holding_player(const world* w);
