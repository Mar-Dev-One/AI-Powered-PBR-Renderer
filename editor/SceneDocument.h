#pragma once

#include "Assets/AssetCache.h"
#include "Commands/Commands.h"
#include "Commands/History.h"
#include "Core/Defines.h"
#include "Project/Project.h"
#include "World/Environment.h"
#include "World/World.h"

#include "cJSON.h"

// The scene being edited: one World -- the single source of truth the
// inspector, gizmo, console and agent all change through `commands` -- its
// undo history, its environment, the file it was opened from or saved to,
// and what that file holds, which is how unsaved changes are noticed (every
// command that changes the world bumps commands.revision).
//
// commands.w and commands.environment point into the document, so it must
// not move after doc_init(). Replacing the scene keeps them valid.

typedef struct scene_document
{
    world             scene;
    command_history   history;
    command_context   commands;
    scene_environment env;     // HDRI, IBL / skybox switches, exposure, ambient; saved with the scene

    char              name[PROJECT_SCENE_MAX];   // "" = never saved
    uint64            saved_revision;
    scene_environment saved_env;

    // A scene.load (from the console, a script or the agent) renames the
    // document. Undoing the load must put the old name back -- or Ctrl+S
    // would write the old scene into the loaded one's file -- and redoing it
    // the loaded name. The load's undo step is known by its changes buffer
    // (unique, and it stays put as the history grows or drops old steps);
    // it's found once the step is recorded (a chat turn records it when the
    // turn ends).
    b8                load_watched;
    const void*       load_step;       // the step's history_step.changes; NULL until recorded
    b8                load_undone;
    char              name_before_load[PROJECT_SCENE_MAX];
    char              name_loaded[PROJECT_SCENE_MAX];
} scene_document;

// An empty, untitled, unchanged scene in `assets`. `commands` supplies the
// context's hooks and command tables (on_change, host and module commands);
// its world, history and environment are set to the document's.
void doc_init(scene_document* doc, asset_cache* assets, command_context commands);

// Frees the history and the world (releasing their assets, not the cache).
void doc_shutdown(scene_document* doc);

// Changes since the scene was opened or saved: any command that changed the
// world (undo included), or the environment.
b8 doc_dirty(const scene_document* doc);

// The document now matches the file `name` (NULL keeps the current name).
void doc_mark_saved(scene_document* doc, const char* name);

// A scene.save or a replacing scene.load run as a command (by anyone: the
// menus, the console, a script, the agent) -- Commands.h's
// cmd_scene_file_fn. The document now matches the file `name`; after a load,
// undoing it gives the document its previous name back
// (doc_follow_history()).
void doc_scene_file(scene_document* doc, cmd_scene_file_op op, const char* name);

// Once a frame, and after undo / redo: if the history went back past a
// command-driven load, the document is again the scene it was before it,
// and forward past it again, the loaded one -- with unsaved changes either
// way, so Ctrl+S writes what's on screen to the file named in the title.
void doc_follow_history(scene_document* doc);

// A fresh undo history: steps from another scene mean nothing in this one.
void doc_reset_history(scene_document* doc);

// Reads a parsed scene file into `out_world` (initialized here, in the
// document's asset cache) and `out_env`, without touching the document --
// so a bad file leaves the current scene as it was. A scene that names no
// HDRI gets `default_hdri`. False, with why in `error` (and nothing left to
// free).
b8 doc_read(const scene_document* doc, const cJSON* scene_json, const char* default_hdri, world* out_world,
            scene_environment* out_env, char* error, size_t error_size);

// Makes `next` (from doc_read(), taken over) the scene, named `name`, with a
// fresh history; bumps the revision and marks it saved. Close any edit in
// progress first. Returns the new revision, for the change announcement.
uint64 doc_replace(scene_document* doc, world* next, const scene_environment* env, const char* name);
