#pragma once

// The editor's state and the pieces of editor/ that share it. Each .c file is
// one part of the editor:
//
//   main.c          the App hooks, the frame, the keyboard and the panel layout
//   HostCommands.c  the commands for state the World doesn't hold (camera,
//                   environment, rendering, project, play, export)
//   Actions.c       what menus, keys and panels ask for: open / save scenes and
//                   projects, undo / redo, duplicate / delete, Play, framing
//   Viewport.c      the Viewport panel: the frame, its toolbar, picking, the
//                   selection outline, the overlays (lights, cameras, probes,
//                   the ground grid) and the gizmo
//   Panels.c        Scene (Add and Environment), Renderer, Assets, History and
//                   GPU timings
//   Menus.c         the main menu, the status bar and every dialog
//   Export.c        Export: project_export() and the exported game's check
//   Agent.c         the chat agent's Python sidecar
//
// Everything that changes the scene runs as a command (Commands/Commands.h)
// through active_commands(), so the console, the agent, the panels and the
// gizmo all go through the same validated, undoable path.

#include "App.h"
#include "Assets/AssetCache.h"
#include "Chat/ChatSession.h"
#include "Core/Defines.h"
#include "Core/Process.h"
#include "Net/CommandServer.h"
#include "Project/Project.h"
#include "RHI/RHI.h"
#include "Renderer/IBL.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Camera.h"
#include "Scene/OrbitCamera.h"
#include "UI/ChatPanel.h"
#include "UI/CommandConsole.h"
#include "UI/EntityPanel.h"
#include "UI/Gizmo.h"
#include "UI/Inspector.h"
#include "UI/LogPanel.h"
#include "World/World.h"

#include "PlaySession.h"
#include "SceneDocument.h"

#include "cJSON.h"
#include "cimgui.h"

// Resolution of *each* cascade's shadow map (Renderer/Shadow.h). Depth-only
// framebuffers are cheap, so this and CASCADE_COUNT are both sized
// generously; CASCADE_COUNT tops out at CSM_MAX_CASCADES.
#define SHADOW_MAP_SIZE 2048
#define CASCADE_COUNT   4

// HDRIs the environment picker lists: the .hdr files in the project's hdri/
// (and one folder level below it).
#define HDRI_LIST_MAX 64

// Something that replaces the scene on screen, so it waits for an answer
// to "save changes?" when there are any (see request()).
typedef enum pending_action
{
    ACTION_NONE,
    ACTION_OPEN_PROJECT,   // pending_arg: the project folder
    ACTION_NEW_SCENE,
    ACTION_OPEN_SCENE,     // pending_arg: the scene name
    ACTION_QUIT
} pending_action;

// The exported game's --check run.
typedef enum export_check
{
    CHECK_NONE,      // no export yet (or it failed before the check)
    CHECK_RUNNING,
    CHECK_PASSED,
    CHECK_FAILED     // any non-zero exit code, crashes included, or it couldn't start
} export_check;

// What the Viewport draws over the scene (View > Overlays, the toolbar).
typedef struct overlay_settings
{
    b8 grid;      // the ground plane's grid, 1 m lines
    b8 lights;    // a marker per light; the selected one's range / cone / direction
    b8 cameras;   // each Camera entity's view frustum
    b8 probes;    // each ReflectionProbe's box
} overlay_settings;

// Which panels are open (View menu).
typedef struct panel_visibility
{
    b8 scene;
    b8 entities;
    b8 inspector;
    b8 renderer;
    b8 assets;
    b8 history;
    b8 console;      // F1
    b8 chat;
    b8 log;          // opens by itself when an error is logged
    b8 gpu_timings;
} panel_visibility;

