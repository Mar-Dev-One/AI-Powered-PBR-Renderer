// editor/HostCommands.c
//
// Commands for state the World doesn't hold -- the camera, the HDRI, the
// renderer settings, the log, Play, the project and Export -- registered with
// the command context, so the console (and the agent, and scripts over the
// command server) can drive them like any built-in command. Each mirrors a
// panel or a toolbar control: whatever the GUI can set, a command can too.

#include "Editor.h"

#include "Core/Encoding.h"
#include "Core/FileIO.h"
#include "Core/Image.h"
#include "Core/Paths.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- HDRI list ------------------------------------------------------------------

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

void
list_hdris(hdri_list* out)
{
    out->count = 0;
    char*      folder = project_path("hdri");
    hdri_visit visit  = { out, "hdri/", false };
    file_list_directory(folder, visit_hdri, &visit);
    free(folder);
}

// --- Helpers ---------------------------------------------------------------------

void
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

void
on_scene_file(void* user_data, command_context* ctx, cmd_scene_file_op op, const char* name)
{
    editor_state* state = user_data;
    if (ctx != &state->doc.commands)
        return;   // Play's copy: never saved, and loading into it renames nothing
    doc_scene_file(&state->doc, op, name);
    if (op == CMD_SCENE_LOADED)
        state->look_through = ENTITY_NULL;   // a Camera of the scene that was replaced
}

cJSON*
number_json(f32 value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.4g", (double)value);
    return cJSON_CreateNumber(strtod(buffer, NULL));
}

cJSON*
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

void
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

// --- Environment -------------------------------------------------------------------

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

    // A path from environment.list, or just its file name; null for none.
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
        snprintf(error, error_size, "\"hdri\" must be a path from environment.list, or null for none");
        return false;
    }

    const cJSON* blur = cJSON_GetObjectItem(args, "sky_blur");
    if (blur && (!cJSON_IsNumber(blur) || blur->valuedouble < 0.0 || blur->valuedouble > 1.0))
    {
        snprintf(error, error_size, "\"sky_blur\" must be a number from 0 (sharp) to 1 (blurriest)");
        return false;
    }

    if (found)
        snprintf(state->doc.env.hdri, sizeof(state->doc.env.hdri), "%s", found);
    else if (cJSON_IsNull(hdri))
        state->doc.env.hdri[0] = '\0';
    if (hdri)
        ibl_cache_sync(&state->ibl, state->doc.env.hdri);

    const cJSON* use_ibl = cJSON_GetObjectItem(args, "use_ibl");
    const cJSON* skybox  = cJSON_GetObjectItem(args, "show_skybox");
    if (cJSON_IsBool(use_ibl)) state->doc.env.use_ibl     = cJSON_IsTrue(use_ibl);
    if (cJSON_IsBool(skybox))  state->doc.env.show_skybox = cJSON_IsTrue(skybox);
    if (blur)                  state->doc.env.sky_blur    = (f32)blur->valuedouble;

    return host_environment_list(user_data, ctx, NULL, out_result, error, error_size);
}

// --- View ---------------------------------------------------------------------------

