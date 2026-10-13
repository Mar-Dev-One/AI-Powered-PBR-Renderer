// testbed/main.c
//
// The game runtime: loads a scene file (assets/scenes/<name>.json, as saved
// by the editor's scene.save) and plays it. No ImGui, no command console, no
// command server, no chat -- those are editor-only (editor/main.c).
//
//   Testbed [options] [scene] [hdri]
//     scene         a name under assets/scenes/ ("orrery_plaza") or a path to
//                   a .json file; defaults to DEFAULT_SCENE
//     hdri          a Radiance .hdr under assets/ for image-based lighting,
//                   overriding the scene's own
//     --width N     window size in pixels (default 1280 x 720; in fullscreen,
//     --height N    the monitor's resolution unless given)
//     --fullscreen  fullscreen on the primary monitor
//
// The game runs the scene's behaviours (World/World.h world_tick(): Spin
// components turn their entities); the editor shows them standing still.
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
#include "Core/Paths.h"
#include "Platform/Window.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Camera.h"
#include "World/Environment.h"
#include "World/Serialize.h"
#include "World/World.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLFW/glfw3.h>

#define DEFAULT_SCENE "orrery_plaza"
#define DEFAULT_HDRI  "hdri/citrus_orchard_road_puresky_4k.hdr"

// Same shadow setup as the editor, so a scene looks the same in both.
#define SHADOW_MAP_SIZE 2048
#define CASCADE_COUNT   4

typedef struct game_state
{
    const char* scene_arg;
    const char* hdri_arg;

    asset_cache     assets;
    world           world;
    scene_renderer  renderer;
    scene_environment env;   // from the scene file; see World/Environment.h
    ibl_environment   ibl;     // env.hdri baked; all zeroes if it couldn't be loaded

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

// `arg` is a scene name under assets/scenes/ or a path to a .json file.
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
    return asset_path(relative);
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

    char error[640];
    b8   ok = scene && deserialize_scene(&state->world, scene, NULL, NULL, error, sizeof(error));

    if (!ok)
    {
        LOG_ERROR("testbed: can't load '%s': %s\n", path, scene ? error : "not valid JSON");
        world_clear(&state->world);   // no half-loaded scene
    }
    else
    {
        LOG_INFO("testbed: loaded '%s' (%u entities)\n", path, world_count(&state->world));
    }

    // A bad environment doesn't stop the scene from playing: it keeps the defaults.
    const cJSON* env = ok ? cJSON_GetObjectItem(scene, "environment") : NULL;
    if (env && !deserialize_environment(env, &state->env, error, sizeof(error)))
        LOG_WARN("testbed: '%s': %s -- using the default environment\n", path, error);

    cJSON_Delete(scene);
    free(path);
    return ok;
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

// --- App hooks -------------------------------------------------------------------

static void
on_init(App* app)
{
    game_state* state = (game_state*)app->user_data;
    window_size size  = window_get_size(get_renderer()->drawing_window);
    state->aspect     = size.height ? (f32)size.width / (f32)size.height : 1.0f;

    if (!scene_renderer_create(&state->renderer, size.width, size.height, CASCADE_COUNT, SHADOW_MAP_SIZE))
        FATAL("testbed: failed to create the scene renderer");

    state->env = scene_environment_default();
    snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", DEFAULT_HDRI);

    assets_init(&state->assets);
    world_init(&state->world, &state->assets);
    load_scene(state, state->scene_arg);
    world_update(&state->world);

    if (state->hdri_arg)
        snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", state->hdri_arg);

    if (state->env.hdri[0])
    {
        char* hdri_path = asset_path(state->env.hdri);
        if (!ibl_create(&state->ibl, hdri_path))
            LOG_WARN("No environment loaded from '%s' -- using the hemisphere ambient light\n", hdri_path);
        free(hdri_path);
    }

    const uint32 camera_mask = COMPONENT_BIT(COMPONENT_CAMERA);
    state->scene_camera = world_next(&state->world, ENTITY_NULL, camera_mask);
    if (state->scene_camera)
        LOG_INFO("testbed: viewing through camera '%s'\n", world_name(&state->world, state->scene_camera));
    else
        frame_scene(state);
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

static void
on_frame(App* app, f32 dt)
{
    game_state* state = (game_state*)app->user_data;

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
}

static void
on_shutdown(App* app)
{
    game_state* state = (game_state*)app->user_data;

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
    printf("usage: Testbed [--width N] [--height N] [--fullscreen] [scene] [hdri]\n"
           "  scene  name under assets/scenes/ or a path to a .json file (default %s)\n"
           "  hdri   .hdr under assets/, overriding the scene's own\n", DEFAULT_SCENE);
}

int main(int argc, char** argv)
{
    game_state state = { .scene_arg = DEFAULT_SCENE };

    uint16 width      = 0;
    uint16 height     = 0;
    b8     fullscreen = false;
    int    positional = 0;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        if (strcmp(arg, "--fullscreen") == 0)
        {
            fullscreen = true;
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

    // Windowed defaults to 1280 x 720; fullscreen to the monitor's own mode
    // (Platform/Window.h), unless a size was given.
    if (!fullscreen)
    {
        width  = width ? width : 1280;
        height = height ? height : 720;
    }

    App app = {
        .name        = "PBR Renderer",
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

    return 0;
}
