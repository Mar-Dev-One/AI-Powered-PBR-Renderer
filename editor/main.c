// editor/main.c
//
// The editor: opens a project (Project/Project.h) and edits one of its
// scenes at a time, with the entity inspector, the gizmo, the command
// console, the command server other processes (MCP server, AI agent,
// scripts) drive it through, and the in-app chat. The game runtime -- the
// same engine with none of this -- is game/main.c.
//
// Projects: File > New Project makes a folder with project.json, scenes/,
// models/ and hdri/ (and a starter scene); File > Open Project / Recent
// switches to another. The open project is the project root (Core/Paths.h):
// every "models/..." and "hdri/..." in a scene, every scene.save / scene.load
// / assets.list -- the agent's too -- works inside it. Without --project the
// editor opens the sample project, the source tree's assets/.
//
// Scenes: File > New Scene / Open Scene / Save (Ctrl+S) / Save As. The
// title bar shows "*" while the scene has changes that aren't saved; opening
// something else, or quitting, asks first. Opening a scene starts a fresh
// undo history.
//
// Play (the Viewport toolbar) runs a copy of the scene in place; Export
// makes the game a player runs (game/main.c, Project/Project.h).

#include "App.h"
#include "Chat/ChatSession.h"
#include "Core/Defines.h"
#include "Core/Encoding.h"
#include "Core/FileIO.h"
#include "Core/Image.h"
#include "Core/Paths.h"
#include "Core/Process.h"
#include "Project/Project.h"
#include "RHI/RHI.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Camera.h"
#include "Scene/OrbitCamera.h"
#include "Scene/Model.h"
#include "Assets/AssetCache.h"
#include "World/Serialize.h"
#include "World/World.h"
#include "Platform/Window.h"
#include "Net/CommandServer.h"
#include "UI/ChatPanel.h"
#include "UI/CommandConsole.h"
#include "UI/EntityPanel.h"
#include "UI/Gizmo.h"
#include "UI/Inspector.h"
#include "UI/ImGuiLayer.h"
#include "cimgui.h"

#include "PlaySession.h"
#include "SceneDocument.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <GLFW/glfw3.h>

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

typedef struct editor_state
{
    App*   app;
    camera cam;

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

    command_console console;
    inspector       inspector;      // the selected entity's fields; edits run as undoable commands
    b8              show_console;   // F1 toggles
    b8              show_gpu_timings;
    f32             cpu_frame_ms;   // smoothed, for the GPU timings window

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
    // Renderer/SceneRenderer.h runs the whole lit frame (cascaded shadows,
    // HDR target, skybox, PBR uniforms, every MeshRenderer, tone mapping);
    // build_render_settings() turns the state below into its settings.
    scene_renderer renderer;
    b8             debug_show_cascades;   // tints shading by cascade index; see pbr.frag's u_debug_show_cascades
    scene_debug_view debug_view;          // lit, or one input to the shading (SceneRenderer.h)

    // --- Image-based lighting -------------------------------------------------
    // doc.env (World/Environment.h) is the single source of truth for the
    // HDRI, IBL / skybox switches, exposure and ambient strength: the GUI, the
    // environment.set / render.set commands and opening a scene all write it,
    // and saving stores it with the scene. `ibl` is doc.env.hdri baked
    // (Renderer/IBL.h), re-baked when it changes.
    ibl_cache ibl;

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

    // Mouse-orbit control of `cam` (Scene/OrbitCamera.h).
    orbit_camera orbit;

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
    // operation; the Scene panel has the local/world toggle and snapping
    // (Ctrl while dragging snaps too). Edits are component.set on Transform,
    // one undo step per drag, like the Inspector.
    gizmo_operation gizmo_op;
    gizmo_space     gizmo_space;
    b8              gizmo_snap;
    f32             snap_move;        // meters
    f32             snap_rotate;      // degrees
    f32             snap_scale;       // scale factor step
    b8              gizmo_hot;        // over or dragging a handle, as of last frame's draw
    b8              gizmo_editing;    // a history group is open for the drag in progress
    b8              gizmo_executing;  // inside one of the gizmo's own commands (not logged)

    // --- Export (the Export dialog) ----------------------------------------------------
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

    // --- Add (Scene panel) ----------------------------------------------------------
    f32 add_size;   // meters: the longest side of a model or shape added from the panel

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
} editor_state;

#define POPUP_UNSAVED      "Unsaved changes"
#define POPUP_SAVE_AS      "Save scene as"
#define POPUP_NEW_PROJECT  "New project"
#define POPUP_OPEN_PROJECT "Open project"
#define POPUP_IMPORT       "Import asset"
#define POPUP_SETTINGS     "Project settings"
#define POPUP_EXPORT       "Export game"

// The scene on screen and its commands: the edited scene, or its copy while
// playing.
static command_context*
active_commands(editor_state* state)
{
    return play_active_commands(&state->play, &state->doc);
}

static world*
active_world(editor_state* state)
{
    return active_commands(state)->w;
}

static void select_at(editor_state* state, f32 x, f32 y);

// The viewport's mouse: left-drag orbits, a short left click selects, the
// wheel zooms. Reads the item just submitted -- the invisible button
// covering the view -- so ImGui decides who owns the mouse: a press that
// starts on the view keeps orbiting even when the drag leaves it, and a
// press anywhere else never reaches here.
static void
view_input(editor_state* state)
{
    const ImGuiIO* io = igGetIO_Nil();

    if (igIsItemActivated())
    {
        state->press_x        = io->MousePos.x;
        state->press_y        = io->MousePos.y;
        state->press_on_gizmo = state->gizmo_hot;
    }

    if (igIsItemActive() && !state->press_on_gizmo && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f))
    {
        orbit_drag(&state->orbit, &state->cam, io->MouseDelta.x, io->MouseDelta.y);
    }

    // Released after barely moving: a click, not an orbit -- select.
    const f32 click_slop = 4.0f;   // pixels
    if (igIsItemDeactivated() && !state->press_on_gizmo && fabsf(io->MousePos.x - state->press_x) <= click_slop
        && fabsf(io->MousePos.y - state->press_y) <= click_slop)
        select_at(state, io->MousePos.x - state->view_x, io->MousePos.y - state->view_y);

    if (igIsItemHovered(0) && io->MouseWheel != 0.0f)
        orbit_zoom(&state->orbit, &state->cam, io->MouseWheel);
}

// --- HDRI list ------------------------------------------------------------------
// The project's .hdr files, as scene paths ("hdri/sky.hdr", "hdri/night/moon.hdr").

typedef struct hdri_list
{
    char   paths[HDRI_LIST_MAX][ENVIRONMENT_HDRI_MAX];
    uint32 count;
} hdri_list;

typedef struct hdri_visit
{
    hdri_list*  list;
    const char* prefix;   // "hdri/" or "hdri/<folder>/"
    b8          nested;
} hdri_visit;

static void
visit_hdri(const char* name, b8 is_directory, void* user_data)
{
    hdri_visit* visit  = user_data;
    size_t      length = strlen(name);

    if (is_directory && !visit->nested)
    {
        char prefix[ENVIRONMENT_HDRI_MAX], relative[ENVIRONMENT_HDRI_MAX];
        snprintf(prefix, sizeof(prefix), "%s%s/", visit->prefix, name);
        snprintf(relative, sizeof(relative), "%s%s", visit->prefix, name);

        char*      folder = project_path(relative);
        hdri_visit inner  = { visit->list, prefix, true };
        file_list_directory(folder, visit_hdri, &inner);
        free(folder);
    }
    else if (!is_directory && length > 4 && strcmp(name + length - 4, ".hdr") == 0 && visit->list->count < HDRI_LIST_MAX)
    {
        snprintf(visit->list->paths[visit->list->count++], ENVIRONMENT_HDRI_MAX, "%s%s", visit->prefix, name);
    }
}

static void
list_hdris(hdri_list* out)
{
    out->count = 0;
    char*      folder = project_path("hdri");
    hdri_visit visit  = { out, "hdri/", false };
    file_list_directory(folder, visit_hdri, &visit);
    free(folder);
}

// --- Application commands ------------------------------------------------------
// Commands for state the World doesn't hold -- the camera, the HDRI, the
// renderer settings and the project -- registered with the command context,
// so the console (and the agent) can drive them like any built-in command.

static scene_render_settings build_render_settings(const editor_state* state);

// on_change: show the change in the console, and push it to connected
// clients.
static void
on_world_change(void* user_data, const cJSON* event)
{
    editor_state* state = user_data;

    // The inspector's own edits show in its fields as they happen; logging
    // each frame of a drag would flood the console. Clients still hear them.
    if (!state->inspector.executing && !state->gizmo_executing)
        console_on_change(&state->console, event);

    if (state->server)
    {
        cJSON* params = cJSON_Duplicate(event, true);
        cJSON_AddStringToObject(params, "world", play_active(&state->play) ? "play" : "scene");
        server_notify(state->server, SERVER_CHANGE_NOTIFICATION, params);
        cJSON_Delete(params);
    }
}

// f32 -> JSON number at 4 significant digits, so 1.3f reads 1.3 rather
// than 1.2999999523162842.
static cJSON*
number_json(f32 value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.4g", (double)value);
    return cJSON_CreateNumber(strtod(buffer, NULL));
}

static cJSON*
vec3_json(const f32* v)
{
    cJSON* a = cJSON_CreateArray();
    for (int i = 0; i < 3; ++i)
        cJSON_AddItemToArray(a, number_json(v[i]));
    return a;
}

static b8
json_vec3(const cJSON* item, vec3 out)
{
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 3)
        return false;
    for (int i = 0; i < 3; ++i)
    {
        const cJSON* n = cJSON_GetArrayItem(item, i);
        if (!cJSON_IsNumber(n))
            return false;
        out[i] = (f32)n->valuedouble;
    }
    return true;
}

static b8
host_environment_list(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;

    hdri_list hdris;
    list_hdris(&hdris);

    *out_result = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(*out_result, "hdris");
    for (uint32 i = 0; i < hdris.count; ++i)
        cJSON_AddItemToArray(list, cJSON_CreateString(hdris.paths[i]));

    if (state->doc.env.hdri[0])
        cJSON_AddStringToObject(*out_result, "current", state->doc.env.hdri);
    else
        cJSON_AddNullToObject(*out_result, "current");
    cJSON_AddBoolToObject(*out_result, "hdri_loaded", ibl_cache_maps(&state->ibl) != NULL);
    cJSON_AddBoolToObject(*out_result, "use_ibl", state->doc.env.use_ibl);
    cJSON_AddBoolToObject(*out_result, "show_skybox", state->doc.env.show_skybox);
    cJSON_AddItemToObject(*out_result, "sky_blur", number_json(state->doc.env.sky_blur));
    return true;
}

static b8
host_environment_set(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state = user_data;

    // A path from environment.list, or just its file name.
    const cJSON* hdri  = cJSON_GetObjectItem(args, "hdri");
    const char*  found = NULL;
    hdri_list    hdris;
    if (cJSON_IsString(hdri))
    {
        list_hdris(&hdris);
        for (uint32 i = 0; i < hdris.count && !found; ++i)
        {
            const char* slash = strrchr(hdris.paths[i], '/');
            if (strcmp(hdris.paths[i], hdri->valuestring) == 0 || (slash && strcmp(slash + 1, hdri->valuestring) == 0))
                found = hdris.paths[i];
        }
        if (!found)
        {
            snprintf(error, error_size, "unknown HDRI \"%s\"; environment.list shows the project's HDRIs", hdri->valuestring);
            return false;
        }
    }
    else if (hdri && !cJSON_IsNull(hdri))
    {
        snprintf(error, error_size, "\"hdri\" must be a path from environment.list");
        return false;
    }

    const cJSON* blur = cJSON_GetObjectItem(args, "sky_blur");
    if (blur && (!cJSON_IsNumber(blur) || blur->valuedouble < 0.0 || blur->valuedouble > 1.0))
    {
        snprintf(error, error_size, "\"sky_blur\" must be a number from 0 (sharp) to 1 (blurriest)");
        return false;
    }

    if (found)
    {
        snprintf(state->doc.env.hdri, sizeof(state->doc.env.hdri), "%s", found);
        ibl_cache_sync(&state->ibl, state->doc.env.hdri);
    }

    const cJSON* use_ibl = cJSON_GetObjectItem(args, "use_ibl");
    const cJSON* skybox  = cJSON_GetObjectItem(args, "show_skybox");
    if (cJSON_IsBool(use_ibl)) state->doc.env.use_ibl     = cJSON_IsTrue(use_ibl);
    if (cJSON_IsBool(skybox))  state->doc.env.show_skybox = cJSON_IsTrue(skybox);
    if (blur)                  state->doc.env.sky_blur    = (f32)blur->valuedouble;

    return host_environment_list(user_data, ctx, NULL, out_result, error, error_size);
}

static b8
host_view_get(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;

    *out_result = cJSON_CreateObject();
    cJSON_AddItemToObject(*out_result, "position", vec3_json(state->cam.position));
    cJSON_AddItemToObject(*out_result, "target", vec3_json(state->cam.target));
    cJSON_AddItemToObject(*out_result, "fov_degrees", number_json(state->cam.fov_degrees));
    cJSON_AddItemToObject(*out_result, "exposure", number_json(state->doc.env.exposure));
    cJSON_AddItemToObject(*out_result, "ambient_strength", number_json(state->doc.env.ambient_strength));
    cJSON_AddStringToObject(*out_result, "debug_view", scene_debug_view_name(state->debug_view));
    return true;
}

