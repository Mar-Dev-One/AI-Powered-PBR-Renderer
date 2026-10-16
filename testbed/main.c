// testbed/main.c
//
// The game runtime: loads a scene file (assets/scenes/<name>.json, as saved
// by the editor's scene.save) and plays it. No ImGui, no command console, no
// command server, no chat -- those are editor-only (editor/main.c).
//
//   Testbed [options] [scene] [hdri]
//     scene         a name under the project's scenes/ ("orrery_plaza") or a
//                   path to a .json file; defaults to the project's startup
//                   scene (project.json, Project/Project.h), else DEFAULT_SCENE
//     hdri          a Radiance .hdr under the project for image-based
//                   lighting, overriding the scene's own
//     --project D   play the project in folder D (scenes/, models/, hdri/;
//                   Core/Paths.h) instead of the source tree's assets/
//     --width N     window size in pixels (default 1920 x 1080; in fullscreen,
//     --height N    the monitor's resolution unless given)
//     --fullscreen  fullscreen on the primary monitor
//     --live        follow the editor: play the scene it is editing
//                   (World/LiveScene.h) and pick up every edit as it happens,
//                   instead of loading `scene` once. F5 in the editor starts
//                   the game this way.
//     --check PNG   load the scene in a hidden window, render a few frames,
//                   save the picture to PNG and exit: 0 if the scene loaded
//                   and rendered, 1 if not. The editor checks every export
//                   this way.
//
// The project: --project, else a project.json next to the executable (an
// exported game, Project/Project.h project_export()), else the source tree's
// assets/. Its project.json names the startup scene, and the window's title
// and size.
//
// Built twice: Testbed (a console program, for development: its log shows
// in the terminal) and Game (PBR_GAME_RUNTIME: no console window, the log
// goes to log.txt next to it) -- the executable an export ships.
//
// The game runs the scene's behaviours (World/World.h world_tick(): Spin
// components turn their entities); the editor only previews them, keeping
// the authored pose in the scene.
//
// The scene file's "environment" (World/Environment.h) sets the HDRI,
// exposure, ambient strength and skybox, as they were in the editor when it
// was saved. Older files without one get the defaults with DEFAULT_HDRI.
//
// The view comes from the scene's first Camera entity. A scene without one
// gets an orbit camera framing everything in it: left-drag to orbit, scroll
// to zoom. Esc quits.

#include "App.h"
#include "Assets/AssetCache.h"
#include "Core/Defines.h"
#include "Core/FileIO.h"
#include "Core/Image.h"
#include "Core/Paths.h"
#include "Platform/Window.h"
#include "Project/Project.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Camera.h"
#include "World/Environment.h"
#include "World/LiveScene.h"
#include "World/Serialize.h"
#include "World/World.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLFW/glfw3.h>

#define DEFAULT_SCENE "orrery_plaza"   // for a project folder without a project.json
#define DEFAULT_HDRI  "hdri/citrus_orchard_road_puresky_4k.hdr"

// How often --live checks the editor's scene file.
#define LIVE_POLL_SECONDS 0.1

// Same shadow setup as the editor, so a scene looks the same in both.
#define SHADOW_MAP_SIZE 2048
#define CASCADE_COUNT   4

// --check renders this many frames (the shadows and IBL settle) before the
// picture is taken.
#define CHECK_FRAMES 3

