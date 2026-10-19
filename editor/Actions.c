// editor/Actions.c
//
// What the menus, keys and panels ask for. Everything that changes the
// scene is a command (run_command()), so it is validated, reaches connected
// clients and lands in the undo history like an edit from the console or
// the agent.

#include "Editor.h"

#include "Core/FileIO.h"
#include "Core/Paths.h"
#include "Platform/Window.h"
#include "Renderer/Renderer.h"
#include "World/Serialize.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Running commands for the UI ----------------------------------------------------

cJSON*
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

// Runs a command whose result isn't needed; true if it succeeded.
static b8
run_ok(editor_state* state, const char* cmd, cJSON* args)
{
    cJSON* result = run_command(state, cmd, args);
    cJSON_Delete(result);
    return result != NULL;
}

// One undo step around several commands -- when the scene on screen has a
// history (Play's copy has none).
static b8
begin_step(editor_state* state, const char* label)
{
    if (!active_commands(state)->history)
        return false;
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "label", label);
    return run_ok(state, "history.begin", args);
}

static void
end_step(editor_state* state, b8 begun)
{
    if (begun)
        run_ok(state, "history.commit", NULL);
}

// --- Selection -------------------------------------------------------------------

void
validate_selection(editor_state* state)
{
    if (!world_is_alive(active_world(state), state->selected))
        state->selected = ENTITY_NULL;
}

void
select_entity(editor_state* state, entity_id e)
{
    state->selected         = e;
    state->reveal_selection = e != ENTITY_NULL;
}

// Selects the entity a create/place result names (and frees the result).
static void
select_result(editor_state* state, cJSON* result)
{
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(result, "name"));
    if (name)
        select_entity(state, world_find(active_world(state), name));
    cJSON_Delete(result);
}

void
finish_edits(editor_state* state)
{
    inspector_finish_edit(&state->inspector, active_commands(state));
    gizmo_finish_edit(state, active_commands(state));
}

// --- Undo / redo ------------------------------------------------------------------

// history.undo / history.redo on the edited scene; the reason in the console
// if there's nothing to do. False if it didn't happen.
static b8
undo_or_redo(editor_state* state, b8 redo)
{
    if (play_active(&state->play))
    {
        console_info(&state->console, "No undo while playing: Stop (Ctrl+P) discards everything Play changed.");
        return false;
    }
    finish_edits(state);

    char*       reply   = cmd_execute_text(&state->doc.commands, redo ? "{\"cmd\":\"history.redo\"}" : "{\"cmd\":\"history.undo\"}");
    cJSON*      parsed  = cJSON_Parse(reply);
    b8          ok      = cJSON_IsTrue(cJSON_GetObjectItem(parsed, "ok"));
    const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(parsed, "error"), "message"));
    if (message)
        console_info(&state->console, message);
    cJSON_Delete(parsed);
    cJSON_free(reply);
    doc_follow_history(&state->doc);   // undone past a load: the old scene's name again
    return ok;
}

void undo(editor_state* state) { undo_or_redo(state, false); }
void redo(editor_state* state) { undo_or_redo(state, true); }

void
history_jump(editor_state* state, uint32 cursor)
{
    const command_history* h = &state->doc.history;
    while (h->cursor > cursor && undo_or_redo(state, false))
        ;
    while (h->cursor < cursor && undo_or_redo(state, true))
        ;
}

// --- Duplicate / delete / reparent ---------------------------------------------------

// Creates a copy of `e` under `parent_name` (NULL = the scene root), then
// copies of its children under the copy. Moves the top copy by `offset` (its
// parent's space). Returns the copy, or ENTITY_NULL.
static entity_id
duplicate_subtree(editor_state* state, entity_id e, const char* parent_name, f32 offset)
{
    world* w    = active_world(state);
    cJSON* json = serialize_entity(w, e);
    if (!json)
        return ENTITY_NULL;

    cJSON* components = cJSON_DetachItemFromObject(json, "components");
    cJSON* transform  = cJSON_GetObjectItem(components, "Transform");
    cJSON* position   = cJSON_GetObjectItem(transform, "position");
    if (offset != 0.0f && cJSON_IsArray(position) && cJSON_IsNumber(cJSON_GetArrayItem(position, 0)))
    {
        cJSON* x = cJSON_GetArrayItem(position, 0);
        cJSON_SetNumberValue(x, x->valuedouble + offset);
    }

    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "name", world_name(w, e));   // taken: the copy gets _2, _3...
    if (parent_name)
        cJSON_AddStringToObject(args, "parent", parent_name);
    cJSON_AddItemToObject(args, "components", components);
    cJSON_Delete(json);

    cJSON*      result = run_command(state, "entity.create", args);
    const char* name   = cJSON_GetStringValue(cJSON_GetObjectItem(result, "name"));
    entity_id   copy   = name ? world_find(w, name) : ENTITY_NULL;
    cJSON_Delete(result);
    if (!copy)
        return ENTITY_NULL;

    // The children, found before any copy joins the world (copies are
    // parented to `copy`, never to `e`, so they don't show up here anyway).
    char copy_name[ENTITY_NAME_MAX];
    snprintf(copy_name, sizeof(copy_name), "%s", world_name(w, copy));
    const uint32 mask = COMPONENT_BIT(COMPONENT_PARENT);
    for (entity_id child = world_next(w, ENTITY_NULL, mask); child; child = world_next(w, child, mask))
    {
        if (world_parent(w, child) == e)
            duplicate_subtree(state, child, copy_name, 0.0f);
    }
    return copy;
}