static b8
host_view_set_camera(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state = user_data;

    vec3 position, target;
    glm_vec3_copy(state->cam.position, position);
    glm_vec3_copy(state->cam.target, target);

    const cJSON* p = cJSON_GetObjectItem(args, "position");
    const cJSON* t = cJSON_GetObjectItem(args, "target");
    if ((p && !json_vec3(p, position)) || (t && !json_vec3(t, target)))
    {
        snprintf(error, error_size, "\"position\" and \"target\" must be [x, y, z] in meters");
        return false;
    }
    if (!p && !t)
    {
        snprintf(error, error_size, "pass \"position\" and/or \"target\"");
        return false;
    }
    if (glm_vec3_distance(position, target) < 0.01f)
    {
        snprintf(error, error_size, "the camera can't be at the point it looks at");
        return false;
    }

    glm_vec3_copy(position, state->cam.position);
    glm_vec3_copy(target, state->cam.target);
    orbit_from_camera(&state->orbit, &state->cam);
    orbit_apply(&state->orbit, &state->cam);
    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

// Points the camera at an entity (or the whole scene), keeping the current
// viewing angle, and backs off until it fits.
static b8
host_view_frame(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state = user_data;
    const world*  w     = ctx->w;

    vec3 bmin = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    b8   any  = false;

    const cJSON* target = cJSON_GetObjectItem(args, "entity");
    if (target && !cJSON_IsNull(target))
    {
        entity_id e = cJSON_IsString(target) ? world_find(w, target->valuestring) : ENTITY_NULL;
        if (!e)
        {
            snprintf(error, error_size, "no entity named \"%s\"", cJSON_IsString(target) ? target->valuestring : "?");
            return false;
        }
        any = world_bounds(w, e, bmin, bmax);
        if (!any)
        {
            const vec4* m = world_matrix(w, e);
            vec3 c = { m[3][0], m[3][1], m[3][2] };
            glm_vec3_sub(c, (vec3){ 0.5f, 0.5f, 0.5f }, bmin);
            glm_vec3_add(c, (vec3){ 0.5f, 0.5f, 0.5f }, bmax);
            any = true;
        }
    }
    else
    {
        // Everything visible, framed as the game's camera frames it.
        any = world_framing_bounds(w, bmin, bmax);
    }

    if (!any)
    {
        snprintf(error, error_size, "nothing to frame");
        return false;
    }

    orbit_fit(&state->orbit, &state->cam, bmin, bmax, 1.1f);
    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

static b8
host_render_set(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state = user_data;

    const cJSON* exposure = cJSON_GetObjectItem(args, "exposure");
    const cJSON* ambient  = cJSON_GetObjectItem(args, "ambient_strength");
    const cJSON* cascades = cJSON_GetObjectItem(args, "debug_show_cascades");
    const cJSON* view     = cJSON_GetObjectItem(args, "debug_view");

    if ((exposure && (!cJSON_IsNumber(exposure) || exposure->valuedouble <= 0.0 || exposure->valuedouble > 16.0))
        || (ambient && (!cJSON_IsNumber(ambient) || ambient->valuedouble < 0.0 || ambient->valuedouble > 4.0))
        || (cascades && !cJSON_IsBool(cascades)))
    {
        snprintf(error, error_size, "exposure must be in (0, 16], ambient_strength in [0, 4], debug_show_cascades true/false");
        return false;
    }

    scene_debug_view debug_view = state->debug_view;
    if (view && !scene_debug_view_from_name(cJSON_GetStringValue(view), &debug_view))
    {
        snprintf(error, error_size, "debug_view must be one of: lit, albedo, normals, roughness, metallic, ao, shadow, "
                                    "lighting, wireframe");
        return false;
    }

    if (exposure) state->doc.env.exposure         = (f32)exposure->valuedouble;
    if (ambient)  state->doc.env.ambient_strength = (f32)ambient->valuedouble;
    if (cascades) state->debug_show_cascades = cJSON_IsTrue(cascades);
    state->debug_view = debug_view;

    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

// How long the last frames took: each render pass on the GPU (smoothed
// over about 20 frames) and the whole frame on the CPU.
static b8
host_render_timings(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state*       state    = user_data;
    const gpu_profiler* profiler = &state->renderer.profiler;

    *out_result = cJSON_CreateObject();
    cJSON* passes = cJSON_AddArrayToObject(*out_result, "gpu_passes");
    for (uint32 i = 0; i < profiler->timing_count; ++i)
    {
        cJSON* pass = cJSON_CreateObject();
        cJSON_AddStringToObject(pass, "name", profiler->timings[i].name);
        cJSON_AddItemToObject(pass, "ms", number_json(profiler->timings[i].ms));
        cJSON_AddItemToArray(passes, pass);
    }
    cJSON_AddItemToObject(*out_result, "gpu_frame_ms", number_json(profiler->frame_ms));
    cJSON_AddItemToObject(*out_result, "cpu_frame_ms", number_json(state->cpu_frame_ms));
    return true;
}

// What the scene looks like right now, as a PNG -- rendered fresh,
// without the editor's panels, so a model can see its own work.
static b8
host_view_screenshot(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state = user_data;

    // Longest side in pixels. Smaller keeps the payload (and an LLM's
    // context) small; 768 is plenty to judge composition and lighting.
    int          max_size = 768;
    const cJSON* size     = cJSON_GetObjectItem(args, "max_size");
    if (size)
    {
        if (!cJSON_IsNumber(size) || size->valuedouble < 64 || size->valuedouble > 4096)
        {
            snprintf(error, error_size, "\"max_size\" must be a whole number of pixels from 64 to 4096");
            return false;
        }
        max_size = (int)size->valuedouble;
    }

    scene_render_settings settings = build_render_settings(state);

    world_update(ctx->w);

    uint16 width, height;
    uint8* rgba = scene_renderer_capture(&state->renderer, ctx->w, &state->cam, &settings, &width, &height);
    if (!rgba)
    {
        snprintf(error, error_size, "nothing to capture (is the window minimized?)");
        return false;
    }

    // Fit within max_size, keeping the aspect ratio.
    uint16 out_w = width, out_h = height;
    int    longest = width > height ? width : height;
    if (longest > max_size)
    {
        out_w = (uint16)glm_max(1.0f, roundf((f32)width  * (f32)max_size / (f32)longest));
        out_h = (uint16)glm_max(1.0f, roundf((f32)height * (f32)max_size / (f32)longest));
    }

    uint8* scaled = rgba;
    if (out_w != width || out_h != height)
    {
        scaled = malloc((size_t)out_w * out_h * 4);
        if (!scaled)
            FATAL("screenshot: out of memory");
        image_resize_box(rgba, width, height, 4, scaled, out_w, out_h);
    }

    // Alpha is always 1 here; RGB makes a smaller PNG.
    uint8* rgb = malloc((size_t)out_w * out_h * 3);
    if (!rgb)
        FATAL("screenshot: out of memory");
    for (size_t i = 0; i < (size_t)out_w * out_h; ++i)
    {
        rgb[i * 3 + 0] = scaled[i * 4 + 0];
        rgb[i * 3 + 1] = scaled[i * 4 + 1];
        rgb[i * 3 + 2] = scaled[i * 4 + 2];
    }

    size_t png_size;
    uint8* png = image_encode_png(rgb, out_w, out_h, 3, &png_size);

    if (scaled != rgba)
        free(scaled);
    free(rgba);
    free(rgb);

    if (!png)
    {
        snprintf(error, error_size, "PNG encoding failed");
        return false;
    }

    char* data = base64_encode_alloc(png, png_size);
    free(png);

    *out_result = cJSON_CreateObject();
    cJSON_AddStringToObject(*out_result, "format", "png");
    cJSON_AddStringToObject(*out_result, "encoding", "base64");
    cJSON_AddNumberToObject(*out_result, "width", out_w);
    cJSON_AddNumberToObject(*out_result, "height", out_h);
    cJSON_AddNumberToObject(*out_result, "bytes", (double)png_size);
    cJSON_AddStringToObject(*out_result, "data", data);
    free(data);
    return true;
}


static b8
host_project_info(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;

    *out_result = cJSON_CreateObject();
    cJSON_AddStringToObject(*out_result, "name", state->project.name);
    cJSON_AddStringToObject(*out_result, "folder", paths_project_root());
    cJSON_AddStringToObject(*out_result, "startup_scene", state->project.startup_scene);
    if (state->doc.name[0])
        cJSON_AddStringToObject(*out_result, "scene", state->doc.name);
    else
        cJSON_AddNullToObject(*out_result, "scene");
    cJSON_AddBoolToObject(*out_result, "unsaved_changes", doc_dirty(&state->doc));
    return true;
}

// The folder new projects go in unless told otherwise: Documents/PBR Projects.
static void
default_projects_location(char* out, size_t size)
{
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    snprintf(out, size, "%s/Documents/PBR Projects", home && home[0] ? home : ".");
    for (char* c = out; *c; ++c)
        if (*c == '\\')
            *c = '/';
}

// Creates a project; the user opens it (switching projects mid-turn would
// pull the scene out from under the agent).
static b8
host_project_create(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)user_data;

    const char* name   = cJSON_GetStringValue(cJSON_GetObjectItem(args, "name"));
    const char* folder = cJSON_GetStringValue(cJSON_GetObjectItem(args, "folder"));

    char location[PROJECT_PATH_MAX], directory[PROJECT_PATH_MAX * 2];
    if (folder && folder[0])
        snprintf(location, sizeof(location), "%s", folder);
    else
        default_projects_location(location, sizeof(location));
    snprintf(directory, sizeof(directory), "%s/%s", location, name ? name : "");

    if (!project_create(directory, name, error, error_size))
        return false;

    *out_result = cJSON_CreateObject();
    cJSON_AddStringToObject(*out_result, "folder", directory);
    cJSON_AddStringToObject(*out_result, "name", name);
    cJSON_AddStringToObject(*out_result, "note", "Created. The user opens it from File > Open Project in the editor.");
    return true;
}

static void play_start(editor_state* state);
static void play_stop(editor_state* state);

static b8   run_export(editor_state* state, const char* folder);
static void default_export_location(const editor_state* state, char* out, size_t size);

static b8
host_export_status(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;

    *out_result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*out_result, "exported", state->export_ok);
    cJSON_AddStringToObject(*out_result, "message", state->export_status);
    if (state->export_ok)
    {
        cJSON_AddStringToObject(*out_result, "folder", state->export_dir);
        cJSON_AddStringToObject(*out_result, "executable", state->export_report.executable);
        cJSON_AddNumberToObject(*out_result, "files", state->export_report.files);
        cJSON_AddNumberToObject(*out_result, "bytes", (double)state->export_report.bytes);
    }
    // The exported game's own check of its startup scene.
    static const char* const CHECK_NAMES[] = { "none", "running", "passed", "failed" };
    cJSON_AddStringToObject(*out_result, "check", CHECK_NAMES[state->check]);
    return true;
}

// The Export dialog's Export, for scripts and the agent.
static b8
host_export(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state  = user_data;
    const char*   folder = cJSON_GetStringValue(cJSON_GetObjectItem(args, "folder"));

    if (folder && folder[0])
        snprintf(state->export_dir, sizeof(state->export_dir), "%s", folder);
    else
        default_export_location(state, state->export_dir, sizeof(state->export_dir));

    if (!run_export(state, state->export_dir))
    {
        snprintf(error, error_size, "%s", state->export_status);
        return false;
    }
    return host_export_status(user_data, ctx, NULL, out_result, error, error_size);
}

static const cmd_arg ARGS_EXPORT[] = {
    { "folder", "string", false, "Where to put the game: a new or empty folder, or an earlier export; "
                                 "default Documents/PBR Exports/<project name>" },
};

static b8
host_play_status(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;

    *out_result = cJSON_CreateObject();
    cJSON_AddStringToObject(*out_result, "state", play_mode_name(state->play.mode));
    cJSON_AddItemToObject(*out_result, "time", number_json(state->play.time));
    return true;
}

// The Viewport toolbar's buttons, for scripts and the agent.
static b8
host_play_set(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state  = user_data;
    const char*   action = cJSON_GetStringValue(cJSON_GetObjectItem(args, "action"));

    if (action && strcmp(action, "play") == 0)
    {
        play_start(state);
        if (!play_active(&state->play))
        {
            snprintf(error, error_size, "can't play while a chat turn is running");
            return false;
        }
    }
    else if (action && strcmp(action, "stop") == 0)
        play_stop(state);
    else if (action && (strcmp(action, "pause") == 0 || strcmp(action, "resume") == 0))
    {
        b8 pause = action[0] == 'p';
        if ((pause && state->play.mode == PLAY_RUNNING) || (!pause && state->play.mode == PLAY_PAUSED))
            play_toggle_pause(&state->play);
    }
    else if (action && strcmp(action, "step") == 0)
    {
        if (state->play.mode != PLAY_PAUSED)
        {
            snprintf(error, error_size, "step works while paused; pause first");
            return false;
        }
        state->play.step = true;
    }
    else
    {
        snprintf(error, error_size, "\"action\" must be \"play\", \"pause\", \"resume\", \"step\" or \"stop\"");
        return false;
    }
    return host_play_status(user_data, ctx, NULL, out_result, error, error_size);
}

static const cmd_arg ARGS_PLAY_SET[] = {
    { "action", "string", true, "\"play\" (run a copy of the scene), \"pause\", \"resume\", \"step\" (one frame, while "
                                "paused) or \"stop\" (drop the copy, back to the edited scene)" },
};

static const cmd_arg ARGS_SCREENSHOT[] = {
    { "max_size", "number", false, "Longest side in pixels, 64-4096 (default 768); the window's size if smaller" },
};

static const cmd_arg ARGS_ENVIRONMENT_SET[] = {
    { "hdri",        "string",  false, "HDRI path from environment.list" },
    { "use_ibl",     "boolean", false, "Light the scene from the HDRI (false = simple sky/ground ambient)" },
    { "show_skybox", "boolean", false, "Show the HDRI as the background" },
    { "sky_blur",    "number",  false, "Background blur, 0 (sharp) to 1" },
};

static const cmd_arg ARGS_SET_CAMERA[] = {
    { "position", "vec3", false, "Camera position [x, y, z] in meters" },
    { "target",   "vec3", false, "Point the camera looks at" },
};

static const cmd_arg ARGS_FRAME[] = {
    { "entity", "entity", false, "Entity to frame; omit to frame everything visible" },
};

static const cmd_arg ARGS_RENDER_SET[] = {
    { "exposure",            "number",  false, "Brightness multiplier before tone mapping, e.g. 1" },
    { "ambient_strength",    "number",  false, "Strength of the image-based / ambient light, e.g. 0.5" },
    { "debug_show_cascades", "boolean", false, "Tint shading by shadow cascade" },
    { "debug_view",          "string",  false, "What the view shows: lit (normal), or one input to the shading -- albedo, "
                                                "normals, roughness, metallic, ao, shadow, lighting (white surfaces), "
                                                "wireframe" },
};

static const cmd_arg ARGS_PROJECT_CREATE[] = {
    { "name",   "string", true,  "Project name, also its folder's name (none of \\ / : * ? \" < > |)" },
    { "folder", "string", false, "Folder to create it in; default Documents/PBR Projects" },
};

#define HOST_ARGS(array) array, (uint32)(sizeof(array) / sizeof((array)[0]))

static const cmd_host_command HOST_COMMANDS[] = {
    { "environment.list", "The project's HDRIs that can light the scene, and the current settings", NULL, 0, false, host_environment_list },
    { "environment.set",  "Changes the HDRI and how it lights and backs the scene",         HOST_ARGS(ARGS_ENVIRONMENT_SET), false, host_environment_set },
    { "view.get",         "Where the camera is and what it looks at",                       NULL, 0, false, host_view_get },
    { "view.set_camera",  "Moves the camera and/or what it looks at",                       HOST_ARGS(ARGS_SET_CAMERA),      false, host_view_set_camera },
    { "view.frame",       "Points the camera at an entity (or everything) so it fills the view", HOST_ARGS(ARGS_FRAME),     false, host_view_frame },
    { "render.set",       "Exposure, ambient strength and debug views",                      HOST_ARGS(ARGS_RENDER_SET),      false, host_render_set },
    { "render.timings",   "How long each render pass takes on the GPU, and the frame on the CPU, in ms",
                                                                                               NULL, 0,                         false, host_render_timings },
    { "view.screenshot",  "The scene as a PNG (base64), rendered now without the editor's panels -- to check how it looks",
                                                                                               HOST_ARGS(ARGS_SCREENSHOT),      false, host_view_screenshot },
    { "project.info",     "The open project: name, folder, startup scene, the scene being edited and whether it has unsaved changes",
                                                                                               NULL, 0,                         false, host_project_info },
    { "project.create",   "Creates a new project folder with a starter scene (the user opens it in the editor)",
                                                                                               HOST_ARGS(ARGS_PROJECT_CREATE),  false, host_project_create },
    { "play.set",         "Play mode: runs a throwaway copy of the scene (behaviours like Spin move); pause, step, stop",
                                                                                               HOST_ARGS(ARGS_PLAY_SET),        false, host_play_set },
    { "play.status",      "Whether the scene is playing, paused or stopped, and for how long it has played",
                                                                                               NULL, 0,                         false, host_play_status },
    { "project.export",   "Exports the project's saved scenes as a game a player can run, then has the game check itself",
                                                                                               HOST_ARGS(ARGS_EXPORT),          false, host_export },
    { "project.export_status", "The last export: where, how big, and whether the game's own check passed",
                                                                                               NULL, 0,                         false, host_export_status },
};

#define HOST_COMMAND_COUNT ((uint32)(sizeof(HOST_COMMANDS) / sizeof(HOST_COMMANDS[0])))

// --- Chat agent sidecar -----------------------------------------------------------
// The agent runs as "python -m pbr_agent.agent --exit-with-app" with the
// Python from the agent's virtual environment (agent/README.md), or the one
// PBR_AGENT_PYTHON names. It reads its API key from the environment it
// inherits from the app, and logs to ~/.pbr-renderer/agent.log. AGENT_DIR
// (set by CMake) is agent/ in the source tree -- a development tool, not
// part of any project.

// "<directory>/<relative>", heap-allocated; caller frees.
static char*
join_path(const char* directory, const char* relative)
{
    size_t length = strlen(directory) + 1 + strlen(relative) + 1;
    char*  path   = malloc(length);
    if (!path)
        FATAL("editor: out of memory");
    snprintf(path, length, "%s/%s", directory, relative);
    return path;
}

static void
start_agent(editor_state* state)
{
    if (state->agent_process && process_running(state->agent_process, NULL))
        return;
    process_destroy(state->agent_process);
    state->agent_process = NULL;

    char*       python;
    const char* configured = getenv("PBR_AGENT_PYTHON");
    if (configured && configured[0])
    {
        size_t length = strlen(configured) + 1;
        python = malloc(length);
        if (!python)
            FATAL("editor: out of memory");
        memcpy(python, configured, length);
    }
    else
    {
#ifdef _WIN32
        python = join_path(AGENT_DIR, ".venv/Scripts/python.exe");
#else
        python = join_path(AGENT_DIR, ".venv/bin/python");
#endif
    }

    if (!file_exists(python))
    {
        snprintf(state->agent_note, sizeof(state->agent_note),
                 "No agent environment at %s. Set it up as agent/README.md describes, or set PBR_AGENT_PYTHON.", python);
        free(python);
        return;
    }

    const char* argv[] = { python, "-m", "pbr_agent.agent", "--exit-with-app", NULL };
    state->agent_process = process_spawn(argv, state->agent_log);
    if (state->agent_process)
        snprintf(state->agent_note, sizeof(state->agent_note), "Starting the agent...");
    else
        snprintf(state->agent_note, sizeof(state->agent_note), "Could not start %s.", python);
    free(python);
}

// Notices the sidecar exiting, and says where to look.
static void
watch_agent(editor_state* state)
{
    int exit_code;
    if (!state->agent_process || process_running(state->agent_process, &exit_code))
        return;

    snprintf(state->agent_note, sizeof(state->agent_note), "The agent exited (code %d); its log is %s.",
             exit_code, state->agent_log);
    process_destroy(state->agent_process);
    state->agent_process = NULL;
}

// --- Selection -------------------------------------------------------------------

// Clears the selection if it no longer names a live entity (deleted, another
// scene opened).
static void
validate_selection(editor_state* state)
{
    if (!world_is_alive(active_world(state), state->selected))
        state->selected = ENTITY_NULL;
}

// A click in the view at (x, y) pixels from its top-left corner: selects
// what's under it, or clears the selection on empty space.
static void
select_at(editor_state* state, f32 x, f32 y)
{
    vec3 origin, direction;
    camera_screen_ray(&state->cam, x, y, state->view_width, state->view_height, origin, direction);

    // Lights and cameras have nothing to click on: a 25 cm sphere stands in.
    state->selected         = world_pick(active_world(state), origin, direction, 0.25f, NULL);
    state->reveal_selection = state->selected != ENTITY_NULL;
}

// What to outline for `e`: its own bounds, else the bounds of everything
// parented under it (a pivot), else false.
static b8
selection_bounds(const world* w, entity_id e, vec3 lo, vec3 hi)
{
    if (world_bounds(w, e, lo, hi))
        return true;

    b8 any = false;
    glm_vec3_fill(lo, FLT_MAX);
    glm_vec3_fill(hi, -FLT_MAX);
    for (entity_id child = world_next(w, ENTITY_NULL, COMPONENT_BIT(COMPONENT_MESH_RENDERER)); child;
         child = world_next(w, child, COMPONENT_BIT(COMPONENT_MESH_RENDERER)))
    {
        b8 under = false;
        for (entity_id p = world_parent(w, child); p && !under; p = world_parent(w, p))
            under = p == e;

        vec3 child_lo, child_hi;
        if (under && world_bounds(w, child, child_lo, child_hi))
        {
            glm_vec3_minv(lo, child_lo, lo);
            glm_vec3_maxv(hi, child_hi, hi);
            any = true;
        }
    }
    return any;
}

// Outlines the selection over the view: its bounding box (or a ring at its
// position for a light, camera or empty), with its name, into `draw` (the
// Viewport window's list, so it stays inside the panel). Call after the
// world was drawn, with the same pose (inside a Spin preview).
static void
draw_selection(editor_state* state, ImDrawList* draw)
{
    const world* w = active_world(state);
    if (!world_is_alive(w, state->selected))
        return;

    const ImVec2_c origin = { state->view_x, state->view_y };
    const f32      width  = state->view_width;
    const f32      height = state->view_height;
    const ImU32    color  = 0xFF00AAFF;   // orange (ABGR)

    entity_id e = state->selected;
    f32 label_x = 0.0f, label_y = FLT_MAX;
    b8  labelled = false;

    vec3 lo, hi;
    if (selection_bounds(w, e, lo, hi))
    {
        // 8 corners (bit 0 = x, 1 = y, 2 = z) and the 12 edges between them.
        f32 sx[8], sy[8];
        b8  visible[8];
        for (int i = 0; i < 8; ++i)
        {
            vec3 corner = { (i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2] };
            visible[i] = camera_world_to_screen(&state->cam, corner, width, height, &sx[i], &sy[i]);
            if (visible[i] && sy[i] < label_y)
            {
                label_x  = sx[i];
                label_y  = sy[i];
                labelled = true;
            }
        }

        static const int EDGES[12][2] = {
            { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },   // along x
            { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },   // along y
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },   // along z
        };
        for (int i = 0; i < 12; ++i)
        {
            int a = EDGES[i][0], b = EDGES[i][1];
            if (visible[a] && visible[b])
                ImDrawList_AddLine(draw, (ImVec2_c){ origin.x + sx[a], origin.y + sy[a] },
                                   (ImVec2_c){ origin.x + sx[b], origin.y + sy[b] }, color, 2.0f);
        }
    }
    else
    {
        const vec4* m = world_matrix(w, e);
        f32 x, y;
        if (m && camera_world_to_screen(&state->cam, (vec3){ m[3][0], m[3][1], m[3][2] }, width, height, &x, &y))
        {
            ImDrawList_AddCircle(draw, (ImVec2_c){ origin.x + x, origin.y + y }, 12.0f, color, 24, 2.0f);
            label_x  = x;
            label_y  = y - 12.0f;
            labelled = true;
        }
    }

    if (labelled)
    {
        const char* name = world_name(w, e);
        ImDrawList_AddText_Vec2(draw, (ImVec2_c){ origin.x + label_x + 4.0f, origin.y + label_y - 18.0f },
                                color, name, NULL);
    }
}

