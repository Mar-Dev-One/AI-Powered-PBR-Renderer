#include "PlaySession.h"

#include <math.h>

void
play_begin(play_session* play, scene_document* doc)
{
    if (play_active(play))
        return;

    world_copy(&play->copy, &doc->scene);

    // The same commands and hooks, on the copy, without undo: what happens
    // in Play is thrown away as a whole.
    play->commands             = doc->commands;
    play->commands.w           = &play->copy;
    play->commands.history     = NULL;
    play->commands.revision    = 0;
    play->commands.batch_depth = 0;
    play->commands.save_refused =
        "the scene is playing, so what's on screen is Play's throwaway copy; stop first (play.set stop / Ctrl+P), "
        "then save the edited scene";

    play->saved_env = doc->env;
    play->time      = 0.0f;
    play->step      = false;
    play->mode      = PLAY_RUNNING;   // same IDs in the copy: a selection carries over
}

void
play_end(play_session* play, scene_document* doc)
{
    if (!play_active(play))
        return;

    play->mode = PLAY_STOPPED;
    doc->env   = play->saved_env;
    world_shutdown(&play->copy);
    assets_collect_unused(doc->scene.assets);   // models only the copy used (placed while playing)
}

void
play_toggle_pause(play_session* play)
{
    if (play->mode == PLAY_RUNNING)
        play->mode = PLAY_PAUSED;
    else if (play->mode == PLAY_PAUSED)
        play->mode = PLAY_RUNNING;
}

void
play_advance(play_session* play, f32 dt)
{
    f32 step = 0.0f;
    if (play->mode == PLAY_RUNNING)
        step = fminf(dt, 0.1f);
    else if (play->mode == PLAY_PAUSED && play->step)
        step = PLAY_STEP_SECONDS;
    play->step = false;

    if (step > 0.0f)
    {
        world_tick(&play->copy, step);
        play->time += step;
    }
}

const char*
play_mode_name(play_mode mode)
{
    return mode == PLAY_RUNNING ? "playing" : mode == PLAY_PAUSED ? "paused" : "stopped";
}