void
duplicate_selection(editor_state* state)
{
    validate_selection(state);
    world* w = active_world(state);
    if (!state->selected)
        return;
    finish_edits(state);

    // Beside the original: one bounding box's width along x (1 m for things
    // without bounds), in the parent's space.
    vec3 lo, hi;
    f32  offset = world_bounds(w, state->selected, lo, hi) ? glm_max(hi[0] - lo[0], 0.1f) : 1.0f;

    char label[ENTITY_NAME_MAX + 16];
    snprintf(label, sizeof(label), "duplicate %s", world_name(w, state->selected));
    b8 begun = begin_step(state, label);

    entity_id   parent = world_parent(w, state->selected);
    entity_id   copy   = duplicate_subtree(state, state->selected, parent ? world_name(w, parent) : NULL, offset);
    end_step(state, begun);

    if (copy)
        select_entity(state, copy);
}

void
delete_selection(editor_state* state)
{
    validate_selection(state);
    if (!state->selected)
        return;
    finish_edits(state);

    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "entity", world_name(active_world(state), state->selected));
    if (run_ok(state, "entity.destroy", args))
        state->selected = ENTITY_NULL;
}

void
reparent(editor_state* state, entity_id child, entity_id parent)
{
    world* w = active_world(state);
    if (!world_is_alive(w, child) || (parent && !world_is_alive(w, parent)) || world_parent(w, child) == parent)
        return;
    finish_edits(state);

    // Where it is in the world stays put: local = inverse(new parent) * world.
    mat4 local;
    if (parent)
    {
        mat4 parent_inverse;
        glm_mat4_inv((vec4*)world_matrix(w, parent), parent_inverse);
        glm_mat4_mul(parent_inverse, (vec4*)world_matrix(w, child), local);
    }
    else
    {
        glm_mat4_copy((vec4*)world_matrix(w, child), local);
    }
    c_transform t;
    transform_from_matrix(local, &t);
    for (int axis = 0; axis < 3; ++axis)
        t.scale[axis] = glm_max(t.scale[axis], 0.0001f);

    char label[ENTITY_NAME_MAX * 2 + 16];
    snprintf(label, sizeof(label), "parent %s to %s", world_name(w, child), parent ? world_name(w, parent) : "the scene root");
    b8 begun = begin_step(state, label);

    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "entity", world_name(w, child));
    if (parent)
        cJSON_AddStringToObject(args, "parent", world_name(w, parent));
    else
        cJSON_AddNullToObject(args, "parent");
    if (run_ok(state, "entity.update", args))
    {
        cJSON* fields = cJSON_CreateObject();
        cJSON_AddItemToObject(fields, "position", cJSON_CreateFloatArray(t.position, 3));
        cJSON_AddItemToObject(fields, "rotation_deg", cJSON_CreateFloatArray(t.rotation_deg, 3));
        cJSON_AddItemToObject(fields, "scale", cJSON_CreateFloatArray(t.scale, 3));

        cJSON* set = cJSON_CreateObject();
        cJSON_AddStringToObject(set, "entity", world_name(w, child));
        cJSON_AddStringToObject(set, "component", "Transform");
        cJSON_AddItemToObject(set, "fields", fields);
        run_ok(state, "component.set", set);
    }
    end_step(state, begun);
    select_entity(state, child);
}

// --- Framing and cameras ---------------------------------------------------------------

void
frame_view(editor_state* state, const char* entity)
{
    cJSON* args = cJSON_CreateObject();
    if (entity)
        cJSON_AddStringToObject(args, "entity", entity);

    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", "view.frame");
    cJSON_AddItemToObject(request, "args", args);
    cJSON* response = cmd_execute(active_commands(state), request);

    const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "message"));
    if (message && entity)
        console_info(&state->console, message);
    cJSON_Delete(response);
    cJSON_Delete(request);
}

void
frame_selection(editor_state* state)
{
    validate_selection(state);
    if (state->selected)
        frame_view(state, world_name(active_world(state), state->selected));
}

void
use_editor_camera(editor_state* state)
{
    // Carry on from the Camera entity's view, orbiting a point in front of it.
    if (state->view_cam_entity)
    {
        vec3 forward;
        glm_vec3_sub(state->view_cam.target, state->view_cam.position, forward);
        glm_vec3_normalize(forward);
        f32 distance = glm_clamp(state->orbit.distance, 2.0f, 20.0f);

        glm_vec3_copy(state->view_cam.position, state->cam.position);
        glm_vec3_copy(state->view_cam.position, state->cam.target);
        glm_vec3_muladds(forward, distance, state->cam.target);
        orbit_from_camera(&state->orbit, &state->cam);
        orbit_apply(&state->orbit, &state->cam);
    }
    state->look_through     = ENTITY_NULL;
    state->play_game_camera = false;
    state->view_cam_entity  = ENTITY_NULL;
}