// Points the camera at `entity` (NULL = everything visible), as view.frame
// does; says why in the console if it can't frame an entity.
static void
frame_view(editor_state* state, const char* entity)
{
    cJSON* args = cJSON_CreateObject();
    if (entity)
        cJSON_AddStringToObject(args, "entity", entity);

    cJSON* result = NULL;
    char   error[256];
    if (!host_view_frame(state, active_commands(state), args, &result, error, sizeof(error)) && entity)
        console_info(&state->console, error);
    cJSON_Delete(result);
    cJSON_Delete(args);
}

// F: points the camera at the selection.
static void
frame_selection(editor_state* state)
{
    validate_selection(state);
    if (state->selected)
        frame_view(state, world_name(active_world(state), state->selected));
}

// --- Gizmo --------------------------------------------------------------------------

// Runs {cmd, args} for the gizmo (takes `args`); false and a console line on failure.
static b8
gizmo_run(editor_state* state, command_context* ctx, const char* cmd, cJSON* args)
{
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", cmd);
    cJSON_AddItemToObject(request, "args", args ? args : cJSON_CreateObject());

    state->gizmo_executing = true;
    cJSON* response = cmd_execute(ctx, request);
    state->gizmo_executing = false;

    b8 ok = cJSON_IsTrue(cJSON_GetObjectItem(response, "ok"));
    if (!ok)
    {
        const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "message"));
        console_info(&state->console, message ? message : "gizmo: edit failed");
    }
    cJSON_Delete(response);
    cJSON_Delete(request);
    return ok;
}

// Ends the drag's history group: the whole drag becomes one undo step.
static void
gizmo_finish_edit(editor_state* state, command_context* ctx)
{
    if (!state->gizmo_editing)
        return;
    state->gizmo_editing = false;
    gizmo_run(state, ctx, "history.commit", NULL);
}

static cJSON*
vec3_array(const f32* v)
{
    return cJSON_CreateFloatArray(v, 3);
}

// Draws the handles on the selection and turns a drag into Transform edits.
// The gizmo works on the world matrix; the edit is the Transform that gives
// it: local = inverse(parent world) * new world, decomposed back into
// position / rotation_deg / scale (World/World.h transform_from_matrix()).
// Drawn into `draw`, the Viewport window's list; `view_hovered`: the mouse
// is over the view; `view_item`: the ID of the view's input button, which
// mustn't keep the handles from taking a click (UI/Gizmo.h).
static void
draw_gizmo(editor_state* state, ImDrawList* draw, b8 view_hovered, ImGuiID view_item)
{
    command_context* ctx = active_commands(state);
    world*           w   = ctx->w;

    if (!world_is_alive(w, state->selected))
    {
        state->gizmo_hot = false;
        gizmo_finish_edit(state, ctx);
        return;
    }
    entity_id e = state->selected;

    mat4 view, projection, model;
    camera_get_view(&state->cam, view);
    camera_get_projection(&state->cam, projection);
    glm_mat4_copy((vec4*)world_matrix(w, e), model);

    const f32  snap_values[] = { state->snap_move, state->snap_rotate, state->snap_scale };
    const b8   snapping      = state->gizmo_snap != igGetIO_Nil()->KeyCtrl;   // Ctrl flips it while dragging
    const f32* snap          = snapping ? &snap_values[state->gizmo_op] : NULL;

    // Live only while the mouse is over the view (or mid-drag); otherwise
    // drawn but deaf, so a click on a panel in front never grabs a handle.
    const b8 interactive = view_hovered || gizmo_is_using();

    b8 changed = gizmo_manipulate((const f32*)view, (const f32*)projection, state->gizmo_op, state->gizmo_space,
                                  (f32*)model, snap, state->view_x, state->view_y, state->view_width,
                                  state->view_height, draw, interactive, view_item);
    state->gizmo_hot = gizmo_is_over() || gizmo_is_using();

    if (changed)
    {
        // Back to the parent's space.
        entity_id parent = world_parent(w, e);
        mat4      local;
        if (parent)
        {
            mat4 parent_inverse;
            glm_mat4_inv((vec4*)world_matrix(w, parent), parent_inverse);
            glm_mat4_mul(parent_inverse, model, local);
        }
        else
        {
            glm_mat4_copy(model, local);
        }

        c_transform t;
        transform_from_matrix(local, &t);
        for (int axis = 0; axis < 3; ++axis)
            t.scale[axis] = glm_max(t.scale[axis], 0.0001f);   // Transform's smallest scale

        static const char* const VERBS[] = { "move", "rotate", "scale" };
        if (!state->gizmo_editing && ctx->history)
        {
            char   label[ENTITY_NAME_MAX + 16];
            cJSON* args = cJSON_CreateObject();
            snprintf(label, sizeof(label), "%s %s", VERBS[state->gizmo_op], world_name(w, e));
            cJSON_AddStringToObject(args, "label", label);
            state->gizmo_editing = gizmo_run(state, ctx, "history.begin", args);
        }

        // Only what this operation changes, so a move never nudges the
        // rotation by a decomposition rounding error.
        cJSON* fields = cJSON_CreateObject();
        if (state->gizmo_op == GIZMO_TRANSLATE)
            cJSON_AddItemToObject(fields, "position", vec3_array(t.position));
        else if (state->gizmo_op == GIZMO_ROTATE)
            cJSON_AddItemToObject(fields, "rotation_deg", vec3_array(t.rotation_deg));
        else
            cJSON_AddItemToObject(fields, "scale", vec3_array(t.scale));

        cJSON* args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "entity", world_name(w, e));
        cJSON_AddStringToObject(args, "component", "Transform");
        cJSON_AddItemToObject(args, "fields", fields);
        gizmo_run(state, ctx, "component.set", args);
        world_update(w);   // this frame's outline and panels see the new pose
    }

    if (state->gizmo_editing && !gizmo_is_using())
        gizmo_finish_edit(state, ctx);
}