static b8
host_view_get(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;

    *out_result = cJSON_CreateObject();
    cJSON_AddItemToObject(*out_result, "position", vec3_json(state->cam.position));
    cJSON_AddItemToObject(*out_result, "target", vec3_json(state->cam.target));
    cJSON_AddItemToObject(*out_result, "fov_degrees", number_json(state->cam.fov_degrees));
    if (state->view_cam_entity && world_is_alive(ctx->w, state->view_cam_entity))
        cJSON_AddStringToObject(*out_result, "looking_through", world_name(ctx->w, state->view_cam_entity));
    else
        cJSON_AddNullToObject(*out_result, "looking_through");
    cJSON_AddItemToObject(*out_result, "exposure", number_json(state->doc.env.exposure));
    cJSON_AddItemToObject(*out_result, "ambient_strength", number_json(state->doc.env.ambient_strength));
    cJSON_AddItemToObject(*out_result, "bloom", number_json(state->doc.env.bloom));
    cJSON_AddBoolToObject(*out_result, "auto_exposure", state->doc.env.auto_exposure);
    cJSON_AddBoolToObject(*out_result, "ssao", state->doc.env.ssao);
    cJSON_AddItemToObject(*out_result, "ssao_radius", number_json(state->doc.env.ssao_radius));
    cJSON_AddStringToObject(*out_result, "tonemap", scene_tonemap_name(state->doc.env.tonemap));
    cJSON_AddBoolToObject(*out_result, "shadows", state->shadows);
    cJSON_AddItemToObject(*out_result, "cascade_split_lambda", number_json(state->cascade_split_lambda));
    cJSON_AddStringToObject(*out_result, "debug_view", scene_debug_view_name(state->debug_view));
    cJSON_AddStringToObject(*out_result, "antialiasing", scene_antialiasing_name(state->antialiasing));
    return true;
}