typedef struct editor_state
{
    App* app;

    // --- Project (Project/Project.h) -------------------------------------------
    // The open project's settings (its folder is the project root,
    // paths_project_root()), and the File > Recent list.
    project_settings project;
    project_recent   recent;

    // --- The scene being edited (SceneDocument.h) ---------------------------------
    // Its World, undo history, commands and environment, and whether it has
    // unsaved changes.
    asset_cache    assets;
    scene_document doc;

    // --- Play mode (PlaySession.h, the Viewport toolbar) ---------------------------
    // While it plays, everything that shows or edits "the scene on screen" --
    // the view, selection, gizmo, Inspector, Entities, console, command
    // server, agent, Add -- works on the copy (active_commands()). Saving and
    // opening act on the edited scene, so they stop play or wait.
    play_session play;

    // --- Panels ----------------------------------------------------------------------
    panel_visibility show;
    command_console  console;
    inspector        inspector;      // the selected entity's fields; edits run as undoable commands
    entity_panel     entities;       // the outliner's filter
    log_panel        log;            // the engine's log, from main() on (UI/LogPanel.h)
    f32              cpu_frame_ms;   // smoothed, for the GPU timings window and the status bar

    // Lets other processes (the MCP server, an agent, scripts) run the same
    // commands over a localhost WebSocket; see Net/CommandServer.h. NULL if
    // it couldn't start -- the app works without it.
    command_server* server;

    // --- Chat (Chat/ChatSession.h) -------------------------------------------------
    // The in-app chat with the AI agent. The agent is a Python sidecar
    // (agent/pbr_agent/agent.py) that the app starts at launch and that
    // connects back through the command server; agent_note says why it isn't
    // there when it isn't. Any agent that connects works, not only ours.
    chat_session chat;
    chat_panel   chat_panel;
    process*     agent_process;
    char         agent_note[1280];
    char         agent_log[1024];

    // --- Rendering -------------------------------------------------------------
    // Renderer/SceneRenderer.h runs the whole lit frame; build_render_settings()
    // turns the state below and doc.env into its settings. These are the
    // editor's viewing choices, not the scene's: they aren't saved with it.
    scene_renderer     renderer;
    b8                 shadows;                // every kind of shadow map (Renderer panel, render.set)
    f32                cascade_split_lambda;   // 0 = even cascade splits, 1 = logarithmic (csm_update())
    b8                 debug_show_cascades;    // tints shading by cascade index; see pbr.frag's u_debug_show_cascades
    b8                 debug_unclustered;      // every light evaluated everywhere: clustering's reference
    scene_debug_view   debug_view;             // lit, or one input to the shading (SceneRenderer.h)
    scene_antialiasing antialiasing;           // FXAA from on_init()
    overlay_settings   overlays;

    // --- Image-based lighting -------------------------------------------------
    // doc.env (World/Environment.h) is the single source of truth for the
    // HDRI, IBL / skybox switches, exposure and ambient strength: the GUI, the
    // environment.set / render.set commands and opening a scene all write it,
    // and saving stores it with the scene. `ibl` is doc.env.hdri baked
    // (Renderer/IBL.h), re-baked when it changes.
    ibl_cache ibl;

    // --- Cameras ------------------------------------------------------------------------
    // `cam` is the editor's own orbit camera (Scene/OrbitCamera.h). The view
    // can instead look through a Camera entity: `look_through` (Viewport
    // toolbar, view.look_through), or -- while playing with `play_game_camera`
    // -- the scene's first Camera, as the game does. `view_cam` is the camera
    // this frame is drawn with (update_view_camera()): picking, the outline,
    // the overlays, the gizmo and screenshots all use it.
    camera       cam;
    orbit_camera orbit;
    entity_id    look_through;       // ENTITY_NULL = the editor camera
    b8           play_game_camera;   // Play views the scene's first Camera, as the game will
    camera       view_cam;
    entity_id    view_cam_entity;    // whose view `view_cam` is (ENTITY_NULL = the editor camera)

    // --- Viewport (the "Viewport" panel) ----------------------------------------
    // The scene renders into view_target, an RGBA8 texture exactly the
    // panel's size, which the panel shows; the camera's aspect follows it.
    // view_x/y/width/height is that image's rectangle in ImGui screen
    // coordinates (desktop pixels, since panels can leave the main window)
    // as of this frame: picking, the outline and the gizmo all work in it.
    rhi_framebuffer view_target;
    uint16          view_target_width;
    uint16          view_target_height;
    f32             view_x;
    f32             view_y;
    f32             view_width;
    f32             view_height;

    // --- Selection --------------------------------------------------------------
    // One entity of the scene. A short left click in the view picks
    // (World/World.h world_pick()); a drag still orbits. The Entities panel
    // shows it and can set it; F frames it; Esc clears it.
    entity_id selected;
    b8        reveal_selection;   // picked in the view this frame: open the Entities tree to it
    f32       press_x;            // where the left button went down in the view (screen coordinates)
    f32       press_y;
    b8        press_on_gizmo;     // that press began on a gizmo handle: neither orbit nor pick

    // --- Gizmo (UI/Gizmo.h) -------------------------------------------------------
    // Move / rotate / scale handles on the selection. W / E / R pick the
    // operation; the toolbar has the local/world toggle and snapping (Ctrl
    // while dragging flips it). Edits are component.set on Transform, one undo
    // step per drag, like the Inspector.
    gizmo_operation gizmo_op;
    gizmo_space     gizmo_space;
    b8              gizmo_snap;
    f32             snap_move;        // meters
    f32             snap_rotate;      // degrees
    f32             snap_scale;       // scale factor step
    b8              gizmo_hot;        // over or dragging a handle, as of last frame's draw
    b8              gizmo_editing;    // a history group is open for the drag in progress
    b8              gizmo_executing;  // inside one of the gizmo's own commands (not logged)

    // --- Export (the Export dialog, Export.c) ------------------------------------------
    // project_export() into export_dir, then the exported game's own --check
    // run (check_process, in the background): it loads the startup scene in
    // a hidden window and saves what it rendered to check_png, shown in the
    // dialog as check_picture.
    char                  export_dir[PROJECT_PATH_MAX];
    char                  export_status[PROJECT_PATH_MAX + 640];
    b8                    export_ok;      // the last export succeeded (the check may still run)
    project_export_report export_report;
    process*              check_process;
    export_check          check;
    int                   check_exit_code;   // the game's, once check is CHECK_FAILED
    rhi_texture           check_picture;
    char                  check_png[PROJECT_PATH_MAX];

    // --- Add (Scene panel, Assets panel) -------------------------------------------
    f32 add_size;   // meters: the longest side of a model or shape added from the panels

    // --- Menus and dialogs --------------------------------------------------------
    // A menu item sets open_popup; the next draw_popups() opens that modal.
    // An action that replaces the scene waits in `pending` while "save
    // changes?" is asked.
    const char*    open_popup;
    pending_action pending;
    char           pending_arg[PROJECT_PATH_MAX];
    b8             continue_after_save_as;   // Save As was opened from "save changes?"
    char           popup_error[640];         // shown in the open dialog until it closes or succeeds

    char             save_as_name[PROJECT_SCENE_MAX];
    char             new_project_name[PROJECT_NAME_MAX];
    char             new_project_location[PROJECT_PATH_MAX];
    char             open_project_path[PROJECT_PATH_MAX];
    char             import_path[PROJECT_PATH_MAX];
    project_settings settings_edit;     // the Project Settings dialog's copy

    // Its Input section: the action waiting for "Press to bind" (-1 none),
    // each action's typed binding, and the action being added.
    int  input_capture;
    char input_binding_text[INPUT_ACTION_MAX][INPUT_BINDING_TEXT_MAX];
    char input_new_action[INPUT_ACTION_NAME_MAX];
    int  input_new_kind;
} editor_state;

