// game/main.c
//
// The game runtime: loads a scene file (scenes/<name>.json in the project,
// as saved by the editor) and plays it. No ImGui, no command console, no
// command server, no chat -- those are editor-only (editor/main.c). An
// export ships this program, renamed after the project.
//
//   Game [options] [scene] [hdri]
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
//     --check PNG   load the scene in a hidden window, render a few frames,
//                   save the picture to PNG and exit: 0 if the scene loaded
//                   and rendered, 1 if not. The editor checks every export
//                   this way, and tests/golden_tests.c compares the pictures
//                   with saved ones. Behaviours (Spin) don't run, so the
//                   picture is always the scene as saved.
//     --view NAME   show a debug view instead of the lit scene: albedo,
//                   normals, roughness, metallic, ao, shadow, lighting,
//                   wireframe, lights (Renderer/SceneRenderer.h)
//     --antialiasing NAME
//                   fxaa (the default), taa or off
//     --unclustered every light shaded at every pixel instead of by cluster
//                   (Renderer/Lighting.h): the reference clustering must match
//
// The project: --project, else a project.json next to the executable (an
// exported game, Project/Project.h project_export()), else the source tree's
// assets/. Its project.json names the startup scene, and the window's title
// and size.
//
// A Debug build is a console program, so its log shows in the terminal;
// other builds (PBR_GAME_NO_CONSOLE, what players get) open no console and
// write the log to log.txt next to the program. Those also show a message
// box before closing on an error (a FATAL, or a startup scene that doesn't
// load), so the player learns why -- except under --check, which nobody
// watches.
//
// The game runs the scene's behaviours (World/World.h world_tick(): Spin
// components turn their entities), as the editor's Play mode does, and lets
// the player move PlayerController entities (World/PlayerController.h) with
// the project's input actions: keys and a gamepad at once, the mouse while
// the cursor is locked -- from the start when there's a controller, again
// by clicking. Esc frees the cursor, and quits when it's already free.
// Walking into a SceneTrigger fades over to the scene it names
// (World/SceneTransition.h); one that doesn't load leaves the player where
// they were.
//
// The scene file's "environment" (World/Environment.h) sets the HDRI,
// exposure, ambient strength and skybox, as they were in the editor when it
// was saved. A scene that names no HDRI gets the project's default HDRI
// (which an export copies), or DEFAULT_HDRI without a project.json.
//
// The view comes from the scene's first Camera entity. A scene without one
// gets an orbit camera framing everything in it: left-drag (or the right
// stick) to orbit, scroll (or the triggers) to zoom.

#include "App.h"
#include "Assets/AssetCache.h"
#include "Core/Defines.h"
#include "Core/FileIO.h"
#include "Core/Image.h"
#include "Core/Paths.h"
#include "Platform/Dialog.h"
#include "Platform/Input.h"
#include "Platform/InputActions.h"
#include "Platform/Window.h"
#include "Project/Project.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Camera.h"
#include "Scene/OrbitCamera.h"
#include "World/Environment.h"
#include "World/PlayerController.h"
#include "World/SceneTransition.h"
#include "World/Serialize.h"
#include "World/World.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SCENE "orrery_plaza"   // for a project folder without a project.json
#define DEFAULT_HDRI  "hdri/citrus_orchard_road_puresky_4k.hdr"

// Same shadow setup as the editor, so a scene looks the same in both.
#define SHADOW_MAP_SIZE 2048
#define CASCADE_COUNT   4

// --check renders this many frames before the picture is taken: the
// shadows and IBL settle, and TAA goes once through its 8-step jitter.
#define CHECK_FRAMES 8

typedef struct game_state
{
    const char* scene_arg;
    const char* hdri_arg;
    const char* fallback_hdri;   // for a scene that names none ("" = the hemisphere ambient)
    const input_action_map* input;   // the project's actions; NULL = the defaults App starts with
    b8          scene_loaded;
    char        scene_error[768];   // why it didn't, for the player

    // --check: where the picture goes, frames drawn so far, and the verdict
    // main() returns.
    const char* check_path;
    uint32      check_frames;
    b8          check_passed;

    scene_debug_view   debug_view;     // --view; SCENE_VIEW_LIT normally
    scene_antialiasing antialiasing;   // --antialiasing; FXAA normally
    b8                 unclustered;    // --unclustered

    asset_cache     assets;
    world           world;
    scene_renderer  renderer;
    scene_environment env;   // from the scene file; see World/Environment.h
    ibl_cache         ibl;     // env.hdri baked

    // The scene's Camera entity, or ENTITY_NULL to use the orbit camera.
    entity_id scene_camera;
    camera    cam;
    f32       aspect;

    b8 lock_on_start;   // lock the cursor next frame: a scene with a PlayerController has loaded
    scene_transition transition;

    // Orbit camera, for scenes without a Camera entity.
    orbit_camera orbit;
} game_state;