// The editor camera is moving: stop looking through a Camera entity.
static void
leave_entity_camera(editor_state* state)
{
    state->look_through     = ENTITY_NULL;
    state->play_game_camera = false;
    state->view_cam_entity  = ENTITY_NULL;
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

    leave_entity_camera(state);
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

    leave_entity_camera(state);
    orbit_fit(&state->orbit, &state->cam, bmin, bmax, 1.1f);
    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

// Looks through a Camera entity (what the player sees from it), or back
// through the editor's own camera.
static b8
host_view_look_through(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error,
                       size_t error_size)
{
    editor_state* state  = user_data;
    const cJSON*  target = cJSON_GetObjectItem(args, "camera");

    if (!target || cJSON_IsNull(target))
    {
        use_editor_camera(state);
    }
    else
    {
        entity_id e = cJSON_IsString(target) ? world_find(ctx->w, target->valuestring) : ENTITY_NULL;
        if (!e || !world_has(ctx->w, e, COMPONENT_CAMERA))
        {
            snprintf(error, error_size, "\"camera\" must name an entity with a Camera component, or be null for the "
                                        "editor camera");
            return false;
        }
        state->look_through = e;
    }
    update_view_camera(state);
    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

// --- Rendering -------------------------------------------------------------------------

static b8
host_render_set(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state = user_data;

    const cJSON* exposure = cJSON_GetObjectItem(args, "exposure");
    const cJSON* ambient  = cJSON_GetObjectItem(args, "ambient_strength");
    const cJSON* bloom    = cJSON_GetObjectItem(args, "bloom");
    const cJSON* cascades = cJSON_GetObjectItem(args, "debug_show_cascades");
    const cJSON* unclust  = cJSON_GetObjectItem(args, "debug_unclustered");
    const cJSON* view     = cJSON_GetObjectItem(args, "debug_view");
    const cJSON* aa       = cJSON_GetObjectItem(args, "antialiasing");
    const cJSON* tonemap  = cJSON_GetObjectItem(args, "tonemap");
    const cJSON* auto_exp = cJSON_GetObjectItem(args, "auto_exposure");
    const cJSON* ssao     = cJSON_GetObjectItem(args, "ssao");
    const cJSON* radius   = cJSON_GetObjectItem(args, "ssao_radius");
    const cJSON* shadows  = cJSON_GetObjectItem(args, "shadows");
    const cJSON* lambda   = cJSON_GetObjectItem(args, "cascade_split_lambda");

    if ((exposure && (!cJSON_IsNumber(exposure) || exposure->valuedouble <= 0.0 || exposure->valuedouble > 16.0))
        || (ambient && (!cJSON_IsNumber(ambient) || ambient->valuedouble < 0.0 || ambient->valuedouble > 4.0))
        || (bloom && (!cJSON_IsNumber(bloom) || bloom->valuedouble < 0.0 || bloom->valuedouble > 1.0))
        || (cascades && !cJSON_IsBool(cascades)) || (unclust && !cJSON_IsBool(unclust))
        || (auto_exp && !cJSON_IsBool(auto_exp)) || (ssao && !cJSON_IsBool(ssao)) || (shadows && !cJSON_IsBool(shadows))
        || (radius && (!cJSON_IsNumber(radius) || radius->valuedouble < 0.05 || radius->valuedouble > 5.0))
        || (lambda && (!cJSON_IsNumber(lambda) || lambda->valuedouble < 0.0 || lambda->valuedouble > 1.0)))
    {
        snprintf(error, error_size, "exposure must be in (0, 16], ambient_strength in [0, 4], bloom in [0, 1], "
                                    "ssao_radius in [0.05, 5], cascade_split_lambda in [0, 1]; shadows, "
                                    "debug_show_cascades, debug_unclustered, auto_exposure and ssao true/false");
        return false;
    }

    scene_antialiasing aa_value = state->antialiasing;
    if (aa && !scene_antialiasing_from_name(cJSON_GetStringValue(aa), &aa_value))
    {
        snprintf(error, error_size, "antialiasing must be off, fxaa or taa");
        return false;
    }

    scene_tonemap tonemap_value = state->doc.env.tonemap;
    if (tonemap && !scene_tonemap_from_name(cJSON_GetStringValue(tonemap), &tonemap_value))
    {
        snprintf(error, error_size, "tonemap must be aces, agx or neutral");
        return false;
    }

    scene_debug_view debug_view = state->debug_view;
    if (view && !scene_debug_view_from_name(cJSON_GetStringValue(view), &debug_view))
    {
        snprintf(error, error_size, "debug_view must be one of: lit, albedo, normals, roughness, metallic, ao, shadow, "
                                    "lighting, wireframe, lights");
        return false;
    }

    if (exposure) state->doc.env.exposure         = (f32)exposure->valuedouble;
    if (ambient)  state->doc.env.ambient_strength = (f32)ambient->valuedouble;
    if (bloom)    state->doc.env.bloom            = (f32)bloom->valuedouble;
    state->doc.env.tonemap = tonemap_value;
    if (auto_exp) state->doc.env.auto_exposure = cJSON_IsTrue(auto_exp);
    if (ssao)     state->doc.env.ssao          = cJSON_IsTrue(ssao);
    if (radius)   state->doc.env.ssao_radius   = (f32)radius->valuedouble;
    if (cascades) state->debug_show_cascades   = cJSON_IsTrue(cascades);
    if (unclust)  state->debug_unclustered     = cJSON_IsTrue(unclust);
    if (shadows)  state->shadows               = cJSON_IsTrue(shadows);
    if (lambda)   state->cascade_split_lambda  = (f32)lambda->valuedouble;
    state->antialiasing = aa_value;
    state->debug_view   = debug_view;

    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

// Captures the reflection probes again (Renderer/ReflectionProbes.h) -- they
// re-bake by themselves only when a probe or the environment changes.
static b8
host_render_bake_reflections(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error,
                             size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;
    scene_renderer_bake_probes(&state->renderer);

    *out_result = cJSON_CreateObject();
    cJSON_AddNumberToObject(*out_result, "probes", state->renderer.probes.count);
    cJSON_AddStringToObject(*out_result, "note", "Baked at the next frame. Probes also re-bake by themselves when one "
                                                 "moves or resizes, or the environment changes.");
    return true;
}

// Rebuilds every shader now (they also reload by themselves when their files
// change); a shader that fails keeps its old program, and log.get says why.
static b8
host_render_reload_shaders(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error,
                           size_t error_size)
{
    (void)ctx; (void)args;
    editor_state* state = user_data;
    if (!scene_renderer_reload_shaders(&state->renderer))
    {
        snprintf(error, error_size, "a shader failed to build and keeps its previous program; log.get "
                                    "with min_level \"error\" says which and why");
        return false;
    }
    ibl_cache_invalidate(&state->ibl);   // the bake and skybox shaders, re-baked at the next sync
    *out_result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*out_result, "reloaded", true);
    return true;
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

// What the renderer did with the scene in the last frame: how many lights,
// which got shadow maps, how many probes -- the Renderer panel's numbers.
static b8
host_render_stats(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state*         state = user_data;
    const scene_renderer* r     = &state->renderer;

    *out_result = cJSON_CreateObject();
    cJSON_AddNumberToObject(*out_result, "lights", r->clusters.count);
    cJSON_AddNumberToObject(*out_result, "lights_everywhere", r->clusters.global_count);
    cJSON_AddNumberToObject(*out_result, "cluster_light_indices", r->clusters.index_count);
    cJSON_AddNumberToObject(*out_result, "cluster_light_indices_max", CLUSTER_MAX_INDICES);
    cJSON_AddBoolToObject(*out_result, "sun_shadows", r->shadowed);
    cJSON_AddNumberToObject(*out_result, "shadow_cascades", r->csm.cascade_count);
    cJSON_AddNumberToObject(*out_result, "spot_shadows", r->spot.count);
    cJSON_AddNumberToObject(*out_result, "spot_shadows_redrawn", r->spot.rendered);
    cJSON_AddNumberToObject(*out_result, "point_shadows", r->point.count);
    cJSON_AddNumberToObject(*out_result, "point_shadows_redrawn", r->point.rendered);
    cJSON_AddNumberToObject(*out_result, "reflection_probes", r->probes.count);
    cJSON_AddNumberToObject(*out_result, "view_width", state->view_target_width);
    cJSON_AddNumberToObject(*out_result, "view_height", state->view_target_height);
    return true;
}

// The latest log lines (what View > Log shows), e.g. to see why a shader
// or a model failed.
static b8
host_log_get(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx;
    editor_state*    state     = user_data;
    const log_panel* log       = &state->log;
    const cJSON*     count_arg = cJSON_GetObjectItem(args, "count");
    const cJSON*     level_arg = cJSON_GetObjectItem(args, "min_level");

    int count = cJSON_IsNumber(count_arg) ? count_arg->valueint : 50;
    if (count_arg && (!cJSON_IsNumber(count_arg) || count < 1 || count > LOG_PANEL_MAX_ENTRIES))
    {
        snprintf(error, error_size, "count must be 1..%d", LOG_PANEL_MAX_ENTRIES);
        return false;
    }

    log_level min_level = LOG_LEVEL_TRACE;
    if (level_arg)
    {
        const char* name  = cJSON_GetStringValue(level_arg);
        b8          found = false;
        for (int l = LOG_LEVEL_TRACE; l <= LOG_LEVEL_FATAL && name && !found; ++l)
            if (strcmp(name, log_level_name((log_level)l)) == 0)
                min_level = (log_level)l, found = true;
        if (!found)
        {
            snprintf(error, error_size, "min_level must be trace, debug, info, warn, error or fatal");
            return false;
        }
    }

    // The newest `count` matching lines, oldest first.
    uint32 first = log->entry_count, matched = 0;
    while (first > 0 && matched < (uint32)count)
        if (log_panel_entry_at(log, --first)->level >= min_level)
            ++matched;

    *out_result = cJSON_CreateObject();
    cJSON* entries = cJSON_AddArrayToObject(*out_result, "entries");
    for (uint32 i = first; i < log->entry_count; ++i)
    {
        const log_panel_entry* entry = log_panel_entry_at(log, i);
        if (entry->level < min_level)
            continue;
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "time", entry->time);
        cJSON_AddStringToObject(item, "level", log_level_name(entry->level));
        cJSON_AddStringToObject(item, "text", entry->text);
        cJSON_AddItemToArray(entries, item);
    }
    return true;
}

// What the scene looks like right now, as a PNG -- rendered fresh,
// without the editor's panels, so a model can see its own work. Through the
// camera the Viewport is using.
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
    settings.delta_time            = 0.0f;   // auto-exposure as on screen, not adapted a step further

    world_update(ctx->w);
    update_view_camera(state);

    uint16 width, height;
    uint8* rgba = scene_renderer_capture(&state->renderer, ctx->w, &state->view_cam, &settings, &width, &height);
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

// --- Project ----------------------------------------------------------------------------

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

// --- Input ------------------------------------------------------------------------------

static b8
host_input_get(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;

    *out_result = input_action_map_to_json(&state->project.input);
    cJSON_AddBoolToObject(*out_result, "custom", state->project.input.custom);
    return true;
}

// Changes the project's actions and saves project.json straight away, as the
// Project Settings dialog's Save does.
static b8
host_input_set(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state = user_data;

    const cJSON* action_arg = cJSON_GetObjectItem(args, "action");
    const cJSON* type_arg   = cJSON_GetObjectItem(args, "type");
    const cJSON* bindings   = cJSON_GetObjectItem(args, "bindings");
    const cJSON* remove     = cJSON_GetObjectItem(args, "remove");
    const cJSON* reset      = cJSON_GetObjectItem(args, "reset");
    const cJSON* dead_zone  = cJSON_GetObjectItem(args, "stick_dead_zone");
    const char*  name       = cJSON_GetStringValue(action_arg);

    if ((action_arg && !name) || (type_arg && !cJSON_IsString(type_arg)) || (bindings && !cJSON_IsArray(bindings))
        || (remove && !cJSON_IsBool(remove)) || (reset && !cJSON_IsBool(reset)) || (dead_zone && !cJSON_IsNumber(dead_zone)))
    {
        snprintf(error, error_size, "\"action\" and \"type\" are text, \"bindings\" a list of text, \"remove\" and "
                                    "\"reset\" true or false, \"stick_dead_zone\" a number");
        return false;
    }
    if (!name && !cJSON_IsTrue(reset) && !dead_zone)
    {
        snprintf(error, error_size, "pass \"action\" (with \"type\", \"bindings\" or \"remove\"), \"reset\" or "
                                    "\"stick_dead_zone\"");
        return false;
    }
    if (!name && (type_arg || bindings || remove))
    {
        snprintf(error, error_size, "\"type\", \"bindings\" and \"remove\" need \"action\"");
        return false;
    }

    // Worked on a copy, so a mistake halfway changes nothing.
    static project_settings edited;
    edited = state->project;
    input_action_map* map = &edited.input;

    if (cJSON_IsTrue(reset))
        input_action_map_defaults(map);
    if (dead_zone)
    {
        if (dead_zone->valuedouble < 0.0 || dead_zone->valuedouble > 0.9)
        {
            snprintf(error, error_size, "\"stick_dead_zone\" is from 0 to 0.9");
            return false;
        }
        map->stick_dead_zone = (f32)dead_zone->valuedouble;
        map->custom          = true;
    }

    if (name)
    {
        int index = input_action_map_find(map, name);
        if (cJSON_IsTrue(remove))
        {
            if (index < 0)
            {
                snprintf(error, error_size, "there's no action \"%s\" (input.get lists them)", name);
                return false;
            }
            input_action_map_remove(map, index);
        }
        else
        {
            input_action_kind kind = index >= 0 ? map->actions[index].kind : INPUT_ACTION_BUTTON;
            if (type_arg && !input_action_kind_from_name(type_arg->valuestring, &kind))
            {
                snprintf(error, error_size, "\"type\" is \"button\", \"axis\" or \"delta\"");
                return false;
            }
            if (index < 0 && !type_arg)
            {
                snprintf(error, error_size, "\"%s\" is a new action: say its \"type\" (button, axis or delta)", name);
                return false;
            }
            index = input_action_map_add(map, name, kind);
            if (index < 0)
            {
                snprintf(error, error_size, input_action_name_valid(name)
                                                ? "there's no room for more actions"
                                                : "an action's name is 1-31 letters, digits or '_'");
                return false;
            }

            if (bindings)
            {
                map->actions[index].binding_count = 0;
                const cJSON* item;
                cJSON_ArrayForEach(item, bindings)
                {
                    input_binding binding;
                    char          reason[160];
                    if (!cJSON_IsString(item))
                    {
                        snprintf(error, error_size, "each binding is text like \"key:w\"");
                        return false;
                    }
                    if (!input_binding_from_text(item->valuestring, &binding, reason, sizeof(reason))
                        || !input_action_map_bind(map, index, binding, reason, sizeof(reason)))
                    {
                        snprintf(error, error_size, "%s", reason);
                        return false;
                    }
                }
            }
            else
            {
                // A new type can make a binding no longer fit (mouse motion off a delta axis).
                input_action_def* a = &map->actions[index];
                for (int i = 0; i < a->binding_count; ++i)
                    if (a->bindings[i].kind == INPUT_BINDING_MOUSE_MOTION && a->kind != INPUT_ACTION_AXIS_DELTA)
                    {
                        snprintf(error, error_size, "\"%s\" has mouse motion bindings, which only drive a delta axis: "
                                                    "pass new \"bindings\" too", name);
                        return false;
                    }
            }
        }
        map->custom = true;
    }

    if (!project_write(paths_project_root(), &edited, error, error_size))
        return false;
    state->project = edited;
    input_actions_load(&state->project.input);

    return host_input_get(user_data, ctx, NULL, out_result, error, error_size);
}

// --- Play ------------------------------------------------------------------------------

static b8
host_play_status(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    editor_state* state = user_data;

    *out_result = cJSON_CreateObject();
    cJSON_AddStringToObject(*out_result, "state", play_mode_name(state->play.mode));
    cJSON_AddItemToObject(*out_result, "time", number_json(state->play.time));
    cJSON_AddBoolToObject(*out_result, "game_camera", state->play_game_camera);
    return true;
}

// The Viewport toolbar's buttons, for scripts and the agent.
static b8
host_play_set(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    editor_state* state  = user_data;
    const char*   action = cJSON_GetStringValue(cJSON_GetObjectItem(args, "action"));
    const cJSON*  game   = cJSON_GetObjectItem(args, "game_camera");

    if (game && !cJSON_IsBool(game))
    {
        snprintf(error, error_size, "\"game_camera\" must be true or false");
        return false;
    }
    if (!action && !game)
    {
        snprintf(error, error_size, "pass \"action\" and/or \"game_camera\"");
        return false;
    }
    if (game)
        state->play_game_camera = cJSON_IsTrue(game);

    if (!action)
        ;
    else if (strcmp(action, "play") == 0)
    {
        play_start(state);
        if (!play_active(&state->play))
        {
            snprintf(error, error_size, "can't play while a chat turn is running");
            return false;
        }
    }
    else if (strcmp(action, "stop") == 0)
        play_stop(state);
    else if (strcmp(action, "pause") == 0 || strcmp(action, "resume") == 0)
    {
        b8 pause = action[0] == 'p';
        if ((pause && state->play.mode == PLAY_RUNNING) || (!pause && state->play.mode == PLAY_PAUSED))
            play_toggle_pause(&state->play);
    }
    else if (strcmp(action, "step") == 0)
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

// --- The table -------------------------------------------------------------------------

static const cmd_arg ARGS_EXPORT[] = {
    { "folder", "string", false, "Where to put the game: a new or empty folder, or an earlier export; "
                                 "default Documents/PBR Exports/<project name>" },
};

static const cmd_arg ARGS_PLAY_SET[] = {
    { "action",      "string",  false, "\"play\" (run a copy of the scene), \"pause\", \"resume\", \"step\" (one frame, "
                                       "while paused) or \"stop\" (drop the copy, back to the edited scene)" },
    { "game_camera", "boolean", false, "While playing, look through the scene's first Camera, as the exported game does "
                                       "(false: keep the editor's camera)" },
};

static const cmd_arg ARGS_INPUT_SET[] = {
    { "action",          "string",  false, "The action to change, add or remove, e.g. \"jump\" (1-31 letters, digits, '_')" },
    { "type",            "string",  false, "button (jump, fire), axis (-1..1: move_x) or delta (an amount a frame: "
                                            "look_x); needed for a new action" },
    { "bindings",        "array",   false, "Replaces the action's bindings: text like \"key:space\", \"keys:a/d\" "
                                            "(-1/+1), \"mouse:right\", \"pad:a\", \"pad_axis:left_y*-1\", "
                                            "\"mouse_motion:x*0.1\" (delta only); \"*n\" scales or flips" },
    { "remove",          "boolean", false, "Remove the action" },
    { "reset",           "boolean", false, "Every action back to the defaults (move, look, up_down, jump, sprint) first" },
    { "stick_dead_zone", "number",  false, "How far a stick moves before it counts, 0-0.9 (default 0.2)" },
};

static const cmd_arg ARGS_LOG_GET[] = {
    { "count",     "number", false, "How many lines, newest last (default 50, at most 500)" },
    { "min_level", "string", false, "Only lines at least this serious: trace (default), debug, info, warn, error, fatal" },
};

static const cmd_arg ARGS_SCREENSHOT[] = {
    { "max_size", "number", false, "Longest side in pixels, 64-4096 (default 768); the window's size if smaller" },
};

static const cmd_arg ARGS_ENVIRONMENT_SET[] = {
    { "hdri",        "string",  false, "HDRI path from environment.list; null for none (simple sky/ground ambient)" },
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

static const cmd_arg ARGS_LOOK_THROUGH[] = {
    { "camera", "entity", false, "Entity with a Camera component to view the scene through; null or omitted for the "
                                 "editor's own camera" },
};

static const cmd_arg ARGS_RENDER_SET[] = {
    { "exposure",             "number",  false, "Brightness multiplier before tone mapping, e.g. 1 (with "
                                                 "auto_exposure: brighter or darker than the automatic level)" },
    { "auto_exposure",        "boolean", false, "Adapt to the scene's brightness, as an eye does" },
    { "ssao",                 "boolean", false, "Ambient occlusion: creases, corners and contact areas get less sky "
                                                 "light (default true)" },
    { "ssao_radius",          "number",  false, "How far, in meters, nearby geometry darkens a point (default 0.5)" },
    { "ambient_strength",     "number",  false, "Strength of the image-based / ambient light, e.g. 0.5" },
    { "bloom",                "number",  false, "How much bright light spreads into its surroundings, 0 (none) to 1; "
                                                 "0.04 is subtle, 0.1 strong" },
    { "shadows",              "boolean", false, "Shadow maps for the sun, spot and point lights (default true; the "
                                                 "editor's view only, not saved)" },
    { "cascade_split_lambda", "number",  false, "Where the sun's shadow cascades split, 0 (evenly) to 1 (crowded near "
                                                 "the camera); default 0.5" },
    { "debug_show_cascades",  "boolean", false, "Tint shading by shadow cascade" },
    { "debug_unclustered",    "boolean", false, "Evaluate every light at every pixel, not just its cluster's -- "
                                                 "the reference clustered lighting must match" },
    { "antialiasing",         "string",  false, "How jagged edges are smoothed: fxaa (the default), taa (steadier, "
                                                 "a little softer), off" },
    { "tonemap",              "string",  false, "How light becomes screen color: aces (contrasty, the default), agx "
                                                 "(softer, bright colors keep their hue), neutral (materials show their "
                                                 "true color)" },
    { "debug_view",           "string",  false, "What the view shows: lit (normal), or one input to the shading -- "
                                                 "albedo, normals, roughness, metallic, ao, shadow, lighting (white "
                                                 "surfaces), wireframe, lights (how many lights each pixel shades)" },
};

static const cmd_arg ARGS_PROJECT_CREATE[] = {
    { "name",   "string", true,  "Project name, also its folder's name (none of \\ / : * ? \" < > |)" },
    { "folder", "string", false, "Folder to create it in; default Documents/PBR Projects" },
};

#define HOST_ARGS(array) array, (uint32)(sizeof(array) / sizeof((array)[0]))

static const cmd_host_command HOST_COMMANDS[] = {
    { "environment.list", "The project's HDRIs that can light the scene, and the current settings",
      NULL, 0, false, host_environment_list },
    { "environment.set", "Changes the HDRI and how it lights and backs the scene",
      HOST_ARGS(ARGS_ENVIRONMENT_SET), false, host_environment_set },
    { "view.get", "Where the camera is and what it looks at, and every render setting",
      NULL, 0, false, host_view_get },
    { "view.set_camera", "Moves the editor camera and/or what it looks at",
      HOST_ARGS(ARGS_SET_CAMERA), false, host_view_set_camera },
    { "view.frame", "Points the editor camera at an entity (or everything) so it fills the view",
      HOST_ARGS(ARGS_FRAME), false, host_view_frame },
    { "view.look_through", "Views the scene through a Camera entity (what a player sees from it), or the editor camera",
      HOST_ARGS(ARGS_LOOK_THROUGH), false, host_view_look_through },
    { "view.screenshot", "The scene as a PNG (base64), rendered now without the editor's panels -- to check how it looks",
      HOST_ARGS(ARGS_SCREENSHOT), false, host_view_screenshot },
    { "render.set", "Exposure, ambient strength, bloom, tone mapping, ambient occlusion, shadows and debug views",
      HOST_ARGS(ARGS_RENDER_SET), false, host_render_set },
    { "render.stats", "What the last frame drew: lights, shadow maps (and how many were redrawn rather than kept), "
                      "reflection probes, cluster use",
      NULL, 0, false, host_render_stats },
    { "render.timings", "How long each render pass takes on the GPU, and the frame on the CPU, in ms",
      NULL, 0, false, host_render_timings },
    { "render.bake_reflections", "Captures the reflection probes again -- after lights or objects moved",
      NULL, 0, false, host_render_bake_reflections },
    { "render.reload_shaders", "Rebuilds every shader from its files now (a failing one keeps its old program)",
      NULL, 0, false, host_render_reload_shaders },
    { "log.get", "The engine's latest log lines: shader errors, models that failed to load, warnings",
      HOST_ARGS(ARGS_LOG_GET), false, host_log_get },
    { "project.info", "The open project: name, folder, startup scene, the scene being edited and whether it has unsaved changes",
      NULL, 0, false, host_project_info },
    { "project.create", "Creates a new project folder with a starter scene (the user opens it in the editor)",
      HOST_ARGS(ARGS_PROJECT_CREATE), false, host_project_create },
    { "input.get", "The game's input actions (move_x, jump...) and the keys, mouse and gamepad bindings of each",
      NULL, 0, false, host_input_get },
    { "input.set", "Adds, rebinds or removes an input action, or resets them all; saved in project.json",
      HOST_ARGS(ARGS_INPUT_SET), false, host_input_set },
    { "play.set", "Play mode: runs a throwaway copy of the scene (behaviours like Spin move); pause, step, stop",
      HOST_ARGS(ARGS_PLAY_SET), false, host_play_set },
    { "play.status", "Whether the scene is playing, paused or stopped, and for how long it has played",
      NULL, 0, false, host_play_status },
    { "project.export", "Exports the project's saved scenes as a game a player can run, then has the game check itself",
      HOST_ARGS(ARGS_EXPORT), false, host_export },
    { "project.export_status", "The last export: where, how big, and whether the game's own check passed",
      NULL, 0, false, host_export_status },
};

const cmd_host_command*
editor_host_commands(uint32* out_count)
{
    *out_count = (uint32)(sizeof(HOST_COMMANDS) / sizeof(HOST_COMMANDS[0]));
    return HOST_COMMANDS;
}