#define POPUP_UNSAVED      "Unsaved changes"
#define POPUP_SAVE_AS      "Save scene as"
#define POPUP_NEW_PROJECT  "New project"
#define POPUP_OPEN_PROJECT "Open project"
#define POPUP_IMPORT       "Import asset"
#define POPUP_SETTINGS     "Project settings"
#define POPUP_EXPORT       "Export game"

#define WINDOW_SCENE     "Scene"
#define WINDOW_ENTITIES  "Entities"
#define WINDOW_INSPECTOR "Inspector"
#define WINDOW_RENDERER  "Renderer"
#define WINDOW_ASSETS    "Assets"
#define WINDOW_HISTORY   "History"
#define WINDOW_CONSOLE   "Console (F1)"
#define WINDOW_CHAT      "Chat"
#define WINDOW_LOG       "Log"
#define WINDOW_TIMINGS   "GPU timings"
#define WINDOW_VIEWPORT  "Viewport"

// The colors the editor's own text and markings use.
#define COLOR_ERROR   ((ImVec4_c){ 1.0f, 0.45f, 0.4f, 1.0f })
#define COLOR_WARNING ((ImVec4_c){ 1.0f, 0.71f, 0.0f, 1.0f })
#define COLOR_OK      ((ImVec4_c){ 0.31f, 0.78f, 0.31f, 1.0f })

// --- The scene on screen ---------------------------------------------------------
// The edited scene and its commands, or Play's copy while playing.

static inline command_context*
active_commands(editor_state* state)
{
    return play_active_commands(&state->play, &state->doc);
}

static inline world*
active_world(editor_state* state)
{
    return active_commands(state)->w;
}

// --- main.c --------------------------------------------------------------------------

// This frame's SceneRenderer settings from doc.env and the viewing choices.
scene_render_settings build_render_settings(const editor_state* state);

// --- HostCommands.c ------------------------------------------------------------------

typedef struct hdri_list
{
    char   paths[HDRI_LIST_MAX][ENVIRONMENT_HDRI_MAX];   // scene paths: "hdri/sky.hdr", "hdri/night/moon.hdr"
    uint32 count;
} hdri_list;

// The project's .hdr files.
void list_hdris(hdri_list* out);

// The command table for the context's host_commands.
const cmd_host_command* editor_host_commands(uint32* out_count);

// on_change: shows a change in the console, and pushes it to connected clients.
void on_world_change(void* user_data, const cJSON* event);

// on_scene_file: a scene.save or replacing scene.load of the edited scene,
// run by anyone -- the document now matches that file (SceneDocument.h).
void on_scene_file(void* user_data, command_context* ctx, cmd_scene_file_op op, const char* name);