// --- Play mode ----------------------------------------------------------------------

// Copies the scene and starts ticking the copy. Not while a chat turn runs:
// its transaction is open on the edited scene, and its remaining tool calls
// would land in the copy.
static void
play_start(editor_state* state)
{
    if (play_active(&state->play))
        return;
    if (chat_busy(&state->chat))
    {
        console_info(&state->console, "Play: wait for the chat turn to finish (or Stop it) first.");
        return;
    }

    inspector_finish_edit(&state->inspector, &state->doc.commands);
    gizmo_finish_edit(state, &state->doc.commands);

    play_begin(&state->play, &state->doc);   // same IDs in the copy: the selection carries over

    console_info(&state->console, "Playing. Changes made now are discarded on Stop (Ctrl+P).");
}

// Throws the copy away and goes back to the edited scene.
static void
play_stop(editor_state* state)
{
    if (!play_active(&state->play))
        return;

    inspector_finish_edit(&state->inspector, &state->play.commands);
    gizmo_finish_edit(state, &state->play.commands);
    if (chat_busy(&state->chat) && state->chat.turn_ctx == &state->play.commands)
        chat_stop(&state->chat);   // a turn started while playing; its world is going away

    play_end(&state->play, &state->doc);   // the IBL re-bakes if Play changed the HDRI
    // The selection stays if it names something in the edited scene
    // (validate_selection() drops one made while playing).

    char line[96];
    snprintf(line, sizeof(line), "Stopped after %.1f s of play; back to the edited scene.", (double)state->play.time);
    console_info(&state->console, line);
}

// --- Export ---------------------------------------------------------------------------
// The Export dialog's work: Project/Project.h project_export() with the Game
// executable (GAME_RELEASE_EXE when it's been built, else GAME_EXE) and,
// for a Release game, the C++ runtime DLLs CMake found (GAME_RUNTIME_DLLS);
// then the exported game's --check run, watched by watch_export_check().

#define EXPORT_MAX_DLLS 16

static int64
modified_time(const char* path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (int64)info.st_mtime : 0;
}

// The Game executable to ship. *out_release: it's a non-Debug build (the
// one for players). *out_stale (may be NULL): it's the separately built
// Release game, older than this build's own Game -- probably missing the
// latest engine changes.
static const char*
game_executable(b8* out_release, b8* out_stale)
{
    if (out_stale)
        *out_stale = false;
    if (!GAME_EXE_IS_DEBUG)
    {
        *out_release = true;   // this build's own Game is the player one
        return GAME_EXE;
    }

    *out_release = file_exists(GAME_RELEASE_EXE);
    if (!*out_release)
        return GAME_EXE;
    if (out_stale)
        *out_stale = modified_time(GAME_RELEASE_EXE) < modified_time(GAME_EXE);
    return GAME_RELEASE_EXE;
}

// Splits GAME_RUNTIME_DLLS ('|'-separated) into `paths` (pointers into
// `buffer`); returns how many.
static uint32
runtime_dlls(char* buffer, size_t size, const char* paths[EXPORT_MAX_DLLS])
{
    snprintf(buffer, size, "%s", GAME_RUNTIME_DLLS);
    uint32 count = 0;
    for (char* item = buffer; item && *item && count < EXPORT_MAX_DLLS;)
    {
        char* bar = strchr(item, '|');
        if (bar)
            *bar++ = '\0';
        paths[count++] = item;
        item = bar;
    }
    return count;
}

// Documents/PBR Exports/<project name>.
static void
default_export_location(const editor_state* state, char* out, size_t size)
{
    char projects[PROJECT_PATH_MAX];
    default_projects_location(projects, sizeof(projects));   // .../Documents/PBR Projects
    char* last = strrchr(projects, '/');
    if (last)
        *last = '\0';
    snprintf(out, size, "%s/PBR Exports/%s", projects, state->project.name);
}

// Forgets the last export -- its folder, status, check and picture, ending a
// check still running -- when the project it belonged to closes (and at
// shutdown), so the next export doesn't go to that project's folder.
static void
reset_export(editor_state* state)
{
    process_destroy(state->check_process);
    state->check_process = NULL;
    if (state->check_picture)
        rhi_texture_destroy(state->check_picture);
    state->check_picture    = NULL;
    state->check            = CHECK_NONE;
    state->export_ok        = false;
    state->export_dir[0]    = '\0';
    state->export_status[0] = '\0';
}

static void
open_export_dialog(editor_state* state)
{
    if (!state->export_dir[0])
        default_export_location(state, state->export_dir, sizeof(state->export_dir));
    state->open_popup = POPUP_EXPORT;
}

// Exports into `folder` and, if that worked, starts the exported game's
// check. The outcome goes to export_status (and the return value).
static b8
run_export(editor_state* state, const char* folder)
{
    if (state->check_process)
    {
        snprintf(state->export_status, sizeof(state->export_status), "The last export's check is still running.");
        return false;
    }
    if (state->check_picture)
    {
        rhi_texture_destroy(state->check_picture);
        state->check_picture = NULL;
    }
    state->check = CHECK_NONE;

    b8          release;
    const char* game = game_executable(&release, NULL);
    char        dll_buffer[4096];
    const char* dlls[EXPORT_MAX_DLLS];
    uint32      dll_count = release ? runtime_dlls(dll_buffer, sizeof(dll_buffer), dlls) : 0;

    project_export_options options = { game, paths_engine_root(), dlls, dll_count };
    char error[640];
    state->export_ok = project_export(paths_project_root(), folder, &options, &state->export_report, error, sizeof(error));
    if (!state->export_ok)
    {
        snprintf(state->export_status, sizeof(state->export_status), "Not exported: %s", error);
        return false;
    }

    snprintf(state->export_status, sizeof(state->export_status), "Exported %u files (%.1f MB), %u scene(s), to %s.",
             state->export_report.files, (double)state->export_report.bytes / (1024.0 * 1024.0),
             state->export_report.scenes, folder);
    console_info(&state->console, state->export_status);

    // The game checks itself: its startup scene, in a hidden window.
    char* png = user_data_path("export_check.png");
    snprintf(state->check_png, sizeof(state->check_png), "%s", png);
    free(png);
    remove(state->check_png);

    // A Release game writes log.txt beside itself; a Debug one logs to its
    // console, caught here instead.
    char* log = user_data_path("export_check.log");
    remove(log);
    const char* argv[] = { state->export_report.executable, "--check", state->check_png, NULL };
    state->check_process = process_spawn(argv, log);
    free(log);
    state->check           = state->check_process ? CHECK_RUNNING : CHECK_FAILED;
    state->check_exit_code = -1;
    return true;
}

// Once a frame: notices the check finishing, and loads its picture.
static void
watch_export_check(editor_state* state)
{
    int exit_code;
    if (!state->check_process || process_running(state->check_process, &exit_code))
        return;

    process_destroy(state->check_process);
    state->check_process   = NULL;
    state->check           = exit_code == 0 ? CHECK_PASSED : CHECK_FAILED;
    state->check_exit_code = exit_code;
    if (exit_code == 0 && file_exists(state->check_png))
        state->check_picture = rhi_texture_create_from_file(state->check_png, RHI_FILTER_LINEAR, RHI_WRAP_CLAMP_TO_EDGE, false);

    console_info(&state->console, exit_code == 0 ? "Export check passed: the game loads and renders its startup scene."
                                                 : "Export check FAILED: see log.txt in the export folder "
                                                   "(~/.pbr-renderer/export_check.log for a Debug game).");
}

// Shows `folder` in the system's file browser.
static void
open_folder(const char* folder)
{
#ifdef _WIN32
    char path[PROJECT_PATH_MAX];
    snprintf(path, sizeof(path), "%s", folder);
    for (char* c = path; *c; ++c)
        if (*c == '/')
            *c = '\\';
    const char* argv[] = { "C:\\Windows\\explorer.exe", path, NULL };
#elif defined(__APPLE__)
    const char* argv[] = { "/usr/bin/open", folder, NULL };
#else
    const char* argv[] = { "/usr/bin/xdg-open", folder, NULL };
#endif
    // Left to run on its own: process_destroy() would end it. Its small
    // handle is never freed (one per click).
    (void)process_spawn(argv, NULL);
}

// --- Viewport toolbar ---------------------------------------------------------------

// scene_debug_view's values, for the toolbar's combo and the View menu.
static const char* const DEBUG_VIEW_LABELS[SCENE_VIEW_COUNT] = {
    "Lit", "Albedo", "Normals", "Roughness", "Metallic", "AO", "Shadow", "Lighting", "Wireframe"
};