// --- Telling the player ----------------------------------------------------------

// The message box's title: the game's name.
static char player_title[128] = "PBR Game";
static b8   player_can_see    = false;   // set in main(): no console, and not --check

// Shows `problem` in a message box, pointing at log.txt for the details.
// False (and nothing shown) if the player has a console instead, or nobody
// is watching (--check).
static b8
tell_player(const char* problem)
{
    if (!player_can_see)
        return false;

    char message[1536];
    snprintf(message, sizeof(message), "%s has to close.\n\n%s\n\nThe details are in log.txt, next to the game.",
             player_title, problem);
    dialog_show_error(player_title, message);
    return true;
}

// log_set_fatal_hook(): the program aborts once it returns.
static void
on_fatal(const char* message)
{
    tell_player(message);
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
            FATAL("game: out of memory");
        memcpy(path, arg, length + 1);
        return path;
    }

    char relative[256];
    snprintf(relative, sizeof(relative), "scenes/%s.json", arg);
    return project_path(relative);
}

// Replaces the world (and environment) with `scene`'s. `source` names it in
// the log. The scene is loaded into a fresh world first and swapped in only
// if it loads, so a bad scene leaves the last good one playing.
static b8
apply_scene(game_state* state, const cJSON* scene, const char* source)
{
    world next;
    world_init(&next, &state->assets);

    char error[640];
    if (!deserialize_scene(&next, scene, NULL, NULL, error, sizeof(error)))
    {
        LOG_ERROR("game: can't load '%s': %s%s\n", source, error,
                  world_count(&state->world) ? " -- still showing the last scene that loaded" : "");
        snprintf(state->scene_error, sizeof(state->scene_error), "Can't load the scene '%s': %s", source, error);
        world_shutdown(&next);
        return false;
    }

    LOG_INFO("game: loaded '%s' (%u entities)\n", source, world_count(&next));

    // The new world already holds its models, so releasing the old one's
    // references doesn't unload anything both use.
    world_shutdown(&state->world);
    state->world = next;

    // A bad environment doesn't stop the scene from playing: it keeps the last one.
    const cJSON* env = cJSON_GetObjectItem(scene, "environment");
    if (env && !deserialize_environment(env, &state->env, error, sizeof(error)))
        LOG_WARN("game: '%s': %s -- keeping the previous environment\n", source, error);

    return true;
}

static b8
load_scene(game_state* state, const char* arg)
{
    char* path = resolve_scene_path(arg);
    char* text = file_exists(path) ? file_read_to_string(path) : NULL;
    if (!text)
    {
        LOG_ERROR("game: no scene at '%s'\n", path);
        snprintf(state->scene_error, sizeof(state->scene_error), "There's no scene at '%s'.", path);
        free(path);
        return false;
    }

    cJSON* scene = cJSON_Parse(text);
    free(text);

    b8 ok = scene != NULL;
    if (ok)
        ok = apply_scene(state, scene, path);
    else
    {
        LOG_ERROR("game: can't load '%s': not valid JSON\n", path);
        snprintf(state->scene_error, sizeof(state->scene_error), "Can't load the scene '%s': it isn't valid JSON.", path);
    }

    cJSON_Delete(scene);
    free(path);
    return ok;
}

// --- Camera ----------------------------------------------------------------------

// Points the orbit camera at the middle of what's in the scene
// (World/World.h world_framing_bounds()), far enough back to see all of it.
// Needs world_update() first.
static void
frame_scene(game_state* state)
{
    vec3 lo = { -2.0f, 0.0f, 0.0f }, hi = { 2.0f, 0.0f, 0.0f };   // an empty scene: 2 m around the origin
    world_framing_bounds(&state->world, lo, hi);

    camera_init(&state->cam, (vec3){ 0.0f, 0.0f, 0.0f }, (vec3){ 0.0f, 0.0f, 0.0f }, 45.0f, state->aspect, 0.1f, 100.0f);
    state->orbit = (orbit_camera){ .yaw = 35.0f, .pitch = 25.0f };
    orbit_fit(&state->orbit, &state->cam, lo, hi, 1.0f);

    state->cam.far_plane  = fmaxf(100.0f, state->orbit.distance * 4.0f);
    state->orbit.distance = glm_clamp(state->orbit.distance, 1.0f, ORBIT_MAX_DISTANCE);
    orbit_apply(&state->orbit, &state->cam);
}