typedef struct game_state
{
    const char* scene_arg;
    const char* hdri_arg;
    b8          scene_loaded;

    // --check: where the picture goes, frames drawn so far, and the verdict
    // main() returns.
    const char* check_path;
    uint32      check_frames;
    b8          check_passed;

    asset_cache     assets;
    world           world;
    scene_renderer  renderer;
    scene_environment env;   // from the scene file; see World/Environment.h
    ibl_environment   ibl;     // env.hdri baked; all zeroes if it couldn't be loaded
    char              baked_hdri[ENVIRONMENT_HDRI_MAX];   // what `ibl` was last baked from
    b8                hdri_baked;                         // false until the first bake

    // --live: the editor's scene file, reloaded whenever it changes.
    b8           live;
    live_watcher watcher;
    b8           framed;   // the orbit camera has been aimed at the scene once

    // The scene's Camera entity, or ENTITY_NULL to use the orbit camera.
    entity_id scene_camera;
    camera    cam;
    f32       aspect;

    // Orbit camera, for scenes without a Camera entity.
    f32 orbit_yaw;
    f32 orbit_pitch;
    f32 orbit_distance;
    f64 last_cursor_x;
    f64 last_cursor_y;
    b8  dragging;
} game_state;

static f32
clampf(f32 value, f32 lo, f32 hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

// --- Scene -----------------------------------------------------------------------

// `arg` is a scene name under the project's scenes/ or a path to a .json file.
// Caller frees.
static char*
resolve_scene_path(const char* arg)
{
    size_t length = strlen(arg);
    if (length > 5 && strcmp(arg + length - 5, ".json") == 0)
    {
        char* path = malloc(length + 1);
        if (!path)
            FATAL("testbed: out of memory");
        memcpy(path, arg, length + 1);
        return path;
    }

    char relative[256];
    snprintf(relative, sizeof(relative), "scenes/%s.json", arg);
    return project_path(relative);
}

// Replaces the world (and environment) with `scene`'s. `source` names it in
// the log. The scene is loaded into a fresh world first and swapped in only
// if it loads, so a bad update leaves the game showing the last good scene.
// With `keep_spin`, entities that spin keep the pose the game has turned
// them to, so an edit elsewhere doesn't snap them back -- the editor never
// ticks, so its file always holds the authored pose.
//
// TODO: stopgap for the live link (World/LiveScene.h). A full reload wipes
// runtime state, and only Spin poses are carried over, matched by name.
// Replace with incremental changes that leave untouched entities alone.
static b8
apply_scene(game_state* state, const cJSON* scene, const char* source, b8 keep_spin)
{
    world next;
    world_init(&next, &state->assets);

    char error[640];
    if (!deserialize_scene(&next, scene, NULL, NULL, error, sizeof(error)))
    {
        LOG_ERROR("testbed: can't load '%s': %s%s\n", source, error,
                  world_count(&state->world) ? " -- still showing the last scene that loaded" : "");
        world_shutdown(&next);
        return false;
    }

    if (keep_spin)
    {
        const uint32 mask = COMPONENT_BIT(COMPONENT_SPIN);
        for (entity_id e = world_next(&next, ENTITY_NULL, mask); e; e = world_next(&next, e, mask))
        {
            const c_transform* before = world_transform(&state->world, world_find(&state->world, world_name(&next, e)));
            if (before)
                glm_vec3_copy((f32*)before->rotation_deg, world_transform(&next, e)->rotation_deg);
        }
    }
    else
    {
        LOG_INFO("testbed: loaded '%s' (%u entities)\n", source, world_count(&next));
    }

    // The new world already holds its models, so releasing the old one's
    // references doesn't unload anything both use.
    world_shutdown(&state->world);
    state->world = next;

    // A bad environment doesn't stop the scene from playing: it keeps the last one.
    const cJSON* env = cJSON_GetObjectItem(scene, "environment");
    if (env && !deserialize_environment(env, &state->env, error, sizeof(error)))
        LOG_WARN("testbed: '%s': %s -- keeping the previous environment\n", source, error);

    return true;
}

static b8
load_scene(game_state* state, const char* arg)
{
    char* path = resolve_scene_path(arg);
    char* text = file_exists(path) ? file_read_to_string(path) : NULL;
    if (!text)
    {
        LOG_ERROR("testbed: no scene at '%s'\n", path);
        free(path);
        return false;
    }

    cJSON* scene = cJSON_Parse(text);
    free(text);

    b8 ok = scene != NULL;
    if (ok)
        ok = apply_scene(state, scene, path, false);
    else
        LOG_ERROR("testbed: can't load '%s': not valid JSON\n", path);

    cJSON_Delete(scene);
    free(path);
    return ok;
}

// Bakes env.hdri into `ibl` if it changed since the last bake (a live edit
// can switch it). A missing file leaves the hemisphere ambient light.
static void
sync_hdri(game_state* state)
{
    if (state->hdri_baked && strcmp(state->baked_hdri, state->env.hdri) == 0)
        return;

    ibl_destroy(&state->ibl);
    memcpy(state->baked_hdri, state->env.hdri, sizeof(state->baked_hdri));
    state->hdri_baked = true;

    if (!state->env.hdri[0])
        return;

    char* path = project_path(state->env.hdri);
    if (!ibl_create(&state->ibl, path))
        LOG_WARN("No environment loaded from '%s' -- using the hemisphere ambient light\n", path);
    free(path);
}

// --- Camera ----------------------------------------------------------------------

static void
orbit_camera_apply(game_state* state)
{
    f32 yaw_rad   = glm_rad(state->orbit_yaw);
    f32 pitch_rad = glm_rad(state->orbit_pitch);

    state->cam.position[0] = state->cam.target[0] + state->orbit_distance * cosf(pitch_rad) * sinf(yaw_rad);
    state->cam.position[1] = state->cam.target[1] + state->orbit_distance * sinf(pitch_rad);
    state->cam.position[2] = state->cam.target[2] + state->orbit_distance * cosf(pitch_rad) * cosf(yaw_rad);
}

// Grows [lo, hi] by every drawn entity's bounds; with `skip_flat`, leaves
// out flat ones (ground planes, floors: under 2% as tall as they are wide),
// which would otherwise set the framing on their own. False if none counted.
static b8
gather_bounds(const world* w, b8 skip_flat, vec3 lo, vec3 hi)
{
    b8 any = false;

    const uint32 mask = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        const c_mesh_renderer* mr = world_mesh_renderer(w, e);
        vec3 e_lo, e_hi;
        if (!mr->visible || !world_bounds(w, e, e_lo, e_hi))
            continue;

        f32 height = e_hi[1] - e_lo[1];
        f32 width  = fmaxf(e_hi[0] - e_lo[0], e_hi[2] - e_lo[2]);
        if (skip_flat && height < width * 0.02f)
            continue;

        glm_vec3_minv(lo, e_lo, lo);
        glm_vec3_maxv(hi, e_hi, hi);
        any = true;
    }
    return any;
}