// --- Play mode ----------------------------------------------------------------------

// Not while a chat turn runs: its transaction is open on the edited scene,
// and its remaining tool calls would land in the copy.
void
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

    console_info(&state->console, state->play_game_camera
                                      ? "Playing through the scene's camera. Changes made now are discarded on Stop (Ctrl+P)."
                                      : "Playing. Changes made now are discarded on Stop (Ctrl+P).");
}

void
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

// --- Project and scene ------------------------------------------------------------------

void
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
    state->selected     = ENTITY_NULL;
    state->look_through = ENTITY_NULL;   // a Camera of the old scene

    announce_replaced(state, revision, source);
    frame_view(state, NULL);
    return true;
}

b8
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

void
new_scene(editor_state* state)
{
    cJSON* starter = project_starter_scene(&state->project);
    replace_scene(state, starter, "", "new scene");
    cJSON_Delete(starter);
}

// Saves to scenes/<name>.json (scene.save: the entities and the environment).
b8
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
    if (!run_ok(state, "scene.save", args))   // on_scene_file() marks the document saved
    {
        snprintf(state->popup_error, sizeof(state->popup_error), "Couldn't save; the console says why.");
        return false;
    }

    char line[PROJECT_SCENE_MAX + 32];
    snprintf(line, sizeof(line), "Saved scene %s", name);
    console_info(&state->console, line);
    return true;
}

// Ctrl+S: to the scene's file, or Save As for a scene that has none yet.
void
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
b8
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
    input_actions_load(&state->project.input);
    project_recent_add(&state->recent, paths_project_root(), NULL);

    char line[PROJECT_PATH_MAX + PROJECT_NAME_MAX + 32];
    snprintf(line, sizeof(line), "Opened project %s (%s)", settings.name, paths_project_root());
    console_info(&state->console, line);
    chat_info(&state->chat, line);

    if (!open_scene(state, settings.startup_scene))
        new_scene(state);
    return true;
}

void
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

void
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

void
add_model(editor_state* state, const char* key)
{
    vec3 position;
    add_position(state, 0.0f, position);

    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "model", key);
    cJSON_AddNumberToObject(args, "size", state->add_size);
    cJSON_AddItemToObject(args, "position", cJSON_CreateFloatArray(position, 3));
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
    cJSON_AddItemToObject(transform, "position", cJSON_CreateFloatArray(position, 3));
    if (rotation_deg)
        cJSON_AddItemToObject(transform, "rotation_deg", cJSON_CreateFloatArray(rotation_deg, 3));
    if (component)
        cJSON_AddItemToObject(components, component, fields);

    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "name", name);
    cJSON_AddItemToObject(args, "components", components);

    cJSON* result = run_command(state, "entity.create", args);
    if (result)
        select_result(state, result);
}

void
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
        cJSON_AddItemToObject(light, "direction", cJSON_CreateFloatArray(down, 3));
        cJSON_AddNumberToObject(light, "inner_cone_degrees", 15);
        cJSON_AddNumberToObject(light, "outer_cone_degrees", 30);
    }
    else if (sun)
    {
        const f32 slant[3] = { -0.4f, -0.7f, -0.5f };
        cJSON_AddItemToObject(light, "direction", cJSON_CreateFloatArray(slant, 3));
    }

    char name[32];
    snprintf(name, sizeof(name), "%s_light", sun ? "sun" : type);
    add_entity(state, name, position, NULL, "Light", light);
}

// A Camera entity seeing what the editor's view sees. Its -Z axis has to
// point from the eye at the orbit target: with rotation Ry(yaw) * Rx(pitch)
// that is pitch = -orbit.pitch, yaw = orbit.yaw.
void
add_camera(editor_state* state)
{
    const f32 rotation[3] = { -state->orbit.pitch, state->orbit.yaw, 0.0f };

    cJSON* cam = cJSON_CreateObject();
    cJSON_AddNumberToObject(cam, "fov_degrees", state->cam.fov_degrees);
    add_entity(state, "camera", state->cam.position, rotation, "Camera", cam);
}

// A room-sized box around the view's centre, resting on the ground; the
// probes re-bake by themselves once it exists.
void
add_reflection_probe(editor_state* state)
{
    const f32 extents[3] = { 4.0f, 2.0f, 4.0f };
    vec3      position;
    add_position(state, extents[1], position);

    cJSON* probe = cJSON_CreateObject();
    cJSON_AddItemToObject(probe, "extents", cJSON_CreateFloatArray(extents, 3));
    add_entity(state, "reflection_probe", position, NULL, "ReflectionProbe", probe);
}

void
add_empty(editor_state* state)
{
    vec3 position;
    add_position(state, 0.0f, position);
    add_entity(state, "empty", position, NULL, NULL, NULL);
}