// A button that can be greyed out, with a tooltip saying why (or what it does).
static b8
toolbar_button(const char* label, b8 enabled, const char* tooltip)
{
    igBeginDisabled(!enabled);
    b8 pressed = igButton(label, (ImVec2_c){ 0, 0 });
    igEndDisabled();
    if (tooltip && igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        igSetTooltip("%s", tooltip);
    igSameLine(0.0f, -1.0f);
    return pressed;
}

static void
toolbar_separator(void)
{
    igTextDisabled("|");
    igSameLine(0.0f, -1.0f);
}

// Along the Viewport's top: Play / Pause / Step / Stop, the gizmo's
// operation, space and snapping, and Export.
static void
draw_toolbar(editor_state* state)
{
    const b8 playing = play_active(&state->play);

    if (!playing)
    {
        b8 busy = chat_busy(&state->chat);
        if (toolbar_button("Play", !busy, busy ? "A chat turn is running" : "Run the scene here (Ctrl+P)"))
            play_start(state);
    }
    else if (toolbar_button("Stop", true, "Back to the edited scene; changes made while playing are dropped (Ctrl+P)"))
    {
        play_stop(state);
    }
    if (toolbar_button(state->play.mode == PLAY_PAUSED ? "Resume" : "Pause", playing, "Ctrl+Shift+P"))
        play_toggle_pause(&state->play);
    if (toolbar_button("Step", state->play.mode == PLAY_PAUSED, "One frame forward (F10)"))
        state->play.step = true;
    if (playing)
    {
        igTextColored(state->play.mode == PLAY_PAUSED ? (ImVec4_c){ 1.0f, 0.71f, 0.0f, 1.0f } : (ImVec4_c){ 0.31f, 0.78f, 0.31f, 1.0f },
                      "%s %.1f s", state->play.mode == PLAY_PAUSED ? "Paused" : "Playing", (double)state->play.time);
        igSameLine(0.0f, -1.0f);
    }
    toolbar_separator();

    igRadioButton_IntPtr("Move", (int*)&state->gizmo_op, GIZMO_TRANSLATE);
    if (igIsItemHovered(0)) igSetTooltip("W");
    igSameLine(0.0f, -1.0f);
    igRadioButton_IntPtr("Rotate", (int*)&state->gizmo_op, GIZMO_ROTATE);
    if (igIsItemHovered(0)) igSetTooltip("E");
    igSameLine(0.0f, -1.0f);
    igRadioButton_IntPtr("Scale", (int*)&state->gizmo_op, GIZMO_SCALE);
    if (igIsItemHovered(0)) igSetTooltip("R");
    igSameLine(0.0f, -1.0f);
    toolbar_separator();

    igRadioButton_IntPtr("Local", (int*)&state->gizmo_space, GIZMO_LOCAL);
    igSameLine(0.0f, -1.0f);
    igRadioButton_IntPtr("World", (int*)&state->gizmo_space, GIZMO_WORLD);
    if (state->gizmo_op == GIZMO_SCALE && igIsItemHovered(0))
        igSetTooltip("Scale always follows the object's own axes");
    igSameLine(0.0f, -1.0f);
    toolbar_separator();

    igCheckbox("Snap", &state->gizmo_snap);
    if (igIsItemHovered(0))
        igSetTooltip("Ctrl while dragging flips it");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(70.0f);
    if (state->gizmo_op == GIZMO_TRANSLATE)
        igDragFloat("##step", &state->snap_move, 0.01f, 0.01f, 10.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
    else if (state->gizmo_op == GIZMO_ROTATE)
        igDragFloat("##step", &state->snap_rotate, 0.5f, 1.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
    else
        igDragFloat("##step", &state->snap_scale, 0.01f, 0.01f, 1.0f, "x%.2f", ImGuiSliderFlags_AlwaysClamp);
    igSameLine(0.0f, -1.0f);
    toolbar_separator();

    igSetNextItemWidth(110.0f);
    igCombo_Str_arr("##view", (int*)&state->debug_view, DEBUG_VIEW_LABELS, SCENE_VIEW_COUNT, -1);
    if (igIsItemHovered(0))
        igSetTooltip("What the view shows: the lit scene, or one input to its shading");
    igSameLine(0.0f, -1.0f);
    toolbar_separator();

    if (toolbar_button("Export", true, "Make a folder with the game a player can run"))
        open_export_dialog(state);
    igNewLine();
}

// A frame around the view while playing -- green running, amber paused --
// so a scene about to be thrown away is never mistaken for the edited one.
static void
draw_play_frame(editor_state* state, ImDrawList* draw)
{
    if (!play_active(&state->play))
        return;

    const ImU32 color = state->play.mode == PLAY_PAUSED ? 0xFF00B4FF : 0xFF50C850;   // ABGR
    ImDrawList_AddRect(draw, (ImVec2_c){ state->view_x + 1.0f, state->view_y + 1.0f },
                       (ImVec2_c){ state->view_x + state->view_width - 1.0f, state->view_y + state->view_height - 1.0f },
                       color, 0.0f, 3.0f, 0);
}

// --- Running commands for the UI -------------------------------------------------------

// Runs {cmd, args} (takes `args`). Returns the result on success (caller
// deletes it), or NULL with the error in the console.
static cJSON*
run_command(editor_state* state, const char* cmd, cJSON* args)
{
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", cmd);
    cJSON_AddItemToObject(request, "args", args ? args : cJSON_CreateObject());

    cJSON* response = cmd_execute(active_commands(state), request);
    cJSON_Delete(request);

    cJSON* result = NULL;
    if (cJSON_IsTrue(cJSON_GetObjectItem(response, "ok")))
    {
        result = cJSON_DetachItemFromObject(response, "result");
        if (!result)
            result = cJSON_CreateObject();
    }
    else
    {
        const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "message"));
        char        line[640];
        snprintf(line, sizeof(line), "%s: %s", cmd, message ? message : "failed");
        console_info(&state->console, line);
    }
    cJSON_Delete(response);
    return result;
}

// --- Project and scene ------------------------------------------------------------------

// "PBR Editor - Racing Game - main*"
static void
update_title(editor_state* state)
{
    static char last[PROJECT_NAME_MAX + PROJECT_SCENE_MAX + 64];
    char        title[sizeof(last)];
    snprintf(title, sizeof(title), "PBR Editor - %s - %s%s", state->project.name,
             state->doc.name[0] ? state->doc.name : "untitled", doc_dirty(&state->doc) ? "*" : "");

    if (strcmp(title, last) != 0)
    {
        window_set_title(get_renderer()->drawing_window, title);
        memcpy(last, title, sizeof(last));
    }
}

// Closes any edit in progress, so it doesn't join what comes next.
static void
finish_edits(editor_state* state)
{
    inspector_finish_edit(&state->inspector, active_commands(state));
    gizmo_finish_edit(state, active_commands(state));
}

// Tells the console and clients the scene was replaced wholesale (no
// per-entity changes to list), as of `revision`.
static void
announce_replaced(editor_state* state, uint64 revision, const char* source)
{
    cJSON* event = cJSON_CreateObject();
    cJSON_AddNumberToObject(event, "revision", (double)revision);
    cJSON_AddStringToObject(event, "source", source);
    cJSON_AddArrayToObject(event, "changes");
    on_world_change(state, event);
    cJSON_Delete(event);
}

// Replaces the scene with `scene_json` (a parsed scene file), named `name`
// ("" = untitled). Loaded into a fresh World first, so a bad file leaves
// the current scene as it was. False, with why in the console.
static b8
replace_scene(editor_state* state, const cJSON* scene_json, const char* name, const char* source)
{
    // A scene that names no HDRI gets the project's default, as in the game.
    char              error[640];
    world             next;
    scene_environment env;
    if (!doc_read(&state->doc, scene_json, state->project.default_hdri, &next, &env, error, sizeof(error)))
    {
        console_info(&state->console, error);
        return false;
    }

    finish_edits(state);
    uint64 revision = doc_replace(&state->doc, &next, &env, name);
    state->selected = ENTITY_NULL;

    announce_replaced(state, revision, source);
    frame_view(state, NULL);
    return true;
}

static b8
open_scene(editor_state* state, const char* name)
{
    char relative[PROJECT_SCENE_MAX + 16];
    snprintf(relative, sizeof(relative), "scenes/%s.json", name);
    char*  path   = project_path(relative);
    char*  text   = file_exists(path) ? file_read_to_string(path) : NULL;
    cJSON* parsed = text ? cJSON_Parse(text) : NULL;
    free(text);

    b8 ok = false;
    if (!parsed)
    {
        char line[PROJECT_PATH_MAX + 64];
        snprintf(line, sizeof(line), "No readable scene at %s", path);
        console_info(&state->console, line);
    }
    else
    {
        char source[PROJECT_SCENE_MAX + 16];
        snprintf(source, sizeof(source), "open %s", name);
        ok = replace_scene(state, parsed, name, source);
    }
    cJSON_Delete(parsed);
    free(path);
    return ok;
}

static void
new_scene(editor_state* state)
{
    cJSON* starter = project_starter_scene(&state->project);
    replace_scene(state, starter, "", "new scene");
    cJSON_Delete(starter);
}

// Saves to scenes/<name>.json (scene.save: the entities and the environment).
static b8
save_scene_as(editor_state* state, const char* name)
{
    if (play_active(&state->play))
    {
        snprintf(state->popup_error, sizeof(state->popup_error), "Stop playing first: what's on screen is Play's copy.");
        return false;
    }
    if (!project_scene_name_valid(name))
    {
        snprintf(state->popup_error, sizeof(state->popup_error),
                 "\"%s\": use 1-64 letters, digits, '_' or '-'", name);
        return false;
    }

    finish_edits(state);
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "name", name);
    cJSON* result = run_command(state, "scene.save", args);
    if (!result)
    {
        snprintf(state->popup_error, sizeof(state->popup_error), "Couldn't save; the console says why.");
        return false;
    }
    cJSON_Delete(result);

    doc_mark_saved(&state->doc, name);

    char line[PROJECT_SCENE_MAX + 32];
    snprintf(line, sizeof(line), "Saved scene %s", name);
    console_info(&state->console, line);
    return true;
}

// Ctrl+S: to the scene's file, or Save As for a scene that has none yet.
static void
save_scene(editor_state* state)
{
    if (play_active(&state->play))
    {
        console_info(&state->console, "Stop playing (Ctrl+P) to save: what's on screen is Play's copy.");
        return;
    }
    if (state->doc.name[0])
    {
        save_scene_as(state, state->doc.name);
        return;
    }
    state->save_as_name[0] = '\0';
    state->popup_error[0]  = '\0';
    state->open_popup      = POPUP_SAVE_AS;
}

// Switches to the project in `directory`: its folder becomes the project
// root, and its startup scene opens (a new scene if it has none). False,
// with why in popup_error, if it isn't a project.
static b8
open_project(editor_state* state, const char* directory)
{
    project_settings settings;
    if (!project_read(directory, &settings, state->popup_error, sizeof(state->popup_error)))
        return false;
    if (chat_busy(&state->chat))
    {
        snprintf(state->popup_error, sizeof(state->popup_error), "Wait for the chat turn to finish (or Stop it) first.");
        return false;
    }

    // Everything of the old project goes before anything of the new one
    // loads: the asset cache is keyed by project-relative path, so a model
    // still cached as "models/tree.glb" would stand in for the new
    // project's file of that name.
    finish_edits(state);
    doc_reset_history(&state->doc);
    reset_export(state);
    world_clear(&state->doc.scene);
    assets_collect_unused(&state->assets);
    ibl_cache_invalidate(&state->ibl);   // "hdri/sky.hdr" may be another file now

    paths_set_project_root(directory);
    state->project = settings;
    project_recent_add(&state->recent, paths_project_root(), NULL);

    char line[PROJECT_PATH_MAX + PROJECT_NAME_MAX + 32];
    snprintf(line, sizeof(line), "Opened project %s (%s)", settings.name, paths_project_root());
    console_info(&state->console, line);
    chat_info(&state->chat, line);

    if (!open_scene(state, settings.startup_scene))
        new_scene(state);
    return true;
}

static void
perform(editor_state* state, pending_action action, const char* arg)
{
    switch (action)
    {
        case ACTION_OPEN_PROJECT:
            if (!open_project(state, arg))
            {
                // Show why, in the dialog.
                snprintf(state->open_project_path, sizeof(state->open_project_path), "%s", arg);
                state->open_popup = POPUP_OPEN_PROJECT;
            }
            break;
        case ACTION_NEW_SCENE:  new_scene(state); break;
        case ACTION_OPEN_SCENE: open_scene(state, arg); break;
        case ACTION_QUIT:       state->app->should_close = true; break;
        case ACTION_NONE:       break;
    }
}

// Does `action` now, or -- with unsaved changes -- once "save changes?" is
// answered.
static void
request(editor_state* state, pending_action action, const char* arg)
{
    // Each of these replaces the edited scene (or quits): Play's copy goes
    // first, so "unsaved changes" means the edited scene's.
    play_stop(state);

    if ((action == ACTION_NEW_SCENE || action == ACTION_OPEN_SCENE) && chat_busy(&state->chat))
    {
        console_info(&state->console, "Wait for the chat turn to finish (or Stop it) before opening another scene.");
        return;
    }
    if (!doc_dirty(&state->doc))
    {
        perform(state, action, arg);
        return;
    }
    state->pending = action;
    snprintf(state->pending_arg, sizeof(state->pending_arg), "%s", arg ? arg : "");
    state->open_popup = POPUP_UNSAVED;
}

// --- Add ---------------------------------------------------------------------------

// Where new things go: under the point the view orbits, at `height`.
static void
add_position(const editor_state* state, f32 height, vec3 out)
{
    out[0] = state->cam.target[0];
    out[1] = height;
    out[2] = state->cam.target[2];
}

// Selects the entity a create/place result names (and frees the result).
static void
select_result(editor_state* state, cJSON* result)
{
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(result, "name"));
    if (name)
    {
        state->selected         = world_find(active_world(state), name);
        state->reveal_selection = true;
    }
    cJSON_Delete(result);
}

static void
add_model(editor_state* state, const char* key)
{
    vec3 position;
    add_position(state, 0.0f, position);

    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "model", key);
    cJSON_AddNumberToObject(args, "size", state->add_size);
    cJSON_AddItemToObject(args, "position", vec3_array(position));
    cJSON_AddBoolToObject(args, "on_ground", true);

    cJSON* result = run_command(state, "place_model", args);
    if (result)
        select_result(state, result);
}

// entity.create with a Transform at `position` (and `rotation_deg` if not
// NULL), plus `component` with `fields` if not NULL.
static void
add_entity(editor_state* state, const char* name, const f32* position, const f32* rotation_deg,
           const char* component, cJSON* fields)
{
    cJSON* components = cJSON_CreateObject();
    cJSON* transform  = cJSON_AddObjectToObject(components, "Transform");
    cJSON_AddItemToObject(transform, "position", vec3_array(position));
    if (rotation_deg)
        cJSON_AddItemToObject(transform, "rotation_deg", vec3_array(rotation_deg));
    if (component)
        cJSON_AddItemToObject(components, component, fields);

    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "name", name);
    cJSON_AddItemToObject(args, "components", components);

    cJSON* result = run_command(state, "entity.create", args);
    if (result)
        select_result(state, result);
}

// "point", "spot" or "directional" (a sun).
static void
add_light(editor_state* state, const char* type)
{
    b8   sun = strcmp(type, "directional") == 0;
    vec3 position;
    add_position(state, sun ? 0.0f : 2.5f, position);

    cJSON* light = cJSON_CreateObject();
    cJSON_AddStringToObject(light, "type", type);
    cJSON_AddNumberToObject(light, "intensity", sun ? 2.0 : 10.0);
    if (!sun)
        cJSON_AddNumberToObject(light, "range", 10);
    if (strcmp(type, "spot") == 0)
    {
        const f32 down[3] = { 0.0f, -1.0f, 0.0f };
        cJSON_AddItemToObject(light, "direction", vec3_array(down));
        cJSON_AddNumberToObject(light, "inner_cone_degrees", 15);
        cJSON_AddNumberToObject(light, "outer_cone_degrees", 30);
    }
    else if (sun)
    {
        const f32 slant[3] = { -0.4f, -0.7f, -0.5f };
        cJSON_AddItemToObject(light, "direction", vec3_array(slant));
    }

    char name[32];
    snprintf(name, sizeof(name), "%s_light", sun ? "sun" : type);
    add_entity(state, name, position, NULL, "Light", light);
}

// A Camera entity seeing what the editor's view sees. Its -Z axis has to
// point from the eye at the orbit target: with rotation Ry(yaw) * Rx(pitch)
// that is pitch = -orbit.pitch, yaw = orbit.yaw.
static void
add_camera(editor_state* state)
{
    const f32 rotation[3] = { -state->orbit.pitch, state->orbit.yaw, 0.0f };

    cJSON* cam = cJSON_CreateObject();
    cJSON_AddNumberToObject(cam, "fov_degrees", state->cam.fov_degrees);
    add_entity(state, "camera", state->cam.position, rotation, "Camera", cam);
}