// Points the orbit camera at the middle of what's in the scene, far enough
// back to see all of it. Needs world_update() first (world_bounds() reads it).
static void
frame_scene(game_state* state)
{
    vec3 lo = { FLT_MAX, FLT_MAX, FLT_MAX };
    vec3 hi = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    // Frame the objects, not the floor -- unless the floor is all there is.
    b8 any = gather_bounds(&state->world, true, lo, hi) || gather_bounds(&state->world, false, lo, hi);

    vec3 center = { 0.0f, 0.0f, 0.0f };
    f32  radius = 2.0f;
    if (any)
    {
        glm_vec3_center(lo, hi, center);
        radius = glm_vec3_distance(lo, hi) * 0.5f;
    }

    // Far enough that a sphere of `radius` fits the 45 degree vertical FOV.
    f32 distance = radius / sinf(glm_rad(45.0f * 0.5f));

    camera_init(&state->cam, center, center, 45.0f, state->aspect, 0.1f, fmaxf(100.0f, distance * 4.0f));
    state->orbit_yaw      = 35.0f;
    state->orbit_pitch    = 25.0f;
    state->orbit_distance = clampf(distance, 1.0f, 500.0f);
    orbit_camera_apply(state);
}

// Left-drag orbits. Only for the orbit camera; a scene camera stays put.
static void
orbit_camera_update(game_state* state, window* wind)
{
    f64 cursor_x, cursor_y;
    window_get_cursor_pos(wind, &cursor_x, &cursor_y);

    if (window_is_mouse_button_down(wind, GLFW_MOUSE_BUTTON_LEFT))
    {
        if (state->dragging)
        {
            const f32 sensitivity = 0.25f;

            state->orbit_yaw   += (f32)(cursor_x - state->last_cursor_x) * sensitivity;
            state->orbit_pitch += (f32)(state->last_cursor_y - cursor_y) * sensitivity;
            state->orbit_pitch  = clampf(state->orbit_pitch, -89.0f, 89.0f);
            orbit_camera_apply(state);
        }
        state->dragging = true;
    }
    else
    {
        state->dragging = false;
    }

    state->last_cursor_x = cursor_x;
    state->last_cursor_y = cursor_y;
}