// Left-drag or a pad's right stick orbits; the wheel or the triggers zoom.
// Only for the orbit camera; a scene camera stays put.
static void
orbit_camera_update(game_state* state, f32 dt)
{
    const f32 stick_pixels_per_second = 600.0f;   // a full push turns like a fast drag
    const f32 trigger_notches_per_second = 3.0f;

    f32 dx = 0.0f, dy = 0.0f;
    if (input_mouse_down(INPUT_MOUSE_LEFT))
        input_mouse_delta(&dx, &dy);
    dx += input_pad_axis_value(INPUT_PAD_ANY, INPUT_PAD_RIGHT_X) * stick_pixels_per_second * dt;
    dy += input_pad_axis_value(INPUT_PAD_ANY, INPUT_PAD_RIGHT_Y) * stick_pixels_per_second * dt;
    if (dx != 0.0f || dy != 0.0f)
        orbit_drag(&state->orbit, &state->cam, dx, dy);

    f32 wheel_x, wheel;
    input_scroll(&wheel_x, &wheel);
    wheel += (input_pad_axis_value(INPUT_PAD_ANY, INPUT_PAD_RIGHT_TRIGGER) -
              input_pad_axis_value(INPUT_PAD_ANY, INPUT_PAD_LEFT_TRIGGER)) * trigger_notches_per_second * dt;
    if (wheel != 0.0f)
        orbit_zoom(&state->orbit, &state->cam, wheel);
}

// Picks the view once the scene has loaded: its first Camera entity, else
// the orbit camera aimed at everything in it.
static void
choose_camera(game_state* state)
{
    state->scene_camera = world_next(&state->world, ENTITY_NULL, COMPONENT_BIT(COMPONENT_CAMERA));

    if (state->scene_camera)
        LOG_INFO("game: viewing through camera '%s'\n", world_name(&state->world, state->scene_camera));
    else
        frame_scene(state);
}

// --- App hooks -------------------------------------------------------------------

static void
on_init(App* app)
{
    game_state* state = (game_state*)app->user_data;
    window_size size  = window_get_size(get_renderer()->drawing_window);
    state->aspect     = size.height ? (f32)size.width / (f32)size.height : 1.0f;

    LOG_INFO("game: project %s, engine data %s\n", paths_project_root(), paths_engine_root());

    if (state->input)
        input_actions_load(state->input);

    if (!scene_renderer_create(&state->renderer, size.width, size.height, CASCADE_COUNT, SHADOW_MAP_SIZE))
        FATAL("game: failed to create the scene renderer");

    state->env = scene_environment_default();
    snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", state->fallback_hdri);

    assets_init(&state->assets);
    world_init(&state->world, &state->assets);

    state->scene_loaded = load_scene(state, state->scene_arg);
    if (!state->scene_loaded && tell_player(state->scene_error))
    {
        app->should_close = true;
        return;
    }
    world_update(&state->world);

    if (state->hdri_arg)
        snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", state->hdri_arg);
    ibl_cache_sync(&state->ibl, state->env.hdri);

    choose_camera(state);
    scene_transition_reset(&state->transition, &state->world);
    state->lock_on_start = true;   // the first frame locks it if there's a PlayerController
}