// The Scene panel's Add section.
static void
draw_add_controls(editor_state* state)
{
    igText("Add (at the view's centre)");
    igDragFloat("Size (m)", &state->add_size, 0.05f, 0.05f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (igIsItemHovered(0))
        igSetTooltip("Longest side of a model or shape added below");

    if (igBeginCombo("##add_model", "Model from the project...", 0))
    {
        cJSON*       list   = run_command(state, "assets.list", NULL);
        const cJSON* models = cJSON_GetObjectItem(list, "models");
        const cJSON* key;
        cJSON_ArrayForEach(key, models)
        {
            if (cJSON_IsString(key) && igSelectable_Bool(key->valuestring, false, 0, (ImVec2_c){ 0, 0 }))
                add_model(state, key->valuestring);
        }
        if (cJSON_GetArraySize(models) == 0)
            igTextDisabled("(none yet -- File > Import Asset)");
        cJSON_Delete(list);
        igEndCombo();
    }

    if (igButton("Cube", (ImVec2_c){ 0, 0 }))
        add_model(state, "builtin:cube");
    igSameLine(0.0f, -1.0f);
    if (igButton("Sphere", (ImVec2_c){ 0, 0 }))
        add_model(state, "builtin:sphere");
    igSameLine(0.0f, -1.0f);
    if (igButton("Plane", (ImVec2_c){ 0, 0 }))
        add_model(state, "builtin:plane");

    if (igButton("Point light", (ImVec2_c){ 0, 0 }))
        add_light(state, "point");
    igSameLine(0.0f, -1.0f);
    if (igButton("Spot light", (ImVec2_c){ 0, 0 }))
        add_light(state, "spot");
    igSameLine(0.0f, -1.0f);
    if (igButton("Sun", (ImVec2_c){ 0, 0 }))
        add_light(state, "directional");

    if (igButton("Camera (this view)", (ImVec2_c){ 0, 0 }))
        add_camera(state);
    igSameLine(0.0f, -1.0f);
    if (igButton("Empty", (ImVec2_c){ 0, 0 }))
    {
        vec3 position;
        add_position(state, 0.0f, position);
        add_entity(state, "empty", position, NULL, NULL, NULL);
    }
    igSeparator();
}

// --- Menus and dialogs ------------------------------------------------------------------

static void
draw_scene_menu_items(editor_state* state)
{
    cJSON*       list   = run_command(state, "scene.list", NULL);
    const cJSON* scenes = cJSON_GetObjectItem(list, "scenes");
    const cJSON* name;
    cJSON_ArrayForEach(name, scenes)
    {
        if (!cJSON_IsString(name))
            continue;
        b8 current = strcmp(name->valuestring, state->doc.name) == 0;
        if (igMenuItem_Bool(name->valuestring, NULL, current, true))
            request(state, ACTION_OPEN_SCENE, name->valuestring);
    }
    if (cJSON_GetArraySize(scenes) == 0)
        igTextDisabled("(no saved scenes)");
    cJSON_Delete(list);
}

static void
draw_main_menu(editor_state* state)
{
    if (!igBeginMainMenuBar())
        return;

    if (igBeginMenu("File", true))
    {
        if (igMenuItem_Bool("New Project...", NULL, false, true))
        {
            snprintf(state->new_project_name, sizeof(state->new_project_name), "My Game");
            if (!state->new_project_location[0])
                default_projects_location(state->new_project_location, sizeof(state->new_project_location));
            state->popup_error[0] = '\0';
            state->open_popup     = POPUP_NEW_PROJECT;
        }
        if (igMenuItem_Bool("Open Project...", NULL, false, true))
        {
            state->open_project_path[0] = '\0';
            state->popup_error[0]       = '\0';
            state->open_popup           = POPUP_OPEN_PROJECT;
        }
        if (igBeginMenu("Recent Projects", state->recent.count > 0))
        {
            for (uint32 i = 0; i < state->recent.count; ++i)
            {
                if (igMenuItem_Bool(state->recent.paths[i], NULL, false, true))
                {
                    char directory[PROJECT_PATH_MAX];   // the list changes when it opens
                    snprintf(directory, sizeof(directory), "%s", state->recent.paths[i]);
                    request(state, ACTION_OPEN_PROJECT, directory);
                }
            }
            igEndMenu();
        }
        igSeparator();

        if (igMenuItem_Bool("New Scene", NULL, false, true))
            request(state, ACTION_NEW_SCENE, NULL);
        if (igBeginMenu("Open Scene", true))
        {
            draw_scene_menu_items(state);
            igEndMenu();
        }
        if (igMenuItem_Bool("Save", "Ctrl+S", false, true))
            save_scene(state);
        if (igMenuItem_Bool("Save As...", NULL, false, !play_active(&state->play)))
        {
            snprintf(state->save_as_name, sizeof(state->save_as_name), "%s", state->doc.name);
            state->popup_error[0] = '\0';
            state->open_popup     = POPUP_SAVE_AS;
        }
        igSeparator();

        if (igMenuItem_Bool("Import Asset...", NULL, false, true))
        {
            state->import_path[0] = '\0';
            state->popup_error[0] = '\0';
            state->open_popup     = POPUP_IMPORT;
        }
        if (igMenuItem_Bool("Export Game...", NULL, false, true))
            open_export_dialog(state);
        if (igMenuItem_Bool("Project Settings...", NULL, false, true))
        {
            state->settings_edit  = state->project;
            state->popup_error[0] = '\0';
            state->open_popup     = POPUP_SETTINGS;
        }
        igSeparator();

        if (igMenuItem_Bool("Quit", "Esc", false, true))
            request(state, ACTION_QUIT, NULL);
        igEndMenu();
    }

    if (igBeginMenu("View", true))
    {
        igMenuItem_BoolPtr("Console", "F1", &state->show_console, true);
        igMenuItem_BoolPtr("GPU timings", NULL, &state->show_gpu_timings, true);
        igMenuItem_BoolPtr("Shadow cascades (debug)", NULL, &state->debug_show_cascades, true);
        if (igBeginMenu("Debug view", true))
        {
            for (int v = 0; v < SCENE_VIEW_COUNT; ++v)
                if (igMenuItem_Bool(DEBUG_VIEW_LABELS[v], NULL, state->debug_view == (scene_debug_view)v, true))
                    state->debug_view = (scene_debug_view)v;
            igEndMenu();
        }
        if (igMenuItem_Bool("Frame everything", NULL, false, true))
            frame_view(state, NULL);
        igEndMenu();
    }

    igEndMainMenuBar();
}

// The open dialog's error line, if any.
static void
draw_popup_error(editor_state* state)
{
    if (state->popup_error[0])
    {
        igPushTextWrapPos(480.0f);
        igTextColored((ImVec4_c){ 1.0f, 0.45f, 0.4f, 1.0f }, "%s", state->popup_error);
        igPopTextWrapPos();
    }
}

static b8
dialog_button(const char* label)
{
    return igButton(label, (ImVec2_c){ 120.0f, 0.0f });
}

static void
draw_unsaved_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_UNSAVED, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("Save the changes to %s first?", state->doc.name[0] ? state->doc.name : "this untitled scene");
    igSpacing();

    if (dialog_button("Save"))
    {
        igCloseCurrentPopup();
        if (state->doc.name[0])
        {
            if (save_scene_as(state, state->doc.name))
                perform(state, state->pending, state->pending_arg);
            state->pending = ACTION_NONE;
        }
        else
        {
            // No file yet: ask for a name, then carry on.
            state->save_as_name[0]        = '\0';
            state->popup_error[0]         = '\0';
            state->continue_after_save_as = true;
            state->open_popup             = POPUP_SAVE_AS;
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Don't save"))
    {
        igCloseCurrentPopup();
        perform(state, state->pending, state->pending_arg);
        state->pending = ACTION_NONE;
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
    {
        igCloseCurrentPopup();
        state->pending = ACTION_NONE;
    }
    igEndPopup();
}

static void
draw_save_as_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_SAVE_AS, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("Scene name, saved as scenes/<name>.json in %s", state->project.name);
    if (igIsWindowAppearing())
        igSetKeyboardFocusHere(0);
    b8 enter = igInputText("##scene_name", state->save_as_name, sizeof(state->save_as_name),
                           ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);

    char relative[PROJECT_SCENE_MAX + 16];
    snprintf(relative, sizeof(relative), "scenes/%s.json", state->save_as_name);
    char* full = project_path(relative);
    if (state->save_as_name[0] && strcmp(state->save_as_name, state->doc.name) != 0 && file_exists(full))
        igTextDisabled("Replaces the existing %s.", state->save_as_name);
    free(full);

    draw_popup_error(state);

    if (dialog_button("Save") || enter)
    {
        if (save_scene_as(state, state->save_as_name))
        {
            igCloseCurrentPopup();
            if (state->continue_after_save_as)
                perform(state, state->pending, state->pending_arg);
            state->continue_after_save_as = false;
            state->pending                = ACTION_NONE;
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
    {
        igCloseCurrentPopup();
        state->continue_after_save_as = false;
        state->pending                = ACTION_NONE;
    }
    igEndPopup();
}

static void
draw_new_project_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_NEW_PROJECT, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igSetNextItemWidth(420.0f);
    igInputText("Name", state->new_project_name, sizeof(state->new_project_name), 0, NULL, NULL);
    igSetNextItemWidth(420.0f);
    igInputText("Location", state->new_project_location, sizeof(state->new_project_location), 0, NULL, NULL);

    char directory[PROJECT_PATH_MAX];
    snprintf(directory, sizeof(directory), "%s/%s", state->new_project_location, state->new_project_name);
    igTextDisabled("Creates %s", directory);
    draw_popup_error(state);

    if (dialog_button("Create"))
    {
        // Made now, then opened like any other project (asking about
        // unsaved changes first).
        if (project_create(directory, state->new_project_name, state->popup_error, sizeof(state->popup_error)))
        {
            igCloseCurrentPopup();
            request(state, ACTION_OPEN_PROJECT, directory);
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
        igCloseCurrentPopup();
    igEndPopup();
}

static void
draw_open_project_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_OPEN_PROJECT, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("Project folder (the one with project.json)");
    igSetNextItemWidth(480.0f);
    if (igIsWindowAppearing())
        igSetKeyboardFocusHere(0);
    b8 enter = igInputText("##project_path", state->open_project_path, sizeof(state->open_project_path),
                           ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);

    if (state->recent.count)
    {
        igTextDisabled("Recent:");
        for (uint32 i = 0; i < state->recent.count; ++i)
        {
            if (igSelectable_Bool(state->recent.paths[i], false, 0, (ImVec2_c){ 0, 0 }))
                snprintf(state->open_project_path, sizeof(state->open_project_path), "%s", state->recent.paths[i]);
        }
    }
    draw_popup_error(state);

    if (dialog_button("Open") || enter)
    {
        project_settings check;
        if (project_read(state->open_project_path, &check, state->popup_error, sizeof(state->popup_error)))
        {
            igCloseCurrentPopup();
            char directory[PROJECT_PATH_MAX];
            snprintf(directory, sizeof(directory), "%s", state->open_project_path);
            request(state, ACTION_OPEN_PROJECT, directory);
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
        igCloseCurrentPopup();
    igEndPopup();
}

static void
draw_import_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_IMPORT, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("File to copy into %s: .glb, .gltf, .obj or .hdr", state->project.name);
    igSetNextItemWidth(480.0f);
    if (igIsWindowAppearing())
        igSetKeyboardFocusHere(0);
    b8 enter = igInputText("##import_path", state->import_path, sizeof(state->import_path),
                           ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    igTextDisabled(".gltf and .obj bring the files they name (buffers, .mtl, textures) along.");
    draw_popup_error(state);

    if (dialog_button("Import") || enter)
    {
        // Pasted paths often come quoted.
        char path[PROJECT_PATH_MAX];
        snprintf(path, sizeof(path), "%s", state->import_path[0] == '"' ? state->import_path + 1 : state->import_path);
        size_t length = strlen(path);
        if (length && path[length - 1] == '"')
            path[length - 1] = '\0';

        char key[PROJECT_PATH_MAX];
        if (project_import_asset(paths_project_root(), path, key, sizeof(key), state->popup_error, sizeof(state->popup_error)))
        {
            char line[PROJECT_PATH_MAX + 64];
            snprintf(line, sizeof(line), "Imported %s (Scene panel > Add, or the HDRI list)", key);
            console_info(&state->console, line);
            igCloseCurrentPopup();
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
        igCloseCurrentPopup();
    igEndPopup();
}

static void
draw_settings_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_SETTINGS, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    project_settings* s = &state->settings_edit;
    igTextDisabled("%s", paths_project_root());
    igSetNextItemWidth(300.0f);
    igInputText("Name", s->name, sizeof(s->name), 0, NULL, NULL);

    // The scene the game opens with.
    igSetNextItemWidth(300.0f);
    if (igBeginCombo("Startup scene", s->startup_scene, 0))
    {
        cJSON*       list = run_command(state, "scene.list", NULL);
        const cJSON* name;
        cJSON_ArrayForEach(name, cJSON_GetObjectItem(list, "scenes"))
        {
            if (cJSON_IsString(name)
                && igSelectable_Bool(name->valuestring, strcmp(name->valuestring, s->startup_scene) == 0, 0, (ImVec2_c){ 0, 0 }))
                snprintf(s->startup_scene, sizeof(s->startup_scene), "%s", name->valuestring);
        }
        cJSON_Delete(list);
        igEndCombo();
    }

    int size[2] = { s->window_width, s->window_height };
    igSetNextItemWidth(300.0f);
    if (igInputInt2("Window size", size, 0))
    {
        s->window_width  = (uint16)(size[0] < 64 ? 64 : (size[0] > 16384 ? 16384 : size[0]));
        s->window_height = (uint16)(size[1] < 64 ? 64 : (size[1] > 16384 ? 16384 : size[1]));
    }

    // The HDRI new scenes start with.
    igSetNextItemWidth(300.0f);
    if (igBeginCombo("Default HDRI", s->default_hdri[0] ? s->default_hdri : "(none)", 0))
    {
        hdri_list hdris;
        list_hdris(&hdris);
        if (igSelectable_Bool("(none)", !s->default_hdri[0], 0, (ImVec2_c){ 0, 0 }))
            s->default_hdri[0] = '\0';
        for (uint32 i = 0; i < hdris.count; ++i)
        {
            if (igSelectable_Bool(hdris.paths[i], strcmp(hdris.paths[i], s->default_hdri) == 0, 0, (ImVec2_c){ 0, 0 }))
                snprintf(s->default_hdri, sizeof(s->default_hdri), "%s", hdris.paths[i]);
        }
        igEndCombo();
    }
    draw_popup_error(state);

    if (dialog_button("Save"))
    {
        if (project_write(paths_project_root(), s, state->popup_error, sizeof(state->popup_error)))
        {
            state->project = *s;
            igCloseCurrentPopup();
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
        igCloseCurrentPopup();
    igEndPopup();
}

static void
draw_export_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_EXPORT, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("Export %s as a game a player can run", state->project.name);
    igSetNextItemWidth(480.0f);
    igInputText("Folder", state->export_dir, sizeof(state->export_dir), 0, NULL, NULL);
    igTextDisabled("A new or empty folder, or an earlier export (replaced).");

    // What it ships: a Release game runs anywhere; a Debug one needs Visual
    // Studio's debug runtime.
    b8 release, stale;
    game_executable(&release, &stale);
    if (release && stale)
    {
        igPushTextWrapPos(480.0f);
        igTextColored((ImVec4_c){ 1.0f, 0.71f, 0.0f, 1.0f },
                      "Game: Release build, but older than this build of the engine, so it may miss recent "
                      "changes. Rebuild it: cmake --build build --config Release --target Game");
        igPopTextWrapPos();
    }
    else if (release)
        igTextDisabled("Game: Release build, with the C++ runtime DLLs.");
    else
    {
        igPushTextWrapPos(480.0f);
        igTextColored((ImVec4_c){ 1.0f, 0.71f, 0.0f, 1.0f },
                      "Game: Debug build. It runs only on PCs with Visual Studio. For players, build the Release "
                      "game first: cmake --build build --config Release --target Game");
        igPopTextWrapPos();
    }

    // Only saved scenes go out.
    if (doc_dirty(&state->doc))
    {
        igTextColored((ImVec4_c){ 1.0f, 0.71f, 0.0f, 1.0f }, "%s has unsaved changes, which won't be exported.",
                      state->doc.name[0] ? state->doc.name : "This scene");
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("Save now"))
            save_scene(state);
    }

    igSeparator();
    igBeginDisabled(state->check_process != NULL);
    if (dialog_button("Export"))
        run_export(state, state->export_dir);
    igEndDisabled();
    igSameLine(0.0f, -1.0f);
    igBeginDisabled(!state->export_ok);
    if (dialog_button("Open folder"))
        open_folder(state->export_dir);
    igEndDisabled();
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Close"))
        igCloseCurrentPopup();

    if (state->export_status[0])
    {
        igPushTextWrapPos(480.0f);
        if (state->export_ok)
            igTextWrapped("%s", state->export_status);
        else
            igTextColored((ImVec4_c){ 1.0f, 0.45f, 0.4f, 1.0f }, "%s", state->export_status);
        igPopTextWrapPos();
    }

    // The game's own check, and what it rendered.
    if (state->export_ok)
    {
        if (state->check == CHECK_RUNNING)
            igTextDisabled("Checking: the exported game is loading its startup scene...");
        else if (state->check == CHECK_PASSED)
            igTextColored((ImVec4_c){ 0.31f, 0.78f, 0.31f, 1.0f }, "Check passed: the game loads and renders %s.",
                          state->project.startup_scene);
        else if (state->check == CHECK_FAILED)
            igTextColored((ImVec4_c){ 1.0f, 0.45f, 0.4f, 1.0f },
                          "Check FAILED (exit code %d): see log.txt in the export folder (~/.pbr-renderer/export_check.log for a Debug game).", state->check_exit_code);

        if (state->check_picture)
        {
            ImTextureRef_c picture = { NULL, (ImTextureID)rhi_texture_native_handle(state->check_picture) };
            igImage(picture, (ImVec2_c){ 480.0f, 270.0f }, (ImVec2_c){ 0.0f, 1.0f }, (ImVec2_c){ 1.0f, 0.0f });
        }
    }
    igEndPopup();
}

static void
draw_popups(editor_state* state)
{
    if (state->open_popup)
    {
        igOpenPopup_Str(state->open_popup, 0);
        state->open_popup = NULL;
    }

    draw_unsaved_popup(state);
    draw_save_as_popup(state);
    draw_new_project_popup(state);
    draw_open_project_popup(state);
    draw_import_popup(state);
    draw_settings_popup(state);
    draw_export_popup(state);
}

// --- App hooks -------------------------------------------------------------------------

// Runs once from App.c's run(), after the GL context / RHI / ImGui exist.
static void
on_init(App* app)
{
    editor_state* state = (editor_state*)app->user_data;
    state->app = app;
    window_size framebuffer_size = window_get_size(get_renderer()->drawing_window);

    if (!scene_renderer_create(&state->renderer, framebuffer_size.width, framebuffer_size.height,
                               CASCADE_COUNT, SHADOW_MAP_SIZE))
        FATAL("editor: failed to create the scene renderer");

    assets_init(&state->assets);

    console_init(&state->console);
    inspector_init(&state->inspector);

    state->gizmo_op     = GIZMO_TRANSLATE;
    state->gizmo_space  = GIZMO_WORLD;
    state->snap_move    = 0.25f;
    state->snap_rotate  = 15.0f;
    state->snap_scale   = 0.1f;
    state->show_console = true;
    state->add_size     = 1.0f;

    state->server = server_start(NULL);
    chat_init(&state->chat, state->server);
    chat_panel_init(&state->chat_panel);

    uint32                  chat_command_count;
    const cmd_host_command* chat_command_table = chat_commands(&chat_command_count);

    doc_init(&state->doc, &state->assets, (command_context){
        .on_change            = on_world_change,
        .on_change_user       = state,
        .host_commands        = HOST_COMMANDS,
        .host_command_count   = HOST_COMMAND_COUNT,
        .host_user            = state,
        .module_commands      = chat_command_table,
        .module_command_count = chat_command_count,
        .module_user          = &state->chat,
    });

    if (state->server)
    {
        char line[256];
        snprintf(line, sizeof(line), "Command server: %s (also in ~/.pbr-renderer/server.json)", server_url(state->server));
        console_info(&state->console, line);
    }
    else
    {
        console_info(&state->console, "Command server could not start; only this console can run commands.");
    }

#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    snprintf(state->agent_log, sizeof(state->agent_log), "%s/.pbr-renderer/agent.log", home ? home : ".");

    // The chat agent connects through the server, so without one there's no agent.
    const char* autostart = getenv("PBR_AGENT_AUTOSTART");
    if (!state->server)
        snprintf(state->agent_note, sizeof(state->agent_note), "The command server didn't start, so no agent can connect.");
    else if (autostart && strcmp(autostart, "0") == 0)
        snprintf(state->agent_note, sizeof(state->agent_note), "Not started automatically (PBR_AGENT_AUTOSTART=0).");
    else
        start_agent(state);

    f32 aspect = (f32)framebuffer_size.width / (f32)framebuffer_size.height;
    camera_init(&state->cam, (vec3){ 4.0f, 3.0f, 6.0f }, (vec3){ 0.0f, 0.0f, 0.0f }, 45.0f, aspect, 0.1f, 500.0f);
    orbit_from_camera(&state->orbit, &state->cam);

    // The project main() checked, and its startup scene.
    char error[640];
    if (!project_read(paths_project_root(), &state->project, error, sizeof(error)))
        FATAL("editor: %s", error);
    project_recent_load(&state->recent, NULL);
    project_recent_add(&state->recent, paths_project_root(), NULL);

    char line[PROJECT_PATH_MAX * 2 + PROJECT_NAME_MAX + 64];
    snprintf(line, sizeof(line), "Project %s: %s (engine data: %s)", state->project.name, paths_project_root(),
             paths_engine_root());
    console_info(&state->console, line);

    if (!open_scene(state, state->project.startup_scene))
        new_scene(state);
}

// The window's close button: ask about unsaved changes first.
static b8
on_close_request(App* app)
{
    editor_state* state = (editor_state*)app->user_data;
    if (!doc_dirty(&state->doc))
        return true;
    request(state, ACTION_QUIT, NULL);
    return false;
}

static void on_key(App* app, int key, int action, int mods)
{
    editor_state* state = (editor_state*)app->user_data;

    // Esc clears the selection first; with nothing selected it quits.
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
    {
        validate_selection(state);
        if (state->selected)
            state->selected = ENTITY_NULL;
        else
            request(state, ACTION_QUIT, NULL);
    }
    if (key == GLFW_KEY_S && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL))
        save_scene(state);

    // Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z): undo / redo, like history.undo /
    // history.redo. Only reaches here when no text field has the keyboard
    // (ImGui handles undo inside those itself).
    // While playing there's nothing to undo: Stop drops it all.
    if ((mods & GLFW_MOD_CONTROL) && (action == GLFW_PRESS || action == GLFW_REPEAT)
        && (key == GLFW_KEY_Z || key == GLFW_KEY_Y) && play_active(&state->play))
    {
        if (action == GLFW_PRESS)
            console_info(&state->console, "No undo while playing: Stop (Ctrl+P) discards everything Play changed.");
    }
    else if ((mods & GLFW_MOD_CONTROL) && (action == GLFW_PRESS || action == GLFW_REPEAT)
        && (key == GLFW_KEY_Z || key == GLFW_KEY_Y))
    {
        b8 redo = key == GLFW_KEY_Y || (mods & GLFW_MOD_SHIFT);
        finish_edits(state);

        char*       reply   = cmd_execute_text(&state->doc.commands, redo ? "{\"cmd\":\"history.redo\"}" : "{\"cmd\":\"history.undo\"}");
        cJSON*      parsed  = cJSON_Parse(reply);
        const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(parsed, "error"), "message"));
        if (message)
            console_info(&state->console, message);
        cJSON_Delete(parsed);
        cJSON_free(reply);
    }
    if (key == GLFW_KEY_F && action == GLFW_PRESS && !(mods & GLFW_MOD_CONTROL))
        frame_selection(state);

    // W / E / R: the gizmo's move / rotate / scale (not mid-drag).
    if (action == GLFW_PRESS && !(mods & GLFW_MOD_CONTROL) && !gizmo_is_using())
    {
        if (key == GLFW_KEY_W) state->gizmo_op = GIZMO_TRANSLATE;
        if (key == GLFW_KEY_E) state->gizmo_op = GIZMO_ROTATE;
        if (key == GLFW_KEY_R) state->gizmo_op = GIZMO_SCALE;
    }
    if (key == GLFW_KEY_F1 && action == GLFW_PRESS)
        state->show_console = !state->show_console;

    // Ctrl+P: Play / Stop; Ctrl+Shift+P: pause / resume; F10: one step while paused.
    if (key == GLFW_KEY_P && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL))
    {
        if (mods & GLFW_MOD_SHIFT)
            play_toggle_pause(&state->play);
        else if (!play_active(&state->play))
            play_start(state);
        else
            play_stop(state);
    }
    if (key == GLFW_KEY_F10 && (action == GLFW_PRESS || action == GLFW_REPEAT) && state->play.mode == PLAY_PAUSED)
        state->play.step = true;
}

// Ambient, exposure and image-based lighting: the scene's environment, not
// World entities (saved with the scene all the same).
static void
draw_environment_controls(editor_state* state)
{
    igText("Environment");
    igSliderFloat("Ambient", &state->doc.env.ambient_strength, 0.0f, 2.0f, "%.2f", 0);
    igSliderFloat("Exposure", &state->doc.env.exposure, 0.1f, 4.0f, "%.2f", 0);

    if (igBeginCombo("HDRI", state->doc.env.hdri[0] ? state->doc.env.hdri : "(none)", 0))
    {
        hdri_list hdris;
        list_hdris(&hdris);
        if (igSelectable_Bool("(none)", !state->doc.env.hdri[0], 0, (ImVec2_c){ 0, 0 }))
            state->doc.env.hdri[0] = '\0';
        for (uint32 i = 0; i < hdris.count; ++i)
        {
            if (igSelectable_Bool(hdris.paths[i], strcmp(hdris.paths[i], state->doc.env.hdri) == 0, 0, (ImVec2_c){ 0, 0 }))
                snprintf(state->doc.env.hdri, sizeof(state->doc.env.hdri), "%s", hdris.paths[i]);
        }
        if (hdris.count == 0)
            igTextDisabled("(no .hdr in the project's hdri/)");
        igEndCombo();
    }

    if (ibl_cache_maps(&state->ibl))
    {
        igCheckbox("Use IBL", &state->doc.env.use_ibl);
        igCheckbox("Show skybox", &state->doc.env.show_skybox);
        igSliderFloat("Sky blur", &state->doc.env.sky_blur, 0.0f, 1.0f, "%.2f", 0);
    }
    else if (state->doc.env.hdri[0])
    {
        igTextWrapped("Couldn't load %s; lit by the hemisphere ambient light.", state->doc.env.hdri);
    }
    igSeparator();
}

// --- Spin preview -------------------------------------------------------------------
// The editor never runs world_tick() on the edited scene (Play ticks a
// copy): what it holds -- and what panels, commands and saving see -- is
// the authored pose. To still
// show Spin components moving, a frame's draw is bracketed by
// spin_preview_begin() (turn every spinning entity by `time`) and
// spin_preview_end() (put the authored rotations back).

typedef struct spin_preview
{
    entity_id* entities;
    vec3*      rotations;   // authored rotation_deg, restored by spin_preview_end()
    uint32     count;
} spin_preview;

static spin_preview
spin_preview_begin(world* w, f32 time)
{
    spin_preview preview = { 0 };
    const uint32 mask    = COMPONENT_BIT(COMPONENT_SPIN);

    uint32 capacity = world_count(w);
    if (capacity == 0)
        return preview;
    preview.entities  = malloc(capacity * sizeof(*preview.entities));
    preview.rotations = malloc(capacity * sizeof(*preview.rotations));
    if (!preview.entities || !preview.rotations)
        FATAL("editor: out of memory");

    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        c_transform*  t    = world_transform(w, e);
        const c_spin* spin = world_spin(w, e);

        preview.entities[preview.count] = e;
        glm_vec3_copy(t->rotation_deg, preview.rotations[preview.count]);
        ++preview.count;

        for (int axis = 0; axis < 3; ++axis)
            t->rotation_deg[axis] = fmodf(t->rotation_deg[axis] + spin->degrees_per_second[axis] * time, 360.0f);
    }
    return preview;
}

static void
spin_preview_end(world* w, spin_preview* preview)
{
    for (uint32 i = 0; i < preview->count; ++i)
    {
        c_transform* t = world_transform(w, preview->entities[i]);
        if (t)
            glm_vec3_copy(preview->rotations[i], t->rotation_deg);
    }

    // World matrices and bounds back to the authored pose too, for commands
    // that read them (placement relative to an entity, frame, describe).
    if (preview->count)
        world_update(w);

    free(preview->entities);
    free(preview->rotations);
    *preview = (spin_preview){ 0 };
}

// This frame's SceneRenderer settings from the GUI state.
static scene_render_settings
build_render_settings(const editor_state* state)
{
    scene_render_settings settings = scene_render_settings_default();

    scene_render_settings_apply_environment(&settings, &state->doc.env, ibl_cache_maps(&state->ibl));
    settings.debug_show_cascades = state->debug_show_cascades;
    settings.debug_view          = state->debug_view;

    // Where nothing (not even the skybox) is drawn -- a scene without an
    // HDRI: a neutral dark grey, so an empty background never reads as an
    // error.
    glm_vec3_copy((vec3){ 0.045f, 0.05f, 0.06f }, settings.clear_color);

    return settings;
}

// The panels' first-run arrangement, when the dockspace has never been
// split (no imgui.ini yet): Scene on the left, Entities over the Inspector
// on the right, Console and Chat along the bottom, the Viewport in the
// middle. After that, ImGui remembers whatever the user makes of it.
static void
default_layout(ImGuiID dockspace)
{
    const ImGuiDockNode* node = igDockBuilderGetNode(dockspace);
    if (node && (node->ChildNodes[0] || node->Windows.Size > 0))
        return;

    igDockBuilderRemoveNode(dockspace);
    igDockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(dockspace, igGetMainViewport()->WorkSize);

    ImGuiID center = dockspace, left, right, right_bottom, bottom;
    left   = igDockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, NULL, &center);
    right  = igDockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, NULL, &center);
    bottom = igDockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, NULL, &center);
    right  = igDockBuilderSplitNode(right, ImGuiDir_Up, 0.40f, NULL, &right_bottom);

    igDockBuilderDockWindow("Scene", left);
    igDockBuilderDockWindow("Entities", right);
    igDockBuilderDockWindow("Inspector", right_bottom);
    igDockBuilderDockWindow("Console (F1)", bottom);
    igDockBuilderDockWindow("Chat", bottom);
    igDockBuilderDockWindow("Viewport", center);
    igDockBuilderFinish(dockspace);
}