// Picks the view after the world changed: the scene's first Camera entity,
// else the orbit camera -- aimed at the scene the first time only, so a live
// edit doesn't yank the view away from where the player left it.
static void
choose_camera(game_state* state)
{
    entity_id previous  = state->scene_camera;
    state->scene_camera = world_next(&state->world, ENTITY_NULL, COMPONENT_BIT(COMPONENT_CAMERA));

    if (state->scene_camera && state->scene_camera != previous)
        LOG_INFO("testbed: viewing through camera '%s'\n", world_name(&state->world, state->scene_camera));

    if (!state->scene_camera && (!state->framed || previous))
    {
        frame_scene(state);
        state->framed = world_count(&state->world) > 0;
    }
}

// --- App hooks -------------------------------------------------------------------

static void
on_init(App* app)
{
    game_state* state = (game_state*)app->user_data;
    window_size size  = window_get_size(get_renderer()->drawing_window);
    state->aspect     = size.height ? (f32)size.width / (f32)size.height : 1.0f;

    LOG_INFO("testbed: project %s, engine data %s\n", paths_project_root(), paths_engine_root());

    if (!scene_renderer_create(&state->renderer, size.width, size.height, CASCADE_COUNT, SHADOW_MAP_SIZE))
        FATAL("testbed: failed to create the scene renderer");

    state->env = scene_environment_default();
    snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", DEFAULT_HDRI);

    assets_init(&state->assets);
    world_init(&state->world, &state->assets);

    if (state->live)
    {
        // The editor writes the file as soon as it runs; until then, an empty world.
        live_watcher_init(&state->watcher, NULL);
        cJSON* scene = live_watcher_poll(&state->watcher, 0.0, 0.0);
        if (scene)
            state->scene_loaded = apply_scene(state, scene, state->watcher.path, false);
        else
            LOG_INFO("testbed: waiting for the editor (%s)\n", state->watcher.path);
        cJSON_Delete(scene);
    }
    else
    {
        state->scene_loaded = load_scene(state, state->scene_arg);
    }
    world_update(&state->world);

    if (state->hdri_arg)
        snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", state->hdri_arg);
    sync_hdri(state);

    choose_camera(state);
}

static void
on_resize(App* app, uint16 width, uint16 height)
{
    game_state* state = (game_state*)app->user_data;

    // 0x0 while minimized -- would make the aspect NaN/inf.
    if (width == 0 || height == 0)
        return;

    state->aspect = (f32)width / (f32)height;
    camera_set_aspect(&state->cam, state->aspect);
    scene_renderer_resize(&state->renderer, width, height);
}

static void
on_key(App* app, int key, int action, int mods)
{
    (void)mods;

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        app->should_close = true;
}

static void
on_scroll(App* app, f64 dx, f64 dy)
{
    (void)dx;

    game_state* state = (game_state*)app->user_data;
    if (state->scene_camera)
        return;

    state->orbit_distance = clampf(state->orbit_distance - (f32)dy * 0.1f * state->orbit_distance, 1.0f, 500.0f);
    orbit_camera_apply(state);
}