// A SceneTrigger's scene, mid-transition. False (logged) if it doesn't load:
// the scene that was playing goes on.
static b8
enter_scene(game_state* state, const char* name)
{
    if (!load_scene(state, name))
        return false;

    world_update(&state->world);
    if (state->hdri_arg)
        snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", state->hdri_arg);
    ibl_cache_sync(&state->ibl, state->env.hdri);
    choose_camera(state);
    return true;
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

    window* wind     = get_renderer()->drawing_window;
    b8      controls = !state->check_path && world_next(&state->world, ENTITY_NULL,
                                                         COMPONENT_BIT(COMPONENT_PLAYER_CONTROLLER));
    b8      locked   = input_cursor_mode() == WINDOW_CURSOR_LOCKED;

    // Esc frees the mouse first, then quits; a click takes it back.
    if (input_key_pressed(INPUT_KEY_ESCAPE))
    {
        if (locked)
            input_set_cursor_mode(wind, WINDOW_CURSOR_NORMAL);
        else
            app->should_close = true;
    }
    else if (controls && !locked && (input_mouse_pressed(INPUT_MOUSE_LEFT) || state->lock_on_start))
        input_set_cursor_mode(wind, WINDOW_CURSOR_LOCKED);
    state->lock_on_start = false;

    // A long stall (window dragged, breakpoint) shouldn't fling things around.
    // A check's picture is the scene as saved, whatever the frame times.
    if (!state->check_path)
    {
        // Held still while the view fades between scenes.
        player_input input = { 0 };
        if (!scene_transition_busy(&state->transition))
            input = player_input_from_actions();
        player_controller_tick(&state->world, fminf(dt, 0.1f), &input);
        world_tick(&state->world, fminf(dt, 0.1f));
    }
    world_update(&state->world);

    if (!state->check_path)
    {
        const char* next = scene_transition_update(&state->transition, &state->world, fminf(dt, 0.1f));
        if (next)
            scene_transition_loaded(&state->transition, &state->world, enter_scene(state, next));
    }

    // The scene camera follows its entity (it may be animated or parented);
    // fall back to the orbit camera if the entity is gone.
    if (state->scene_camera && !world_camera_view(&state->world, state->scene_camera, state->aspect, &state->cam))
    {
        state->scene_camera = ENTITY_NULL;
        frame_scene(state);
    }
    if (!state->scene_camera)
        orbit_camera_update(state, fminf(dt, 0.1f));

    scene_render_settings settings = scene_render_settings_default();
    scene_render_settings_apply_environment(&settings, &state->env, ibl_cache_maps(&state->ibl));
    settings.debug_view   = state->debug_view;
    settings.antialiasing = state->antialiasing;
    settings.debug_unclustered = state->unclustered;
    // Auto-exposure adapts by the frame time; a check's picture mustn't
    // depend on frame times, so it goes straight to the scene's level.
    settings.delta_time = state->check_path ? -1.0f : dt;
    settings.exposure  *= 1.0f - scene_transition_fade(&state->transition);

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

    world_shutdown(&state->world);    // releases the world's assets...
    assets_shutdown(&state->assets);  // ...then the models themselves
    ibl_cache_destroy(&state->ibl);
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
    printf("usage: Game [--width N] [--height N] [--fullscreen] [--project DIR] [--check PNG] [--view NAME]\n"
           "            [--antialiasing NAME] [scene] [hdri]\n"
           "  --project  the project folder (default: one next to the program, else the source tree's assets/)\n"
           "  --check    render the scene once, hidden, into PNG and exit (0 = it loaded and rendered)\n"
           "  --view     a debug view: albedo, normals, roughness, metallic, ao, shadow, lighting, wireframe\n"
           "  --antialiasing  fxaa (default), taa or off\n"
           "  scene      name under the project's scenes/ or a path to a .json file\n"
           "             (default: the project's startup scene, else %s)\n"
           "  hdri       .hdr under the project, overriding the scene's own\n", DEFAULT_SCENE);
}

#ifdef PBR_GAME_NO_CONSOLE
// No console: the log goes to log.txt beside the program (replaced each
// run), which is where to look when a player reports a problem.
static void
log_to_file(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/log.txt", paths_executable_dir());

    // Emptied once, then both streams append: each write lands at the
    // file's end, so stdout's info lines never overwrite stderr's errors
    // (two "w" handles would each keep their own position).
    FILE* empty = fopen(path, "w");
    if (!empty)
        return;
    fclose(empty);

    if (freopen(path, "a", stdout))
        setvbuf(stdout, NULL, _IONBF, 0);
    if (freopen(path, "a", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
}
#endif

int main(int argc, char** argv)
{
#ifdef PBR_GAME_NO_CONSOLE
    log_to_file();
#endif

    game_state state   = { 0 };
    state.antialiasing = SCENE_AA_FXAA;

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
        else if (strcmp(arg, "--view") == 0)
        {
            if (!scene_debug_view_from_name(i + 1 < argc ? argv[++i] : NULL, &state.debug_view))
            {
                fprintf(stderr, "--view needs one of: lit, albedo, normals, roughness, metallic, ao, shadow, lighting, wireframe, lights\n");
                print_usage();
                return 1;
            }
        }
        else if (strcmp(arg, "--unclustered") == 0)
        {
            state.unclustered = true;
        }
        else if (strcmp(arg, "--antialiasing") == 0)
        {
            if (!scene_antialiasing_from_name(i + 1 < argc ? argv[++i] : NULL, &state.antialiasing))
            {
                fprintf(stderr, "--antialiasing needs one of: fxaa, taa, off\n");
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
    state.fallback_hdri = has_settings ? project.default_hdri : DEFAULT_HDRI;
    state.input         = has_settings ? &project.input : NULL;

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

    // Once the game's name is known: errors from here on can be shown.
#ifdef PBR_GAME_NO_CONSOLE
    if (!state.check_path)
    {
        if (has_settings)
            snprintf(player_title, sizeof(player_title), "%s", project.name);
        player_can_see = true;
        log_set_fatal_hook(on_fatal);
    #ifdef _MSC_VER
        // After our message box, abort() quietly: no "abort() has been
        // called" box or Windows Error Reporting dialog on top of it.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    #endif
    }
#endif

    App app = {
        .name        = has_settings ? project.name : "PBR Game",
        .hidden      = state.check_path != NULL,
        .width       = width,
        .height      = height,
        .fullscreen  = fullscreen,
        .on_init     = on_init,
        .on_frame    = on_frame,
        .on_shutdown = on_shutdown,
        .on_resize   = on_resize,
        .user_data   = &state,
        .ui          = NULL
    };

    run(&app);
    terminate(&app);

    if (state.check_path)
        return state.check_passed ? 0 : 1;
    return 0;
}