// f32 -> JSON number at 4 significant digits, so 1.3f reads 1.3 rather
// than 1.2999999523162842.
cJSON* number_json(f32 value);
cJSON* vec3_json(const f32* v);

// The folder new projects go in unless told otherwise: Documents/PBR Projects.
void default_projects_location(char* out, size_t size);

// --- Actions.c -------------------------------------------------------------------

// Runs {cmd, args} on the scene on screen (takes `args`). Returns the result
// on success (caller deletes it), or NULL with the error in the console.
cJSON* run_command(editor_state* state, const char* cmd, cJSON* args);

// Clears the selection if it no longer names a live entity.
void validate_selection(editor_state* state);
void select_entity(editor_state* state, entity_id e);

// Closes any Inspector or gizmo edit in progress, so it doesn't join what
// comes next.
void finish_edits(editor_state* state);

// Undo / redo the edited scene (Ctrl+Z / Ctrl+Y, Edit menu, History panel).
void undo(editor_state* state);
void redo(editor_state* state);
// Undoes or redoes until `cursor` steps of the history are applied.
void history_jump(editor_state* state, uint32 cursor);

// The selection, and its children, copied beside it as one undo step (Ctrl+D).
void duplicate_selection(editor_state* state);
// The selection and everything parented to it (Delete).
void delete_selection(editor_state* state);
// Moves `child` under `parent` (ENTITY_NULL = the scene root), keeping where
// it is in the world.
void reparent(editor_state* state, entity_id child, entity_id parent);

// Points the camera at `entity` (NULL = everything visible), as view.frame
// does; says why in the console if it can't frame an entity.
void frame_view(editor_state* state, const char* entity);
void frame_selection(editor_state* state);
// Back to the editor's own camera, starting from where the view is now.
void use_editor_camera(editor_state* state);

// Play: copies the scene and ticks the copy / throws the copy away.
void play_start(editor_state* state);
void play_stop(editor_state* state);

// Scenes and projects.
b8   open_scene(editor_state* state, const char* name);
void new_scene(editor_state* state);
b8   save_scene_as(editor_state* state, const char* name);
void save_scene(editor_state* state);
b8   open_project(editor_state* state, const char* directory);
// Does `action` now, or -- with unsaved changes -- once "save changes?" is
// answered.
void request(editor_state* state, pending_action action, const char* arg);
void perform(editor_state* state, pending_action action, const char* arg);

// Add: a model or shape on the ground under the view's centre, a light, a
// Camera matching the view, a reflection probe around the view's centre, an
// empty entity. Each selects what it made.
void add_model(editor_state* state, const char* key);
void add_light(editor_state* state, const char* type);   // "point", "spot", "directional"
void add_camera(editor_state* state);
void add_reflection_probe(editor_state* state);
void add_empty(editor_state* state);

// "PBR Editor - Racing Game - main*"
void update_title(editor_state* state);

// --- Viewport.c ------------------------------------------------------------------

// The camera this frame draws with (view_cam): the editor's, or the Camera
// entity being looked through. Call once a frame before drawing.
void update_view_camera(editor_state* state);
void draw_viewport(editor_state* state, ImGuiID dockspace, f32 dt);
// Ends the gizmo's drag group, if one is open.
void gizmo_finish_edit(editor_state* state, command_context* ctx);

// scene_debug_view's labels, for the toolbar's combo and the menus.
extern const char* const DEBUG_VIEW_LABELS[SCENE_VIEW_COUNT];

// --- Panels.c --------------------------------------------------------------------

void draw_scene_panel(editor_state* state);
void draw_renderer_panel(editor_state* state);
void draw_assets_panel(editor_state* state);
void draw_history_panel(editor_state* state);
void draw_gpu_timings(editor_state* state);

// --- Menus.c ---------------------------------------------------------------------

void draw_main_menu(editor_state* state);
void draw_status_bar(editor_state* state);
void draw_popups(editor_state* state);
void open_import_dialog(editor_state* state);
void open_export_dialog(editor_state* state);

// --- Export.c --------------------------------------------------------------------

// Exports into `folder` and, if that worked, starts the exported game's
// check. The outcome goes to export_status (and the return value).
b8   run_export(editor_state* state, const char* folder);
// Once a frame: notices the check finishing, and loads its picture.
void watch_export_check(editor_state* state);
// Forgets the last export (when its project closes, and at shutdown).
void reset_export(editor_state* state);
// Documents/PBR Exports/<project name>.
void default_export_location(const editor_state* state, char* out, size_t size);
// The Game executable to ship; see Export.c.
const char* game_executable(b8* out_release, b8* out_stale);
// Shows `folder` in the system's file browser.
void open_folder(const char* folder);

// --- Agent.c ---------------------------------------------------------------------

void start_agent(editor_state* state);
// Notices the sidecar exiting, and says where to look.
void watch_agent(editor_state* state);
