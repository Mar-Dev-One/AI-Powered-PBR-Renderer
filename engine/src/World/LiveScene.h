#pragma once

#include "../Core/Defines.h"
#include "Environment.h"
#include "World.h"

#include "cJSON.h"

// The editor -> game live link. The editor publishes the world it is editing
// (and its environment) to one scene file whenever it changes; a game running
// with --live watches that file and reloads it, so edits show up in the
// running game within a frame or two. One-way: the editor is the source of
// truth, the game only follows.
//
// The file is an ordinary scene file (World/Serialize.h), replaced in one
// step (write to a temp file, then rename over), so a reader never sees half
// of one. Default location: ~/.pbr-renderer/live_scene.json, next to the
// command server's server.json.
//
// TODO: a first version, meant to be replaced (docs/ROADMAP.md, "Editor /
// runtime split"). Known limits:
//   - Whole-world sync: every change re-serializes and reloads every entity,
//     so cost grows with the scene, not the edit.
//   - The game's runtime state is rebuilt on every edit (testbed/main.c
//     apply_scene() carries Spin poses over by name as a stopgap); physics,
//     scripts or animation would reset.
//   - Entity IDs don't survive a reload; only names carry over.
//   - One-way (the editor can't inspect, pause or pick in the game), up to
//     ~0.2 s latency from polling, one fixed file shared by all editors.
// Planned: an in-editor Play mode for testing, and for a separate game
// process, incremental changes over the command server instead of files.

#define LIVE_SCENE_PATH_MAX 1024

// Writes the default live scene path into `out`.
void live_scene_default_path(char* out, size_t size);

// --- Publisher (the editor) -----------------------------------------------------

typedef struct live_publisher
{
    char  path[LIVE_SCENE_PATH_MAX];
    char* last_text;      // what was last written; NULL before the first write
    f64   last_check;     // window time of the last publish() that compared
} live_publisher;

// `path` NULL = live_scene_default_path(). Creates the folder if needed.
void live_publisher_init(live_publisher* p, const char* path);

// Serializes `w` and `env` and writes them if they differ from the last
// write. At most every `interval` seconds of `now` (pass 0 to force a check).
// Returns true if the file was written.
b8 live_publisher_publish(live_publisher* p, const world* w, const scene_environment* env, f64 now, f64 interval);

void live_publisher_shutdown(live_publisher* p);

// --- Watcher (the game) ------------------------------------------------------------

typedef struct live_watcher
{
    char  path[LIVE_SCENE_PATH_MAX];
    char* last_text;      // the content last handed out
    f64   last_check;
} live_watcher;

// `path` NULL = live_scene_default_path().
void live_watcher_init(live_watcher* w, const char* path);

// At most every `interval` seconds of `now`: if the file's content changed
// since the last scene this returned, the parsed scene (caller frees with
// cJSON_Delete); otherwise -- unchanged, missing, or not valid JSON -- NULL.
cJSON* live_watcher_poll(live_watcher* w, f64 now, f64 interval);

void live_watcher_shutdown(live_watcher* w);