// (Re)makes the view's render target at `width` x `height`, with the scene
// renderer's HDR target and the camera's aspect to match. A no-op if the
// size is unchanged.
static void
fit_view_target(editor_state* state, uint16 width, uint16 height)
{
    if (state->view_target && state->view_target_width == width && state->view_target_height == height)
        return;

    if (state->view_target)
        rhi_framebuffer_destroy(state->view_target);

    rhi_framebuffer_attachment color = { .format = RHI_FORMAT_RGBA8 };
    state->view_target = rhi_framebuffer_create((rhi_framebuffer_desc){
        .width                  = width,
        .height                 = height,
        .color_attachments      = &color,
        .color_attachment_count = 1,
    });
    state->view_target_width  = width;
    state->view_target_height = height;

    scene_renderer_resize(&state->renderer, width, height);
    camera_set_aspect(&state->cam, (f32)width / (f32)height);
}

// The "Viewport" panel: the toolbar, then the scene on screen (the copy
// while playing, ticked by `dt`) rendered at the panel's size into
// view_target and shown as an image, with the selection outline and the
// gizmo on top and the mouse controls (view_input()) over it. Docked into
// the main area the first time.
// View > GPU timings: each pass of the Viewport's frame on the GPU, as a
// number and a bar against the whole GPU frame, then the CPU frame time.
static void
draw_gpu_timings(editor_state* state)
{
    if (!igBegin("GPU timings", &state->show_gpu_timings, 0))
    {
        igEnd();
        return;
    }

    const gpu_profiler* profiler = &state->renderer.profiler;
    if (profiler->timing_count == 0)
        igTextDisabled("No timings yet (or no GPU timestamp support).");

    for (uint32 i = 0; i < profiler->timing_count; ++i)
    {
        const gpu_timing* t = &profiler->timings[i];
        igText("%-8s %6.2f ms", t->name, (double)t->ms);
        igSameLine(0.0f, -1.0f);
        f32 fraction = profiler->frame_ms > 0.0f ? t->ms / profiler->frame_ms : 0.0f;
        igProgressBar(fraction, (ImVec2_c){ -1.0f, 0.0f }, "");
    }

    igSeparator();
    igText("GPU frame %6.2f ms", (double)profiler->frame_ms);
    igText("CPU frame %6.2f ms (%.0f fps)", (double)state->cpu_frame_ms,
           state->cpu_frame_ms > 0.0f ? 1000.0 / (double)state->cpu_frame_ms : 0.0);
    igTextDisabled("Averaged over about 20 frames. The Viewport only.");
    igEnd();
}