// --check: the frame as a PNG at check_path. False (logged) if it can't be
// rendered or written.
static b8
save_check_picture(game_state* state, const scene_render_settings* settings)
{
    uint16 width, height;
    uint8* rgba = scene_renderer_capture(&state->renderer, &state->world, &state->cam, settings, &width, &height);
    if (!rgba)
    {
        LOG_ERROR("check: nothing was rendered\n");
        return false;
    }

    size_t png_size = 0;
    uint8* png      = image_encode_png(rgba, width, height, 4, &png_size);
    free(rgba);

    FILE* file = png ? fopen(state->check_path, "wb") : NULL;
    b8    ok   = file && fwrite(png, 1, png_size, file) == png_size;
    if (file)
        ok = fclose(file) == 0 && ok;
    free(png);

    if (ok)
        LOG_INFO("check: %s\n", state->check_path);
    else
        LOG_ERROR("check: couldn't write %s\n", state->check_path);
    return ok;
}

static void
on_frame(App* app, f32 dt)
{
    game_state* state = (game_state*)app->user_data;

    // --live: the editor changed something -- take its world, keep playing.
    if (state->live)
    {
        cJSON* scene = live_watcher_poll(&state->watcher, window_get_time(), LIVE_POLL_SECONDS);
        if (scene)
        {
            apply_scene(state, scene, state->watcher.path, true);
            if (state->hdri_arg)   // the command line still wins
                snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", state->hdri_arg);
            sync_hdri(state);
            world_update(&state->world);
            choose_camera(state);
            cJSON_Delete(scene);
        }
    }

    // A long stall (window dragged, breakpoint) shouldn't fling things around.
    world_tick(&state->world, fminf(dt, 0.1f));
    world_update(&state->world);

    // The scene camera follows its entity (it may be animated or parented);
    // fall back to the orbit camera if the entity is gone.
    if (state->scene_camera && !world_camera_view(&state->world, state->scene_camera, state->aspect, &state->cam))
    {
        state->scene_camera = ENTITY_NULL;
        frame_scene(state);
    }
    if (!state->scene_camera)
        orbit_camera_update(state, get_renderer()->drawing_window);

    scene_render_settings settings = scene_render_settings_default();
    scene_render_settings_apply_environment(&settings, &state->env, &state->ibl);

    scene_renderer_draw(&state->renderer, &state->world, &state->cam, &settings);

    if (state->check_path && ++state->check_frames >= CHECK_FRAMES)
    {
        state->check_passed = state->scene_loaded && save_check_picture(state, &settings);
        app->should_close   = true;
    }
}

static void
on_shutdown(App* app)
{
    game_state* state = (game_state*)app->user_data;

    if (state->live)
        live_watcher_shutdown(&state->watcher);
    world_shutdown(&state->world);    // releases the world's assets...
    assets_shutdown(&state->assets);  // ...then the models themselves
    ibl_destroy(&state->ibl);
    scene_renderer_destroy(&state->renderer);
}

// A whole number in 1..max, or 0 if `text` isn't one.
static uint16
parse_size(const char* text, uint16 max)
{
    char* end;
    long  value = text ? strtol(text, &end, 10) : 0;
    return (text && *end == '\0' && value > 0 && value <= max) ? (uint16)value : 0;
}

static void
print_usage(void)
{
    printf("usage: Testbed [--width N] [--height N] [--fullscreen] [--live] [--project DIR] [--check PNG] [scene] [hdri]\n"
           "  --live     follow the scene the editor is editing, live\n"
           "  --project  the project folder (default: one next to the program, else the source tree's assets/)\n"
           "  --check    render the scene once, hidden, into PNG and exit (0 = it loaded and rendered)\n"
           "  scene      name under the project's scenes/ or a path to a .json file\n"
           "             (default: the project's startup scene, else %s)\n"
           "  hdri       .hdr under the project, overriding the scene's own\n", DEFAULT_SCENE);
}

