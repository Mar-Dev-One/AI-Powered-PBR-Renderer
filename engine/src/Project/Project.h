#pragma once

#include "../Core/Defines.h"
#include "../World/Environment.h"

#include "cJSON.h"

// A project: one folder holding everything a game is made of.
//
//   project.json   name, startup scene, window size, default HDRI
//   scenes/        scene files (World/Serialize.h), by name: scenes/<name>.json
//   models/        .glb / .gltf / .obj, which scenes name as "models/..."
//   hdri/          .hdr environments, "hdri/..."
//   textures/
//
// The editor opens one at a time and makes its folder the project root
// (Core/Paths.h), so every "models/..." / "hdri/..." / scene name resolves
// inside it; the game runtime plays one. Nothing here touches the GPU.
//
// project.json (every key but "format" optional when read):
//   { "format": "pbr-project", "version": 1, "name": "Racing Game",
//     "startup_scene": "main", "window": { "width": 1920, "height": 1080 },
//     "default_hdri": "hdri/sky.hdr" | null }

#define PROJECT_FILE      "project.json"
#define PROJECT_FORMAT    "pbr-project"
#define PROJECT_VERSION   1
#define PROJECT_NAME_MAX  64
#define PROJECT_SCENE_MAX 65   // scene names are 1-64 letters, digits, '_' and '-'
#define PROJECT_PATH_MAX  1024

typedef struct project_settings
{
    char   name[PROJECT_NAME_MAX];
    char   startup_scene[PROJECT_SCENE_MAX];
    uint16 window_width;
    uint16 window_height;
    char   default_hdri[ENVIRONMENT_HDRI_MAX];   // under the project; "" = none
} project_settings;

// `name` (NULL = "Untitled"), startup scene "main", 1920 x 1080, no HDRI.
project_settings project_settings_default(const char* name);

// A project name: 1-63 characters, none of them a control character or one
// a file name can't hold (\ / : * ? " < > |), not "." or "..", and not
// starting or ending with a space or dot -- it doubles as the folder name.
b8 project_name_valid(const char* name);

// The rule scene.save / scene.load apply (Commands/Commands.h).
b8 project_scene_name_valid(const char* name);

// True if `directory` has a project.json.
b8 project_exists(const char* directory);

// Reads `directory`/project.json into `out` (missing keys keep the
// defaults). False, with why in `error`, if it's missing, isn't a project
// file or has a bad value.
b8 project_read(const char* directory, project_settings* out, char* error, size_t error_size);

// Writes `directory`/project.json. False, with why in `error`, if a value is
// invalid or the file can't be written.
b8 project_write(const char* directory, const project_settings* settings, char* error, size_t error_size);

// Makes a new project in `directory`, which must not exist yet or be empty:
// the folders above, project.json named `name`, and scenes/main.json holding
// project_starter_scene(). False, with why in `error`.
b8 project_create(const char* directory, const char* name, char* error, size_t error_size);

// What a new project or "New Scene" starts with, as a scene file (load it
// with deserialize_scene() / deserialize_environment()): a ground plane, a
// sun, a 1 m cube on the ground and a camera looking at it (the game views
// through it), in the default environment with the project's default HDRI.
// Built as JSON rather than in a World because built-in meshes need a GPU.
// Caller owns the result (cJSON_Delete).
cJSON* project_starter_scene(const project_settings* settings);

// --- Recent projects ------------------------------------------------------------
// The editor's File > Recent list, most recent first, kept in
// ~/.pbr-renderer/recent_projects.json ({"projects": ["C:/Games/Racer", ...]}).
// `file` is that path; NULL means the default one (tests pass their own).

#define PROJECT_RECENT_MAX 10

typedef struct project_recent
{
    char   paths[PROJECT_RECENT_MAX][PROJECT_PATH_MAX];
    uint32 count;
} project_recent;

// Only folders that still hold a project are kept. A missing or unreadable
// file is an empty list.
void project_recent_load(project_recent* out, const char* file);

// Puts `directory` first (moving it if listed, dropping the oldest past
// PROJECT_RECENT_MAX) and saves the list.
void project_recent_add(project_recent* list, const char* directory, const char* file);

// --- Importing assets -------------------------------------------------------------

// Copies the asset file `source` into the project in `project_dir`:
//   .glb            -> models/<file>
//   .gltf           -> models/<stem>/, with every buffer and image it names
//   .obj            -> models/<stem>/, with its .mtl files and their textures
//   .hdr            -> hdri/<file>
// `out_key` gets the path scenes use for it ("models/chair.glb"). Every file
// is checked before anything is copied, so on false (unsupported type, a
// referenced file missing or outside the model's folder, already in the
// project) nothing changed and `error` says why.
b8 project_import_asset(const char* project_dir, const char* source, char* out_key, size_t key_size,
                        char* error, size_t error_size);

// --- Exporting the game -------------------------------------------------------------
// An export is a folder a player can run, on its own:
//
//   <out>/<project name>.exe       the game (options->runtime_exe, renamed)
//   <out>/*.dll                    options->extra_files (the C++ runtime)
//   <out>/engine/shaders/          the engine's shaders (Core/Paths.h looks there)
//   <out>/project.json, scenes/    the project's settings and every saved scene
//   <out>/models/..., hdri/...     only what those scenes use: each MeshRenderer
//                                  model (with the files a .gltf / .obj names),
//                                  each environment HDRI, the default HDRI
//   <out>/.pbr-export              marks the folder as an export
//
// The game finds its project next to itself, so double-clicking the .exe
// plays the startup scene. Saved scenes only: unsaved edits aren't in it.

typedef struct project_export_options
{
    const char*        runtime_exe;   // the game executable to ship
    const char*        engine_dir;    // the engine root; its shaders/ are copied
    const char* const* extra_files;   // copied next to the game (may be NULL)
    uint32             extra_count;
} project_export_options;

typedef struct project_export_report
{
    char   executable[PROJECT_PATH_MAX];   // the exported game
    uint32 files;                          // copied, the executable included
    uint32 scenes;
    uint64 bytes;
} project_export_report;

// Exports the project in `project_dir` into `out_dir`: a new or empty
// folder, or an earlier export (its files are replaced; files it no longer
// needs stay). Everything is checked before anything is copied; on false
// (`error` says why: a scene uses a file that's missing or outside the
// project, the startup scene isn't saved, `out_dir` holds other files or is
// inside the project) nothing was copied.
b8 project_export(const char* project_dir, const char* out_dir, const project_export_options* options,
                  project_export_report* report, char* error, size_t error_size);