static void
draw_viewport(editor_state* state, ImGuiID dockspace, f32 dt)
{
    igSetNextWindowDockID(dockspace, ImGuiCond_FirstUseEver);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2_c){ 0.0f, 0.0f });
    // NoNavInputs: with the view focused, keys stay shortcuts (W/E/R, F,
    // Ctrl+Z ...) instead of moving ImGui's keyboard focus.
    b8 open = igBegin("Viewport", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                        | ImGuiWindowFlags_NoNavInputs);
    igPopStyleVar(1);

    // The game keeps running even when the panel is hidden behind a tab.
    play_advance(&state->play, dt);

    if (open)
    {
        // The toolbar gets the padding the image doesn't.
        ImVec2_c start = igGetCursorScreenPos();
        igSetCursorScreenPos((ImVec2_c){ start.x + 6.0f, start.y + 4.0f });
        igBeginGroup();
        draw_toolbar(state);
        igEndGroup();
        ImVec2_c after = igGetCursorScreenPos();
        igSetCursorScreenPos((ImVec2_c){ start.x, after.y + 2.0f });
    }

    ImVec2_c pos  = igGetCursorScreenPos();
    ImVec2_c size = igGetContentRegionAvail();
    if (!open || size.x < 1.0f || size.y < 1.0f)
    {
        // Hidden behind another tab, collapsed, or no room yet (the first
        // frame of a new layout): nothing to draw or drag. The empty item
        // closes the line the cursor was moved to, as ImGui requires.
        state->gizmo_hot = false;
        gizmo_finish_edit(state, active_commands(state));
        if (open)
            igDummy((ImVec2_c){ 0.0f, 0.0f });
        igEnd();
        return;
    }

    state->view_x      = pos.x;
    state->view_y      = pos.y;
    state->view_width  = floorf(size.x);
    state->view_height = floorf(size.y);
    fit_view_target(state, (uint16)state->view_width, (uint16)state->view_height);

    // One item over the whole view, so ImGui hands it the mouse (and doesn't
    // drag the window around instead).
    igInvisibleButton("##view", (ImVec2_c){ state->view_width, state->view_height }, ImGuiButtonFlags_MouseButtonLeft);
    b8      hovered   = igIsItemHovered(0);
    ImGuiID view_item = igGetItemID();
    view_input(state);

    // Playing, the copy really moves; editing, Spin is only previewed and
    // the authored pose put back after the draw.
    world*       shown   = active_world(state);
    spin_preview preview = { 0 };
    if (!play_active(&state->play))
        preview = spin_preview_begin(shown, (f32)window_get_time());
    world_update(shown);

    scene_render_settings settings = build_render_settings(state);
    scene_renderer_draw_to(&state->renderer, shown, &state->cam, &settings, state->view_target);

    // GL's rows run bottom-up: flip V.
    ImDrawList*    draw    = igGetWindowDrawList();
    rhi_texture    color   = rhi_framebuffer_get_color_texture(state->view_target, 0);
    ImTextureRef_c texture = { NULL, (ImTextureID)rhi_texture_native_handle(color) };
    ImDrawList_AddImage(draw, texture, pos, (ImVec2_c){ pos.x + state->view_width, pos.y + state->view_height },
                        (ImVec2_c){ 0.0f, 1.0f }, (ImVec2_c){ 1.0f, 0.0f }, 0xFFFFFFFF);

    draw_selection(state, draw);
    spin_preview_end(shown, &preview);
    draw_play_frame(state, draw);

    // Handles on the selection, at its saved pose (after any Spin preview).
    draw_gizmo(state, draw, hovered, view_item);
    igEnd();
}

static void on_frame(App* app, f32 dt)
{
    editor_state* state = (editor_state*)app->user_data;

    // Smoothed like the GPU timings (GpuProfiler.c), to compare with them.
    f32 frame_ms        = dt * 1000.0f;
    state->cpu_frame_ms = state->cpu_frame_ms > 0.0f ? state->cpu_frame_ms + (frame_ms - state->cpu_frame_ms) * 0.1f : frame_ms;

    validate_selection(state);

    // Picks up edits to pbr.vert/pbr.frag saved while the app runs.
    scene_renderer_poll_hot_reload(&state->renderer);

    // Requests from other processes -- before rendering, so their changes
    // show this same frame.
    server_poll(state->server, active_commands(state));
    chat_poll(&state->chat);
    watch_agent(state);
    watch_export_check(state);

    gizmo_begin_frame();
    ibl_cache_sync(&state->ibl, state->doc.env.hdri);

    // App/on_frame runs between imgui_layer_new_frame() and
    // imgui_layer_render() (see App.c), so any ig*() calls here just work.
    // The menu bar first, so the dockspace fits under it. dockspace_id=0
    // auto-generates an ID; NULL viewport = the main viewport. The scene
    // lives in the Viewport panel, so the dockspace draws its own
    // background where no panel is.
    draw_main_menu(state);
    ImGuiID dockspace = igDockSpaceOverViewport(0, NULL, 0, NULL);
    default_layout(dockspace);
    draw_viewport(state, dockspace, dt);

    igBegin("Scene", NULL, 0);

    igText("%s", state->project.name);
    igTextDisabled("%s", paths_project_root());
    igText("Scene: %s%s", state->doc.name[0] ? state->doc.name : "untitled", doc_dirty(&state->doc) ? " (unsaved)" : "");
    if (igButton("Save (Ctrl+S)", (ImVec2_c){ 0, 0 }))
        save_scene(state);
    igSeparator();

    if (state->server)
        igTextDisabled("Command server: port %u, %u client(s)", (unsigned)server_port(state->server),
                       server_client_count(state->server));

    igSeparator();

    draw_add_controls(state);
    draw_environment_controls(state);

    igEnd();

    // Every entity and component of the scene, and the command console for it.
    validate_selection(state);   // commands this frame may have removed it
    command_context* active = active_commands(state);
    entity_panel_draw(active->w, "Entities", &state->selected, state->reveal_selection);
    inspector_draw(&state->inspector, active, &state->selected, "Inspector");
    state->reveal_selection = false;
    if (state->show_console)
        console_draw(&state->console, active, "Console (F1)");
    if (state->show_gpu_timings)
        draw_gpu_timings(state);

    if (chat_panel_draw(&state->chat_panel, &state->chat, active, "Chat", state->agent_note) == CHAT_PANEL_START_AGENT
        && state->server)
        start_agent(state);

    draw_popups(state);
    update_title(state);
}

// Runs from terminate(), before the GL context goes away.
static void on_shutdown(App* app)
{
    editor_state* state = (editor_state*)app->user_data;

    play_stop(state);                        // the copy holds asset references too
    chat_shutdown(&state->chat);             // closes a running turn's transaction
    server_stop(state->server);              // the agent (--exit-with-app) leaves when its connection closes...
    process_destroy(state->agent_process);   // ...or is ended here
    finish_edits(state);                     // an edit still open becomes its own step
    inspector_shutdown(&state->inspector);
    console_shutdown(&state->console);
    doc_shutdown(&state->doc);               // releases the scene's assets...
    assets_shutdown(&state->assets);     // ...then the models themselves
    ibl_cache_destroy(&state->ibl);
    if (state->view_target)
        rhi_framebuffer_destroy(state->view_target);
    reset_export(state);                     // ends a running check, frees its picture
    scene_renderer_destroy(&state->renderer);
}

int main(int argc, char** argv)
{
    // --project <dir>: the project to open (a folder with project.json).
    // Without it, the sample project: the source tree's assets/.
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--project") == 0 && i + 1 < argc)
        {
            if (!paths_set_project_root(argv[++i]))
            {
                fprintf(stderr, "--project: %s is not a folder\n", argv[i]);
                return 1;
            }
        }
        else
        {
            fprintf(stderr, "usage: Editor [--project <dir>]\n");
            return 1;
        }
    }

    project_settings check;
    char             error[640];
    if (!project_read(paths_project_root(), &check, error, sizeof(error)))
    {
        fprintf(stderr, "%s\nPass --project with a folder that has a project.json.\n", error);
        return 1;
    }

    // GPU resources (renderer, IBL, models) can only be created after RHI
    // is initialized, which happens inside run() -- on_init creates them,
    // on_shutdown destroys them. Static: the state holds large buffers.
    static editor_state state;

    App app = {
        .name             = "PBR Editor",
        // Maximized: a fixed 1920 x 1080 window on a 1920 x 1080 screen ran
        // under the taskbar, hiding the bottom of every docked panel -- where
        // the Console's and the Chat's input boxes are. The size is what
        // un-maximizing restores to.
        .maximized        = true,
        .width            = 1600,
        .height           = 900,
        .on_init          = on_init,
        .on_frame         = on_frame,
        .on_shutdown      = on_shutdown,
        .on_key           = on_key,
        .on_close_request = on_close_request,
        .user_data        = &state,
        .should_close     = false,
        .ui               = imgui_layer()
    };

    run(&app);
    terminate(&app);

    return 0;
}