#ifdef PBR_GAME_RUNTIME
// The game has no console: its log goes to log.txt beside it (replaced each
// run), which is where to look when a player reports a problem.
static void
log_to_file(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/log.txt", paths_executable_dir());
    if (freopen(path, "w", stdout))
    {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (freopen(path, "a", stderr))
            setvbuf(stderr, NULL, _IONBF, 0);
    }
}
#endif

int main(int argc, char** argv)
{
#ifdef PBR_GAME_RUNTIME
    log_to_file();
#endif

    game_state state = { 0 };

    uint16 width       = 0;
    uint16 height      = 0;
    b8     fullscreen  = false;
    b8     has_project = false;
    int    positional  = 0;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        if (strcmp(arg, "--fullscreen") == 0)
        {
            fullscreen = true;
        }
        else if (strcmp(arg, "--live") == 0)
        {
            state.live = true;
        }
        else if (strcmp(arg, "--project") == 0)
        {
            const char* dir = i + 1 < argc ? argv[++i] : NULL;
            if (!dir || !paths_set_project_root(dir))
            {
                fprintf(stderr, "--project needs an existing folder\n");
                print_usage();
                return 1;
            }
            has_project = true;
        }
        else if (strcmp(arg, "--check") == 0)
        {
            state.check_path = i + 1 < argc ? argv[++i] : NULL;
            if (!state.check_path)
            {
                fprintf(stderr, "--check needs the PNG file to write\n");
                print_usage();
                return 1;
            }
        }
        else if (strcmp(arg, "--width") == 0 || strcmp(arg, "--height") == 0)
        {
            uint16 value = parse_size(i + 1 < argc ? argv[++i] : NULL, 16384);
            if (!value)
            {
                fprintf(stderr, "%s needs a size in pixels\n", arg);
                print_usage();
                return 1;
            }
            *(arg[2] == 'w' ? &width : &height) = value;
        }
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            print_usage();
            return 0;
        }
        else if (arg[0] == '-' && arg[1] == '-')
        {
            fprintf(stderr, "unknown option %s\n", arg);
            print_usage();
            return 1;
        }
        else if (positional == 0)
        {
            state.scene_arg = arg;
            ++positional;
        }
        else if (positional == 1)
        {
            state.hdri_arg = arg;
            ++positional;
        }
        else
        {
            fprintf(stderr, "unexpected argument %s\n", arg);
            print_usage();
            return 1;
        }
    }

    // An exported game: its project is the folder it sits in.
    if (!has_project && project_exists(paths_executable_dir()))
        paths_set_project_root(paths_executable_dir());

    // The project names the startup scene (when no scene is given), the
    // window's title and its size.
    static project_settings project;
    b8 has_settings = project_exists(paths_project_root()) && project_read(paths_project_root(), &project, NULL, 0);
    if (!state.scene_arg)
        state.scene_arg = has_settings ? project.startup_scene : DEFAULT_SCENE;

    // Windowed: the size given, else the project's, else 1920 x 1080 (a
    // check renders smaller: it's only a picture). Fullscreen: the monitor's
    // own mode (Platform/Window.h) unless a size was given.
    if (!fullscreen)
    {
        uint16 default_width  = state.check_path ? 960 : has_settings ? project.window_width : 1920;
        uint16 default_height = state.check_path ? 540 : has_settings ? project.window_height : 1080;
        width  = width ? width : default_width;
        height = height ? height : default_height;
    }

    App app = {
        .name        = state.live ? "PBR Renderer (live from the editor)" : has_settings ? project.name : "PBR Renderer",
        .hidden      = state.check_path != NULL,
        .width       = width,
        .height      = height,
        .fullscreen  = fullscreen,
        .on_init     = on_init,
        .on_frame    = on_frame,
        .on_shutdown = on_shutdown,
        .on_key      = on_key,
        .on_scroll   = on_scroll,
        .on_resize   = on_resize,
        .user_data   = &state,
        .use_imgui   = false
    };

    run(&app);
    terminate(&app);

    if (state.check_path)
        return state.check_passed ? 0 : 1;
    return 0;
}
