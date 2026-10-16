#pragma once

#include "Commands/Commands.h"
#include "Core/Defines.h"
#include "World/Environment.h"
#include "World/World.h"

#include "SceneDocument.h"

// The Viewport toolbar's Play: a world_copy() of the edited scene that runs
// the game's behaviours (world_tick()). While it plays, everything that shows
// or edits "the scene on screen" works on the copy, through `commands` (the
// document's context without its undo history). Stopping throws the copy
// away and puts the environment back: the edited scene never changed.

typedef enum play_mode
{
    PLAY_STOPPED,   // editing
    PLAY_RUNNING,   // the copy ticks every frame
    PLAY_PAUSED     // the copy holds still; a step advances it one frame
} play_mode;

// One paused step's worth of game time.
#define PLAY_STEP_SECONDS (1.0f / 60.0f)

typedef struct play_session
{
    play_mode         mode;
    world             copy;
    command_context   commands;
    scene_environment saved_env;   // the document's, as of Play
    f32               time;        // game seconds since Play
    b8                step;        // advance one step next frame (while paused)
} play_session;

static inline b8 play_active(const play_session* play) { return play->mode != PLAY_STOPPED; }

// The scene on screen and its commands: the document's, or the copy's while playing.
static inline command_context*
play_active_commands(play_session* play, scene_document* doc)
{
    return play_active(play) ? &play->commands : &doc->commands;
}

// Copies `doc` and starts running it. No-op if already playing. Close any
// edit in progress on the document first.
void play_begin(play_session* play, scene_document* doc);

// Discards the copy, restores the document's environment and collects the
// models only the copy used. No-op if stopped.
void play_end(play_session* play, scene_document* doc);

void play_toggle_pause(play_session* play);

// This frame's game step: the frame time while running (capped, so a stall
// doesn't fling things around), one fixed step if `step` asked for it while
// paused, else nothing.
void play_advance(play_session* play, f32 dt);

// "playing" / "paused" / "stopped".
const char* play_mode_name(play_mode mode);
