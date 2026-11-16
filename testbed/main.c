// testbed/main.c
//
// The testbed: builds a scene in code with the engine's World API -- no scene
// file -- and draws it with the scene renderer. What a program using the
// engine as a library looks like, and a quick way to try a renderer change.
//
//   Testbed [--scene NAME] [--check PNG] [--view NAME]
//     --scene NAME  materials (the default, below), glass (build_glass_scene()),
//                   focus (build_focus_scene()), coat (build_coat_scene()),
//                   showcase (build_showcase_scene(): the renderer at its
//                   best) or features (build_features_scene(): every engine
//                   feature, walked through)
//     --aperture F  depth of field's f-number (0 = off; the focus scene's is 2,
//                   the showcase's 2.8)
//     --focus M     meters to focus at (default: the focus scene's subject)
//     --dof-taps N  depth of field's samples per pixel (quality), 8..128
//     --timings N   draw N frames in a hidden window, print each pass's GPU
//                   time (Renderer/GpuProfiler.h) and exit; with --check, save the last
//     --size WxH    the window's size (default 1600 x 900, 960 x 540 with --check)
//     --aa NAME     anti-aliasing: off, fxaa (the default) or taa
//     --turn DEG    orbit the camera DEG degrees every frame (TAA under motion)
//     --check PNG   render a few frames in a hidden window, save the picture
//                   to PNG and exit (0 if it was written)
//     --view NAME   a debug view instead of the lit scene: albedo, normals,
//                   roughness, metallic, ao, shadow, lighting, wireframe,
//                   lights, transmission, behind_glass, focus
//                   (Renderer/SceneRenderer.h)
//
// The scene (build_scene()): a ground plane; the material grid -- two rows
// of spheres, dielectric in front and metal behind, going from smooth to
// rough left to right; a polished Suzanne turning on a pedestal; a sun that
// casts shadows, a warm point light and a spot light; an HDRI sky. Models
// and the HDRI come from the source tree's assets/ (Core/Paths.h).
//
// Left-drag orbits, the wheel zooms, Esc quits. A scene whose camera has a
// PlayerController (features) is walked instead: click to take the mouse,
// then WASD / the mouse or a gamepad (the default input actions,
// Platform/InputActions.h); Esc gives the mouse back, and quits once it's
// free. Its SceneTriggers fade over to the testbed scene they name.

#include "App.h"
#include "Assets/AssetCache.h"
#include "Core/Defines.h"
#include "Core/Image.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Camera.h"
#include "Scene/Light.h"
#include "Scene/OrbitCamera.h"
#include "World/Environment.h"
#include "World/PlayerController.h"
#include "World/SceneTransition.h"
#include "World/World.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDRI "hdri/citrus_orchard_road_puresky_4k.hdr"

#define SHADOW_MAP_SIZE 2048
#define CASCADE_COUNT   4

// --check renders this many frames before the picture is taken, so the
// shadows and IBL settle.
#define CHECK_FRAMES 8

#define GRID_COLUMNS 7

typedef enum testbed_scene
{
    TESTBED_SCENE_MATERIALS,   // build_scene()
    TESTBED_SCENE_GLASS,       // build_glass_scene()
    TESTBED_SCENE_FOCUS,       // build_focus_scene()
    TESTBED_SCENE_COAT,        // build_coat_scene()
    TESTBED_SCENE_SHOWCASE,    // build_showcase_scene()
    TESTBED_SCENE_FEATURES,    // build_features_scene()
    TESTBED_SCENE_COUNT
} testbed_scene;

// --scene's names, which SceneTriggers name too.
static const char* const TESTBED_SCENE_NAMES[TESTBED_SCENE_COUNT] = { "materials", "glass", "focus", "coat",
                                                                       "showcase", "features" };

// --scene focus: where the camera is focused, the subject's middle.
static const vec3 FOCUS_SUBJECT = { 0.0f, 0.8f, 0.0f };

// --scene showcase: the glass Suzanne on its pedestal, where the camera focuses.
static const vec3 SHOWCASE_SUBJECT = { 0.0f, 1.3f, 0.0f };

#define SHOWCASE_HDRI "hdri/table_mountain_2_puresky_4k.hdr"

typedef struct testbed_state
{
    const char*      check_path;
    uint32           check_frames;
    uint32           timing_frames;   // --timings N: frames to draw before printing the GPU timings
    uint16           width, height;   // --size; 0 = the default
    b8               check_passed;
    scene_debug_view debug_view;
    testbed_scene    scene;         // --scene
    f32              aperture;      // --aperture; < 0 = the scene's own (f/2 for focus, else off)
    f32              focus;         // --focus; 0 = on the scene's subject
    int              dof_taps;      // --dof-taps; 0 = the renderer's default
    scene_antialiasing antialiasing;   // --aa; FXAA unless given
    b8               antialiasing_set;
    f32              turn_degrees;    // --turn: the camera orbits this far each frame

    asset_cache       assets;
    world             world;
    scene_renderer    renderer;
    scene_environment env;
    ibl_cache         ibl;

    camera       cam;
    orbit_camera orbit;
    f32          aspect;

    // A scene walked rather than orbited: its PlayerController Camera.
    entity_id        player;
    scene_transition transition;      // its SceneTriggers
} testbed_state;

// --- Scene -----------------------------------------------------------------------

// An entity showing `model` at `position`, uniformly scaled.
static entity_id
add_model(world* w, const char* name, asset_id model, vec3 position, f32 scale)
{
    entity_id    e = world_create(w, name);
    c_transform* t = world_transform(w, e);
    glm_vec3_copy(position, t->position);
    glm_vec3_fill(t->scale, scale);
    world_add_mesh_renderer(w, e, model);
    return e;
}

static void
set_material(world* w, entity_id e, vec3 base_color, f32 metallic, f32 roughness)
{
    material_override* m = &world_mesh_renderer(w, e)->material;
    m->fields    = MATERIAL_OVERRIDE_BASE_COLOR | MATERIAL_OVERRIDE_METALLIC | MATERIAL_OVERRIDE_ROUGHNESS;
    glm_vec3_copy(base_color, m->base_color);
    m->metallic  = metallic;
    m->roughness = roughness;
}

// Makes `e` glass (MeshRenderer overrides): clear when attenuation_distance
// is 0, else tinted towards `tint` over that many meters inside it.
// `thickness` is in the model's own units (1 = a built-in sphere's diameter).
static void
set_glass(world* w, entity_id e, f32 roughness, f32 ior, f32 thickness, vec3 tint, f32 attenuation_distance)
{
    material_override* m = &world_mesh_renderer(w, e)->material;
    m->fields |= MATERIAL_OVERRIDE_BASE_COLOR | MATERIAL_OVERRIDE_METALLIC | MATERIAL_OVERRIDE_ROUGHNESS
               | MATERIAL_OVERRIDE_TRANSMISSION | MATERIAL_OVERRIDE_IOR | MATERIAL_OVERRIDE_THICKNESS
               | MATERIAL_OVERRIDE_ATTENUATION_COLOR | MATERIAL_OVERRIDE_ATTENUATION_DISTANCE;
    glm_vec3_one(m->base_color);
    m->metallic             = 0.0f;
    m->roughness            = roughness;
    m->transmission         = 1.0f;
    m->ior                  = ior;
    m->thickness            = thickness;
    glm_vec3_copy(tint, m->attenuation_color);
    m->attenuation_distance = attenuation_distance;
}

// A checkerboard wall of `columns` x `rows` square tiles `tile` meters
// across, standing on the ground at depth z, centred on x = 0.
static void
add_checker_wall(world* w, asset_id cube, int columns, int rows, f32 tile, f32 z)
{
    for (int row = 0; row < rows; ++row)
    {
        for (int column = 0; column < columns; ++column)
        {
            char name[32];
            snprintf(name, sizeof(name), "tile_%d_%d", row, column);

            f32 x = ((f32)column - (columns - 1) * 0.5f) * tile;
            f32 y = ((f32)row + 0.5f) * tile;
            entity_id e = add_model(w, name, cube, (vec3){ x, y, z }, 1.0f);
            glm_vec3_copy((vec3){ tile, tile, 0.05f }, world_transform(w, e)->scale);

            b8 light_tile = (row + column) % 2 == 0;
            set_material(w, e, light_tile ? (vec3){ 0.85f, 0.85f, 0.82f } : (vec3){ 0.05f, 0.05f, 0.06f }, 0.0f, 0.6f);
        }
    }
}

// --scene glass: what glass has to get right, in front of a checkerboard
// wall that shows bending, tint and blur. Left to right: a clear sphere, a
// green "bottle" (a stretched sphere) with an amber one behind it, a
// frosted sphere (roughness 0.3) and, to compare, an ordinary red one. A
// bright lamp behind and above them.
static void
build_glass_scene(world* w, asset_cache* assets)
{
    asset_id plane  = assets_builtin(assets, BUILTIN_PLANE);
    asset_id cube   = assets_builtin(assets, BUILTIN_CUBE);
    asset_id sphere = assets_builtin(assets, BUILTIN_SPHERE);

    entity_id ground = add_model(w, "ground", plane, (vec3){ 0.0f, 0.0f, 0.0f }, 1.0f);
    glm_vec3_copy((vec3){ 12.0f, 1.0f, 8.0f }, world_transform(w, ground)->scale);
    set_material(w, ground, (vec3){ 0.5f, 0.5f, 0.52f }, 0.0f, 0.7f);

    // The checkerboard: 0.4 m tiles on a wall 1.5 m behind the glass.
    add_checker_wall(w, cube, 16, 8, 0.4f, -1.5f);

    entity_id clear = add_model(w, "clear_glass", sphere, (vec3){ -1.8f, 0.6f, 0.0f }, 1.2f);
    set_glass(w, clear, 0.0f, 1.5f, 1.0f, (vec3){ 1.0f, 1.0f, 1.0f }, 0.0f);

    entity_id bottle = add_model(w, "green_bottle", sphere, (vec3){ -0.4f, 0.9f, 0.0f }, 1.0f);
    glm_vec3_copy((vec3){ 0.6f, 1.8f, 0.6f }, world_transform(w, bottle)->scale);
    set_glass(w, bottle, 0.02f, 1.5f, 1.0f, (vec3){ 0.2f, 0.75f, 0.3f }, 0.4f);

    // Glass behind glass: an amber bottle right behind the green one. The
    // green one shows only the opaque scene behind both (SceneRenderer.h).
    entity_id back_bottle = add_model(w, "amber_bottle", sphere, (vec3){ -0.25f, 0.8f, -0.8f }, 1.0f);
    glm_vec3_copy((vec3){ 0.6f, 1.6f, 0.6f }, world_transform(w, back_bottle)->scale);
    set_glass(w, back_bottle, 0.02f, 1.5f, 1.0f, (vec3){ 0.9f, 0.55f, 0.15f }, 0.4f);

    entity_id frosted = add_model(w, "frosted_glass", sphere, (vec3){ 1.0f, 0.6f, 0.0f }, 1.2f);
    set_glass(w, frosted, 0.3f, 1.5f, 1.0f, (vec3){ 1.0f, 1.0f, 1.0f }, 0.0f);

    entity_id solid = add_model(w, "solid", sphere, (vec3){ 2.4f, 0.5f, 0.0f }, 1.0f);
    set_material(w, solid, (vec3){ 0.8f, 0.12f, 0.1f }, 0.0f, 0.3f);

    light sun;
    light_init_directional(&sun, (vec3){ 0.3f, -1.0f, -0.6f }, (vec3){ 1.0f, 0.97f, 0.92f }, 2.0f);
    world_add_light(w, world_create(w, "sun"), sun);

    light lamp;
    light_init_point(&lamp, (vec3){ 1.0f, 0.9f, 0.75f }, 30.0f, 8.0f);
    entity_id lamp_entity = world_create(w, "lamp");
    glm_vec3_copy((vec3){ 0.5f, 2.6f, -0.9f }, world_transform(w, lamp_entity)->position);
    world_add_light(w, lamp_entity, lamp);
}

// --scene focus: depth of field. A blue ceramic "bottle" (a stretched
// sphere) is the subject, with a checkerboard wall 5 m behind it, a white
// ball in front of it, low and to the left, overlapping it, and a row of small bright
// lamps just before the wall -- they should blur into round discs.
static void
build_focus_scene(world* w, asset_cache* assets)
{
    asset_id plane  = assets_builtin(assets, BUILTIN_PLANE);
    asset_id cube   = assets_builtin(assets, BUILTIN_CUBE);
    asset_id sphere = assets_builtin(assets, BUILTIN_SPHERE);

    entity_id ground = add_model(w, "ground", plane, (vec3){ 0.0f, 0.0f, -2.0f }, 1.0f);
    glm_vec3_copy((vec3){ 14.0f, 1.0f, 12.0f }, world_transform(w, ground)->scale);
    set_material(w, ground, (vec3){ 0.4f, 0.38f, 0.36f }, 0.0f, 0.8f);

    add_checker_wall(w, cube, 20, 10, 0.5f, -5.0f);

    entity_id bottle = add_model(w, "bottle", sphere, (vec3){ FOCUS_SUBJECT[0], FOCUS_SUBJECT[1], FOCUS_SUBJECT[2] },
                                 1.0f);
    glm_vec3_copy((vec3){ 0.5f, 1.6f, 0.5f }, world_transform(w, bottle)->scale);
    set_material(w, bottle, (vec3){ 0.1f, 0.25f, 0.7f }, 0.0f, 0.25f);

    entity_id ball = add_model(w, "front_ball", sphere, (vec3){ -0.1f, 0.35f, 1.6f }, 0.7f);
    set_material(w, ball, (vec3){ 0.9f, 0.9f, 0.88f }, 0.0f, 0.5f);

    for (int i = 0; i < 6; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "bokeh_%d", i);
        entity_id e = add_model(w, name, sphere, (vec3){ -2.0f + 0.8f * (f32)i, 1.6f + 0.35f * (f32)(i % 2), -4.6f },
                                0.06f);
        material_override* m = &world_mesh_renderer(w, e)->material;
        m->fields = MATERIAL_OVERRIDE_EMISSIVE;
        glm_vec3_copy((vec3){ 60.0f, 45.0f, 25.0f }, m->emissive);
    }

    light sun;
    light_init_directional(&sun, (vec3){ -0.3f, -1.0f, -0.4f }, (vec3){ 1.0f, 0.96f, 0.9f }, 2.5f);
    world_add_light(w, world_create(w, "sun"), sun);
}

// --scene coat: clear coat and specular. In front, three rough red balls:
// no coat, a smooth coat (sharp highlights over the soft base) and a rough
// coat. Behind, dark grey balls from specular 0 to 1 -- their reflections
// from none to glTF's default -- and one whose reflection is tinted gold.
static void
build_coat_scene(world* w, asset_cache* assets)
{
    asset_id plane  = assets_builtin(assets, BUILTIN_PLANE);
    asset_id sphere = assets_builtin(assets, BUILTIN_SPHERE);

    entity_id ground = add_model(w, "ground", plane, (vec3){ 0.0f, 0.0f, 0.0f }, 1.0f);
    glm_vec3_copy((vec3){ 12.0f, 1.0f, 8.0f }, world_transform(w, ground)->scale);
    set_material(w, ground, (vec3){ 0.3f, 0.3f, 0.32f }, 0.0f, 0.9f);

    const f32 coats[3][2] = { { 0.0f, 0.0f }, { 1.0f, 0.03f }, { 1.0f, 0.3f } };   // clearcoat, its roughness
    for (int i = 0; i < 3; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "red_coat_%d", i);
        entity_id e = add_model(w, name, sphere, (vec3){ -1.3f + 1.3f * (f32)i, 0.5f, 1.0f }, 1.0f);
        set_material(w, e, (vec3){ 0.6f, 0.04f, 0.03f }, 0.0f, 0.7f);

        material_override* m = &world_mesh_renderer(w, e)->material;
        m->fields |= MATERIAL_OVERRIDE_CLEARCOAT | MATERIAL_OVERRIDE_CLEARCOAT_ROUGHNESS;
        m->clearcoat           = coats[i][0];
        m->clearcoat_roughness = coats[i][1];
    }

    for (int i = 0; i < 6; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "specular_%d", i);
        entity_id e = add_model(w, name, sphere, (vec3){ -2.75f + 1.1f * (f32)i, 0.45f, -0.6f }, 0.9f);
        set_material(w, e, (vec3){ 0.05f, 0.05f, 0.05f }, 0.0f, 0.15f);

        material_override* m = &world_mesh_renderer(w, e)->material;
        m->fields |= MATERIAL_OVERRIDE_SPECULAR | MATERIAL_OVERRIDE_SPECULAR_COLOR;
        m->specular = i < 5 ? 0.25f * (f32)i : 1.0f;
        glm_vec3_copy(i < 5 ? (vec3){ 1.0f, 1.0f, 1.0f } : (vec3){ 1.0f, 0.7f, 0.2f }, m->specular_color);
    }

    light sun;
    light_init_directional(&sun, (vec3){ -0.5f, -0.8f, -0.6f }, (vec3){ 1.0f, 0.97f, 0.92f }, 2.5f);
    world_add_light(w, world_create(w, "sun"), sun);

    light lamp;
    light_init_point(&lamp, (vec3){ 1.0f, 0.95f, 0.9f }, 12.0f, 10.0f);
    entity_id lamp_entity = world_create(w, "lamp");
    glm_vec3_copy((vec3){ 1.5f, 2.2f, 2.5f }, world_transform(w, lamp_entity)->position);
    world_add_light(w, lamp_entity, lamp);
}

// A box of `size` with its bottom on the ground, centred over (x, z).
static entity_id
add_block(world* w, const char* name, asset_id cube, f32 x, f32 z, vec3 size, vec3 color, f32 metallic,
          f32 roughness)
{
    entity_id e = add_model(w, name, cube, (vec3){ x, size[1] * 0.5f, z }, 1.0f);
    glm_vec3_copy(size, world_transform(w, e)->scale);
    set_material(w, e, color, metallic, roughness);
    return e;
}

// `model` scaled to `height` meters, turned `yaw_degrees`, its bounds'
// bottom at `base` and their middle over (x, z): model files come in any
// units, centred anywhere. ENTITY_NULL (logged) if it didn't load.
static entity_id
add_model_sized(world* w, asset_cache* assets, const char* name, const char* path, f32 x, f32 base, f32 z,
                f32 height, f32 yaw_degrees)
{
    asset_id model = assets_load_model(assets, path);
    if (model == ASSET_NONE)
    {
        LOG_WARN("testbed: %s didn't load; the showcase goes without it\n", path);
        return ENTITY_NULL;
    }

    entity_id    e = add_model(w, name, model, (vec3){ 0.0f, 0.0f, 0.0f }, 1.0f);
    c_transform* t = world_transform(w, e);
    t->rotation_deg[1] = yaw_degrees;

    // Measured at scale 1 at the origin; bounds scale about it.
    vec3 lo, hi;
    world_update(w);
    if (!world_bounds(w, e, lo, hi) || hi[1] - lo[1] <= 0.0f)
        return e;

    f32 s = height / (hi[1] - lo[1]);
    glm_vec3_fill(t->scale, s);
    t->position[0] = x - (lo[0] + hi[0]) * 0.5f * s;
    t->position[1] = base - lo[1] * s;
    t->position[2] = z - (lo[2] + hi[2]) * 0.5f * s;
    return e;
}

static void
add_point_light(world* w, const char* name, vec3 position, vec3 color, f32 intensity, f32 range)
{
    light l;
    light_init_point(&l, color, intensity, range);
    entity_id e = world_create(w, name);
    glm_vec3_copy(position, world_transform(w, e)->position);
    world_add_light(w, e, l);
}

static void
add_spot_light(world* w, const char* name, vec3 position, vec3 direction, vec3 color, f32 intensity, f32 inner,
               f32 outer)
{
    light l;
    light_init_spot(&l, direction, color, intensity, 12.0f, inner, outer);
    entity_id e = world_create(w, name);
    glm_vec3_copy(position, world_transform(w, e)->position);
    world_add_light(w, e, l);
}

// --scene showcase: everything at once -- a small gallery at dusk.
//   - the centrepiece: a pale blue glass Suzanne turning on a marble
//     pedestal under a spot light; the camera focuses on it at f/2.8
//   - left: three car-paint balls (clear-coated red, blue and green) on
//     plinths
//   - right: the knight on a pedestal, lit by a spot
//   - front left: a table of glass bottles -- green, amber, clear -- before
//     a frosted pane
//   - front right: the avocado and the textured box on a stand, and a black
//     ceramic vase with a gold-tinted reflection (specular color)
//   - the back wall: warm fairy lights that blur into bokeh, and lamps
//   - a polished floor, a reflection probe over the room, a low warm sun, a
//     cool fill light, and TAA
static void
build_showcase_scene(world* w, asset_cache* assets)
{
    asset_id plane  = assets_builtin(assets, BUILTIN_PLANE);
    asset_id cube   = assets_builtin(assets, BUILTIN_CUBE);
    asset_id sphere = assets_builtin(assets, BUILTIN_SPHERE);

    // --- The room ---
    entity_id floor = add_model(w, "floor", plane, (vec3){ 0.0f, 0.0f, -0.5f }, 1.0f);
    glm_vec3_copy((vec3){ 14.0f, 1.0f, 11.0f }, world_transform(w, floor)->scale);
    set_material(w, floor, (vec3){ 0.08f, 0.08f, 0.09f }, 0.0f, 0.22f);

    add_block(w, "back_wall", cube, 0.0f, -4.5f, (vec3){ 14.0f, 4.0f, 0.3f }, (vec3){ 0.55f, 0.42f, 0.33f }, 0.0f,
              0.85f);
    add_block(w, "left_wall", cube, -7.0f, -0.5f, (vec3){ 0.3f, 4.0f, 8.0f }, (vec3){ 0.45f, 0.36f, 0.3f }, 0.0f,
              0.85f);

    // Dark wooden panels along the back wall, between the lamps.
    for (int i = 0; i < 5; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "panel_%d", i);
        add_block(w, name, cube, -5.0f + 2.5f * (f32)i, -4.3f, (vec3){ 1.6f, 2.4f, 0.08f },
                  (vec3){ 0.12f, 0.06f, 0.03f }, 0.0f, 0.35f);
    }

    // Fairy lights strung along the back wall, gently sagging.
    for (int i = 0; i < 21; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "fairy_light_%d", i);
        f32 x = -6.0f + 0.6f * (f32)i;
        f32 y = 3.1f - 0.25f * sinf((f32)(i % 7) / 6.0f * 3.14159f);
        entity_id e = add_model(w, name, sphere, (vec3){ x, y, -4.2f }, 0.07f);
        material_override* m = &world_mesh_renderer(w, e)->material;
        m->fields = MATERIAL_OVERRIDE_EMISSIVE | MATERIAL_OVERRIDE_BASE_COLOR;
        glm_vec3_copy((vec3){ 1.0f, 0.8f, 0.5f }, m->base_color);
        glm_vec3_copy((vec3){ 40.0f, 26.0f, 10.0f }, m->emissive);
    }

    // --- The centrepiece: glass Suzanne on a marble pedestal ---
    entity_id pedestal = add_block(w, "pedestal", cube, 0.0f, 0.0f, (vec3){ 0.8f, 0.9f, 0.8f },
                                   (vec3){ 0.92f, 0.9f, 0.86f }, 0.0f, 0.35f);
    material_override* marble = &world_mesh_renderer(w, pedestal)->material;
    marble->fields |= MATERIAL_OVERRIDE_CLEARCOAT | MATERIAL_OVERRIDE_CLEARCOAT_ROUGHNESS;
    marble->clearcoat           = 1.0f;
    marble->clearcoat_roughness = 0.05f;

    entity_id suzanne = add_model_sized(w, assets, "glass_suzanne", "models/suzanne.obj", SHOWCASE_SUBJECT[0], 0.9f,
                                        SHOWCASE_SUBJECT[2], 0.85f, 15.0f);
    if (suzanne)
    {
        set_glass(w, suzanne, 0.02f, 1.5f, 0.5f, (vec3){ 0.6f, 0.8f, 0.97f }, 1.5f);
        world_add_spin(w, suzanne, (vec3){ 0.0f, 20.0f, 0.0f });
    }

    // --- Left: car paint ---
    const vec3 paints[3] = { { 0.55f, 0.02f, 0.02f }, { 0.02f, 0.08f, 0.45f }, { 0.02f, 0.3f, 0.12f } };
    for (int i = 0; i < 3; ++i)
    {
        char name[32];
        f32  x = -4.6f + 1.15f * (f32)i;
        f32  z = -1.2f + 0.5f * (f32)i;
        snprintf(name, sizeof(name), "plinth_%d", i);
        add_block(w, name, cube, x, z, (vec3){ 0.6f, 0.5f, 0.6f }, (vec3){ 0.15f, 0.15f, 0.16f }, 0.0f, 0.6f);

        snprintf(name, sizeof(name), "car_paint_%d", i);
        entity_id ball = add_model(w, name, sphere, (vec3){ x, 0.5f + 0.4f, z }, 0.8f);
        set_material(w, ball, (vec3){ paints[i][0], paints[i][1], paints[i][2] }, 0.4f, 0.45f);
        material_override* m = &world_mesh_renderer(w, ball)->material;
        m->fields |= MATERIAL_OVERRIDE_CLEARCOAT | MATERIAL_OVERRIDE_CLEARCOAT_ROUGHNESS;
        m->clearcoat           = 1.0f;
        m->clearcoat_roughness = 0.03f;
    }

    // --- Right: the knight ---
    add_block(w, "knight_pedestal", cube, 3.6f, -1.6f, (vec3){ 1.1f, 0.4f, 1.1f }, (vec3){ 0.2f, 0.2f, 0.22f }, 0.0f,
              0.5f);
    add_model_sized(w, assets, "knight", "models/bulky_knight.glb", 3.6f, 0.4f, -1.6f, 1.9f, -25.0f);

    // --- Front left: glass bottles before a frosted pane ---
    add_block(w, "bottle_table", cube, -2.4f, 1.6f, (vec3){ 1.6f, 0.7f, 0.8f }, (vec3){ 0.3f, 0.16f, 0.08f }, 0.0f,
              0.4f);
    const struct { const char* name; f32 x, z, height; vec3 tint; f32 distance; } bottles[3] = {
        { "green_bottle", -2.85f, 1.65f, 0.55f, { 0.2f, 0.7f, 0.3f }, 0.25f },
        { "amber_bottle", -2.4f,  1.45f, 0.7f,  { 0.9f, 0.5f, 0.12f }, 0.3f },
        { "clear_bottle", -1.95f, 1.75f, 0.45f, { 1.0f, 1.0f, 1.0f }, 0.0f },
    };
    for (int i = 0; i < 3; ++i)
    {
        f32 h = bottles[i].height;
        entity_id e = add_model(w, bottles[i].name, sphere, (vec3){ bottles[i].x, 0.7f + h * 0.5f, bottles[i].z },
                                1.0f);
        glm_vec3_copy((vec3){ h * 0.32f, h, h * 0.32f }, world_transform(w, e)->scale);
        set_glass(w, e, 0.02f, 1.5f, 1.0f, (vec3){ bottles[i].tint[0], bottles[i].tint[1], bottles[i].tint[2] },
                  bottles[i].distance);
    }
    entity_id pane = add_model(w, "frosted_pane", cube, (vec3){ -2.4f, 0.7f + 0.45f, 1.25f }, 1.0f);
    glm_vec3_copy((vec3){ 1.4f, 0.9f, 0.03f }, world_transform(w, pane)->scale);
    set_glass(w, pane, 0.35f, 1.5f, 0.0f, (vec3){ 1.0f, 1.0f, 1.0f }, 0.0f);

    // --- Front right: the avocado, the box and a vase ---
    add_block(w, "stand", cube, 2.4f, 1.7f, (vec3){ 1.4f, 0.55f, 0.7f }, (vec3){ 0.85f, 0.85f, 0.82f }, 0.0f, 0.5f);
    add_model_sized(w, assets, "avocado", "models/Avocado.glb", 2.1f, 0.55f, 1.75f, 0.32f, 30.0f);
    add_model_sized(w, assets, "textured_box", "models/BoxTextured.glb", 2.75f, 0.55f, 1.6f, 0.25f, -20.0f);

    entity_id vase = add_model(w, "vase", sphere, (vec3){ 1.2f, 0.45f, 1.0f }, 1.0f);
    glm_vec3_copy((vec3){ 0.5f, 0.9f, 0.5f }, world_transform(w, vase)->scale);
    set_material(w, vase, (vec3){ 0.02f, 0.02f, 0.02f }, 0.0f, 0.12f);
    material_override* glaze = &world_mesh_renderer(w, vase)->material;
    glaze->fields |= MATERIAL_OVERRIDE_SPECULAR_COLOR;
    glm_vec3_copy((vec3){ 1.0f, 0.75f, 0.3f }, glaze->specular_color);

    // --- Light ---
    light sun;   // low and warm, through the open side
    light_init_directional(&sun, (vec3){ -0.75f, -0.35f, -0.4f }, (vec3){ 1.0f, 0.62f, 0.35f }, 2.2f);
    world_add_light(w, world_create(w, "sun"), sun);

    add_point_light(w, "cool_fill", (vec3){ 4.5f, 2.5f, 3.0f }, (vec3){ 0.55f, 0.7f, 1.0f }, 6.0f, 10.0f);
    add_point_light(w, "wall_lamp_left", (vec3){ -3.75f, 2.4f, -3.9f }, (vec3){ 1.0f, 0.7f, 0.4f }, 5.0f, 5.0f);
    add_point_light(w, "wall_lamp_right", (vec3){ 3.75f, 2.4f, -3.9f }, (vec3){ 1.0f, 0.7f, 0.4f }, 5.0f, 5.0f);
    add_spot_light(w, "suzanne_spot", (vec3){ 0.3f, 3.8f, 0.8f }, (vec3){ -0.08f, -1.0f, -0.22f },
                   (vec3){ 1.0f, 0.95f, 0.85f }, 30.0f, 10.0f, 20.0f);
    add_spot_light(w, "knight_spot", (vec3){ 2.2f, 3.8f, 0.4f }, (vec3){ 0.45f, -1.0f, -0.6f },
                   (vec3){ 0.85f, 0.9f, 1.0f }, 35.0f, 12.0f, 24.0f);

    // The floor and the glass reflect the room itself, not only the sky.
    entity_id probe = world_create(w, "room_probe");
    glm_vec3_copy((vec3){ 0.0f, 1.6f, -0.5f }, world_transform(w, probe)->position);
    world_add_reflection_probe(w, probe, (vec3){ 7.0f, 2.0f, 4.2f });
}

// A light with no model: `l` at `position`.
static entity_id
add_light_at(world* w, const char* name, vec3 position, light l)
{
    entity_id e = world_create(w, name);
    glm_vec3_copy(position, world_transform(w, e)->position);
    world_add_light(w, e, l);
    return e;
}

// An empty entity at `position` under `parent` (ENTITY_NULL: the root),
// turning `spin` degrees a second about each axis (0: not at all).
static entity_id
add_pivot(world* w, const char* name, entity_id parent, vec3 position, vec3 spin)
{
    entity_id e = world_create(w, name);
    if (parent)
        world_set_parent(w, e, parent);
    glm_vec3_copy(position, world_transform(w, e)->position);
    if (spin[0] != 0.0f || spin[1] != 0.0f || spin[2] != 0.0f)
        world_add_spin(w, e, spin);
    return e;
}

static void
set_emissive(world* w, entity_id e, vec3 color, f32 strength)
{
    material_override* m = &world_mesh_renderer(w, e)->material;
    m->fields |= MATERIAL_OVERRIDE_EMISSIVE | MATERIAL_OVERRIDE_BASE_COLOR;
    glm_vec3_copy(color, m->base_color);
    glm_vec3_scale(color, strength, m->emissive);
}

// --scene features: every engine feature in one place -- an evening plaza
// you walk through (its camera has a PlayerController: click, then WASD and
// the mouse, or a gamepad). From where you start, looking ahead:
//   - centre: an orrery. A glowing sun (emissive, blooming) holds a point
//     light that casts shadows; a planet and its moon orbit it through a
//     chain of parented, spinning entities (hierarchy, Spin)
//   - left: the material grid, dielectric in front and metal behind, smooth
//     to rough
//   - right: glass and coatings before a checkerboard -- clear, tinted and
//     frosted glass, a clear-coated car-paint ball, a gold-glazed vase
//     (transmission, IOR, attenuation, clearcoat, specular color)
//   - back left: the knight (glTF) under a spot light that casts shadows
//   - back right: Suzanne (OBJ), polished and spinning, beside the avocado
//     and the textured box (glTF textures)
//   - front left: a light garden -- 49 small coloured point lights, which
//     the clustered lighting shades only where each reaches
//   - back centre: a glowing archway, a SceneTrigger into the showcase
//   - around it all: columns casting the sun's cascaded shadows, a polished
//     floor under a reflection probe, the HDRI sky
// The environment (testbed_apply_environment()): auto-exposure, AgX tone
// mapping, bloom, SSAO; TAA.
static void
build_features_scene(world* w, asset_cache* assets)
{
    asset_id plane  = assets_builtin(assets, BUILTIN_PLANE);
    asset_id cube   = assets_builtin(assets, BUILTIN_CUBE);
    asset_id sphere = assets_builtin(assets, BUILTIN_SPHERE);

    // --- The player: walks the plaza from the front, looking in ---
    entity_id player = world_create(w, "player");
    c_transform* pt = world_transform(w, player);
    glm_vec3_copy((vec3){ 0.0f, 1.7f, 11.0f }, pt->position);
    pt->rotation_deg[0] = -6.0f;
    world_add_camera(w, player, 60.0f, 0.1f, 200.0f);
    world_add_player_controller(w, player, PLAYER_WALK);

    // --- The plaza ---
    entity_id floor = add_model(w, "floor", plane, (vec3){ 0.0f, 0.0f, 0.0f }, 1.0f);
    glm_vec3_copy((vec3){ 34.0f, 1.0f, 34.0f }, world_transform(w, floor)->scale);
    set_material(w, floor, (vec3){ 0.32f, 0.3f, 0.28f }, 0.0f, 0.28f);

    // Columns all round: the low sun lays their shadows across the plaza,
    // near and far (the cascades).
    for (int i = 0; i < 24; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "column_%d", i);
        int side = i / 6, k = i % 6;
        f32 along = -12.5f + 5.0f * (f32)k;
        f32 x = side == 0 ? along : side == 1 ? 15.0f : side == 2 ? -along : -15.0f;
        f32 z = side == 0 ? -15.0f : side == 1 ? along : side == 2 ? 15.0f : -along;
        add_block(w, name, cube, x, z, (vec3){ 0.6f, 5.0f, 0.6f }, (vec3){ 0.8f, 0.76f, 0.7f }, 0.0f, 0.6f);
    }

    // The room's own reflection for the polished floor, glass and metal.
    entity_id probe = world_create(w, "plaza_probe");
    glm_vec3_copy((vec3){ 0.0f, 2.5f, 0.0f }, world_transform(w, probe)->position);
    world_add_reflection_probe(w, probe, (vec3){ 15.0f, 2.5f, 15.0f });

    // --- Centre: the orrery ---
    add_block(w, "dais", cube, 0.0f, 0.0f, (vec3){ 6.0f, 0.3f, 6.0f }, (vec3){ 0.15f, 0.16f, 0.2f }, 0.0f, 0.3f);
    add_block(w, "sun_column", cube, 0.0f, 0.0f, (vec3){ 0.3f, 1.2f, 0.3f }, (vec3){ 0.7f, 0.55f, 0.3f }, 1.0f, 0.3f);

    entity_id sun_ball = add_model(w, "orrery_sun", sphere, (vec3){ 0.0f, 1.6f, 0.0f }, 0.8f);
    set_emissive(w, sun_ball, (vec3){ 1.0f, 0.75f, 0.35f }, 25.0f);
    world_mesh_renderer(w, sun_ball)->cast_shadows = false;   // the light is inside it

    light core;   // the first point light in the world: it gets a shadow map
    light_init_point(&core, (vec3){ 1.0f, 0.8f, 0.55f }, 25.0f, 12.0f);
    add_light_at(w, "orrery_light", (vec3){ 0.0f, 1.6f, 0.0f }, core);

    // pivot (turns) -> planet_orbit (2.2 m out, turns) -> planet (sized), moon (0.7 m out)
    entity_id pivot = add_pivot(w, "orrery_pivot", ENTITY_NULL, (vec3){ 0.0f, 1.6f, 0.0f }, (vec3){ 0.0f, 25.0f, 0.0f });
    entity_id orbit = add_pivot(w, "planet_orbit", pivot, (vec3){ 2.2f, 0.0f, 0.0f }, (vec3){ 0.0f, 90.0f, 0.0f });
    entity_id planet = add_model(w, "planet", sphere, (vec3){ 0.0f, 0.0f, 0.0f }, 0.55f);
    world_set_parent(w, planet, orbit);
    set_material(w, planet, (vec3){ 0.1f, 0.3f, 0.75f }, 0.0f, 0.45f);
    entity_id moon = add_model(w, "moon", sphere, (vec3){ 0.7f, 0.1f, 0.0f }, 0.18f);
    world_set_parent(w, moon, orbit);
    set_material(w, moon, (vec3){ 0.75f, 0.75f, 0.72f }, 0.0f, 0.8f);

    // --- Left: the material grid ---
    add_block(w, "grid_plinth", cube, -8.0f, -0.6f, (vec3){ 6.6f, 0.4f, 2.6f }, (vec3){ 0.12f, 0.12f, 0.13f }, 0.0f,
              0.7f);
    add_block(w, "grid_step", cube, -8.0f, -1.2f, (vec3){ 6.6f, 0.9f, 1.2f }, (vec3){ 0.12f, 0.12f, 0.13f }, 0.0f,
              0.7f);   // the metal row stands higher, over the front one
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 5; ++column)
        {
            char name[32];
            snprintf(name, sizeof(name), "%s_%d", row ? "metal" : "dielectric", column);
            f32 roughness = 0.05f + 0.95f * (f32)column / 4.0f;
            entity_id e = add_model(w, name, sphere,
                                    (vec3){ -10.4f + 1.2f * (f32)column, row ? 1.35f : 0.85f, row ? -1.2f : 0.0f }, 0.9f);
            if (row)
                set_material(w, e, (vec3){ 0.95f, 0.64f, 0.54f }, 1.0f, roughness);   // copper
            else
                set_material(w, e, (vec3){ 0.1f, 0.5f, 0.2f }, 0.0f, roughness);
        }
    }

    // --- Right: glass and coatings before a checkerboard ---
    for (int row = 0; row < 6; ++row)
        for (int column = 0; column < 10; ++column)
        {
            char name[32];
            snprintf(name, sizeof(name), "tile_%d_%d", row, column);
            entity_id e = add_model(w, name, cube, (vec3){ 5.5f + 0.4f * (f32)column, 0.2f + 0.4f * (f32)row, -2.2f },
                                    1.0f);
            glm_vec3_copy((vec3){ 0.4f, 0.4f, 0.05f }, world_transform(w, e)->scale);
            b8 light_tile = (row + column) % 2 == 0;
            set_material(w, e, light_tile ? (vec3){ 0.85f, 0.85f, 0.82f } : (vec3){ 0.05f, 0.05f, 0.06f }, 0.0f, 0.6f);
        }

    entity_id clear = add_model(w, "clear_glass", sphere, (vec3){ 6.0f, 0.5f, -0.5f }, 1.0f);
    set_glass(w, clear, 0.0f, 1.5f, 1.0f, (vec3){ 1.0f, 1.0f, 1.0f }, 0.0f);

    entity_id bottle = add_model(w, "tinted_bottle", sphere, (vec3){ 7.2f, 0.75f, -0.6f }, 1.0f);
    glm_vec3_copy((vec3){ 0.5f, 1.5f, 0.5f }, world_transform(w, bottle)->scale);
    set_glass(w, bottle, 0.02f, 1.5f, 1.0f, (vec3){ 0.9f, 0.5f, 0.12f }, 0.35f);

    entity_id pane = add_model(w, "frosted_pane", cube, (vec3){ 8.5f, 0.75f, -0.9f }, 1.0f);
    glm_vec3_copy((vec3){ 1.0f, 1.5f, 0.04f }, world_transform(w, pane)->scale);
    set_glass(w, pane, 0.35f, 1.5f, 0.0f, (vec3){ 1.0f, 1.0f, 1.0f }, 0.0f);

    entity_id paint = add_model(w, "car_paint", sphere, (vec3){ 6.4f, 0.45f, 1.2f }, 0.9f);
    set_material(w, paint, (vec3){ 0.55f, 0.02f, 0.02f }, 0.4f, 0.45f);
    material_override* coat = &world_mesh_renderer(w, paint)->material;
    coat->fields |= MATERIAL_OVERRIDE_CLEARCOAT | MATERIAL_OVERRIDE_CLEARCOAT_ROUGHNESS;
    coat->clearcoat           = 1.0f;
    coat->clearcoat_roughness = 0.03f;

    entity_id vase = add_model(w, "glazed_vase", sphere, (vec3){ 7.8f, 0.45f, 1.3f }, 1.0f);
    glm_vec3_copy((vec3){ 0.5f, 0.9f, 0.5f }, world_transform(w, vase)->scale);
    set_material(w, vase, (vec3){ 0.02f, 0.02f, 0.02f }, 0.0f, 0.12f);
    material_override* glaze = &world_mesh_renderer(w, vase)->material;
    glaze->fields |= MATERIAL_OVERRIDE_SPECULAR | MATERIAL_OVERRIDE_SPECULAR_COLOR;
    glaze->specular = 1.0f;
    glm_vec3_copy((vec3){ 1.0f, 0.75f, 0.3f }, glaze->specular_color);

    // --- Back left: the knight under a spot ---
    add_block(w, "knight_pedestal", cube, -7.0f, -8.0f, (vec3){ 1.4f, 0.4f, 1.4f }, (vec3){ 0.2f, 0.2f, 0.22f }, 0.0f,
              0.5f);
    add_model_sized(w, assets, "knight", "models/bulky_knight.glb", -7.0f, 0.4f, -8.0f, 2.0f, 20.0f);
    add_spot_light(w, "knight_spot", (vec3){ -5.5f, 4.5f, -5.5f }, (vec3){ -0.4f, -1.0f, -0.65f },
                   (vec3){ 0.85f, 0.9f, 1.0f }, 60.0f, 14.0f, 26.0f);

    // --- Back right: Suzanne (OBJ), the avocado and the textured box (glTF) ---
    add_block(w, "model_stand", cube, 7.0f, -8.0f, (vec3){ 3.0f, 0.8f, 1.2f }, (vec3){ 0.85f, 0.85f, 0.82f }, 0.0f, 0.5f);
    entity_id suzanne = add_model_sized(w, assets, "suzanne", "models/suzanne.obj", 6.0f, 0.8f, -8.0f, 0.9f, 0.0f);
    if (suzanne)
    {
        set_material(w, suzanne, (vec3){ 0.95f, 0.95f, 0.95f }, 1.0f, 0.1f);
        world_add_spin(w, suzanne, (vec3){ 0.0f, 30.0f, 0.0f });
    }
    add_model_sized(w, assets, "avocado", "models/Avocado.glb", 7.4f, 0.8f, -8.0f, 0.45f, 30.0f);
    add_model_sized(w, assets, "textured_box", "models/BoxTextured.glb", 8.2f, 0.8f, -8.0f, 0.4f, -20.0f);

    // --- Front left: the light garden -- 49 lights, no shadows ---
    add_block(w, "garden_bed", cube, -5.0f, 6.0f, (vec3){ 5.6f, 0.1f, 5.6f }, (vec3){ 0.04f, 0.04f, 0.05f }, 0.0f, 0.5f);
    for (int i = 0; i < 49; ++i)
    {
        char name[32];
        int  gx = i % 7, gz = i / 7;
        f32  hue = (f32)i / 49.0f * 6.0f;
        vec3 color = { fmaxf(0.0f, fabsf(hue - 3.0f) - 1.0f), fmaxf(0.0f, 2.0f - fabsf(hue - 2.0f)),
                       fmaxf(0.0f, 2.0f - fabsf(hue - 4.0f)) };
        for (int c = 0; c < 3; ++c)
            color[c] = fminf(color[c], 1.0f);
        vec3 at = { -7.4f + 0.8f * (f32)gx, 0.35f, 3.6f + 0.8f * (f32)gz };

        snprintf(name, sizeof(name), "glow_%d", i);
        entity_id bulb = add_model(w, name, sphere, at, 0.08f);
        set_emissive(w, bulb, color, 12.0f);
        world_mesh_renderer(w, bulb)->cast_shadows = false;

        light l;
        light_init_point(&l, color, 1.5f, 1.6f);
        l.cast_shadows = false;
        snprintf(name, sizeof(name), "glow_light_%d", i);
        add_light_at(w, name, at, l);
    }

    // --- Back centre: the archway to the showcase ---
    add_block(w, "arch_left", cube, -1.5f, -11.0f, (vec3){ 0.5f, 3.5f, 0.6f }, (vec3){ 0.8f, 0.76f, 0.7f }, 0.0f, 0.5f);
    add_block(w, "arch_right", cube, 1.5f, -11.0f, (vec3){ 0.5f, 3.5f, 0.6f }, (vec3){ 0.8f, 0.76f, 0.7f }, 0.0f, 0.5f);
    entity_id lintel = add_model(w, "arch_top", cube, (vec3){ 0.0f, 3.75f, -11.0f }, 1.0f);
    glm_vec3_copy((vec3){ 3.5f, 0.5f, 0.6f }, world_transform(w, lintel)->scale);
    set_material(w, lintel, (vec3){ 0.8f, 0.76f, 0.7f }, 0.0f, 0.5f);
    const vec3 strips[3] = { { -1.22f, 1.75f, -10.68f }, { 1.22f, 1.75f, -10.68f }, { 0.0f, 3.47f, -10.68f } };
    for (int i = 0; i < 3; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "arch_glow_%d", i);
        entity_id e = add_model(w, name, cube, (vec3){ strips[i][0], strips[i][1], strips[i][2] }, 1.0f);
        glm_vec3_copy(i < 2 ? (vec3){ 0.06f, 3.4f, 0.04f } : (vec3){ 2.5f, 0.06f, 0.04f }, world_transform(w, e)->scale);
        set_emissive(w, e, (vec3){ 0.3f, 0.8f, 1.0f }, 15.0f);
    }
    entity_id door = world_create(w, "to_showcase");
    glm_vec3_copy((vec3){ 0.0f, 1.5f, -11.0f }, world_transform(w, door)->position);
    c_scene_trigger* trigger = world_add_scene_trigger(w, door, (vec3){ 1.2f, 1.5f, 0.4f });
    snprintf(trigger->scene, sizeof(trigger->scene), "showcase");
    trigger->fade_seconds = 0.6f;

    // --- The sun: low and warm ---
    light sun;
    light_init_directional(&sun, (vec3){ -0.55f, -0.45f, -0.6f }, (vec3){ 1.0f, 0.75f, 0.5f }, 2.0f);
    add_light_at(w, "sun", (vec3){ 0.0f, 10.0f, 0.0f }, sun);
}

static void
build_scene(world* w, asset_cache* assets)
{
    asset_id plane  = assets_builtin(assets, BUILTIN_PLANE);
    asset_id cube   = assets_builtin(assets, BUILTIN_CUBE);
    asset_id sphere = assets_builtin(assets, BUILTIN_SPHERE);

    // Ground.
    entity_id ground = add_model(w, "ground", plane, (vec3){ 0.0f, 0.0f, 0.0f }, 1.0f);
    glm_vec3_copy((vec3){ 16.0f, 1.0f, 12.0f }, world_transform(w, ground)->scale);
    set_material(w, ground, (vec3){ 0.18f, 0.18f, 0.2f }, 0.0f, 0.85f);

    // The material grid: roughness 0.05 .. 1 across, dielectric (red) in
    // front, metal (gold) behind.
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < GRID_COLUMNS; ++column)
        {
            char name[32];
            snprintf(name, sizeof(name), "%s_%d", row ? "metal" : "dielectric", column);

            f32 roughness = 0.05f + 0.95f * (f32)column / (f32)(GRID_COLUMNS - 1);
            f32 x         = ((f32)column - (GRID_COLUMNS - 1) * 0.5f) * 1.1f;
            f32 z         = row ? -1.2f : 0.0f;

            entity_id e = add_model(w, name, sphere, (vec3){ x, 0.45f, z }, 0.9f);
            if (row)
                set_material(w, e, (vec3){ 1.0f, 0.78f, 0.34f }, 1.0f, roughness);
            else
                set_material(w, e, (vec3){ 0.8f, 0.12f, 0.1f }, 0.0f, roughness);
        }
    }

    // A polished Suzanne turning on a pedestal, in front of the grid.
    entity_id pedestal = add_model(w, "pedestal", cube, (vec3){ 0.0f, 0.4f, 2.2f }, 1.0f);
    glm_vec3_copy((vec3){ 0.9f, 0.8f, 0.9f }, world_transform(w, pedestal)->scale);
    set_material(w, pedestal, (vec3){ 0.9f, 0.9f, 0.88f }, 0.0f, 0.4f);

    asset_id suzanne = assets_load_model(assets, "models/suzanne.obj");
    if (suzanne != ASSET_NONE)
    {
        entity_id head = add_model(w, "suzanne", suzanne, (vec3){ 0.0f, 0.0f, 0.0f }, 0.5f);
        set_material(w, head, (vec3){ 0.95f, 0.95f, 0.95f }, 1.0f, 0.12f);
        world_add_spin(w, head, (vec3){ 0.0f, 30.0f, 0.0f });

        // The mesh isn't centred on its origin: put its bounds' centre over
        // the pedestal and their bottom on its top.
        vec3 lo, hi;
        world_update(w);
        if (world_bounds(w, head, lo, hi))
        {
            c_transform* t  = world_transform(w, head);
            t->position[0] -= (lo[0] + hi[0]) * 0.5f - 0.0f;
            t->position[1] -= lo[1] - 0.8f;
            t->position[2] -= (lo[2] + hi[2]) * 0.5f - 2.2f;
        }
    }
    else
        LOG_WARN("testbed: models/suzanne.obj didn't load; the pedestal stays empty\n");

    // Lights: a sun from the upper left, a warm point light by the grid and
    // a cool spot light on the pedestal.
    light sun;
    light_init_directional(&sun, (vec3){ -0.4f, -1.0f, -0.5f }, (vec3){ 1.0f, 0.96f, 0.9f }, 3.0f);
    world_add_light(w, world_create(w, "sun"), sun);

    light warm;
    light_init_point(&warm, (vec3){ 1.0f, 0.6f, 0.3f }, 8.0f, 6.0f);
    entity_id lamp = world_create(w, "warm_lamp");
    glm_vec3_copy((vec3){ -3.5f, 1.5f, 1.0f }, world_transform(w, lamp)->position);
    world_add_light(w, lamp, warm);

    light spot;
    light_init_spot(&spot, (vec3){ 0.0f, -1.0f, 0.0f }, (vec3){ 0.6f, 0.75f, 1.0f }, 20.0f, 8.0f, 15.0f, 25.0f);
    entity_id spot_entity = world_create(w, "spot");
    glm_vec3_copy((vec3){ 0.0f, 4.0f, 2.2f }, world_transform(w, spot_entity)->position);
    world_add_light(w, spot_entity, spot);
}

// Points the orbit camera at the grid and the pedestal (not the whole ground,
// which would leave them small in the middle).
static void
frame_scene(testbed_state* state)
{
    vec3 lo = { -4.0f, 0.0f, -1.8f };
    vec3 hi = { 4.0f, 1.6f, 2.8f };
    if (state->scene == TESTBED_SCENE_GLASS)   // the glass, with the wall filling the background
    {
        glm_vec3_copy((vec3){ -2.6f, 0.0f, -0.6f }, lo);
        glm_vec3_copy((vec3){ 3.0f, 1.8f, 0.6f }, hi);
    }
    else if (state->scene == TESTBED_SCENE_COAT)   // both rows of balls
    {
        glm_vec3_copy((vec3){ -3.3f, 0.0f, -1.1f }, lo);
        glm_vec3_copy((vec3){ 3.3f, 1.0f, 1.5f }, hi);
    }
    else if (state->scene == TESTBED_SCENE_FOCUS)   // the bottle, close
    {
        glm_vec3_copy((vec3){ -0.6f, 0.0f, -0.3f }, lo);
        glm_vec3_copy((vec3){ 0.6f, 1.7f, 0.3f }, hi);
    }
    else if (state->scene == TESTBED_SCENE_SHOWCASE)   // the room, from the front
    {
        glm_vec3_copy((vec3){ -3.0f, 0.4f, -2.0f }, lo);
        glm_vec3_copy((vec3){ 3.4f, 2.0f, 1.6f }, hi);
    }

    // The focus scene through a portrait lens (20 degrees: about 68 mm on
    // full frame), whose shallow focus shows; a wide one hardly blurs at all.
    // The showcase through a normal one (35 degrees, about 38 mm).
    f32 fov = state->scene == TESTBED_SCENE_FOCUS ? 20.0f : state->scene == TESTBED_SCENE_SHOWCASE ? 35.0f : 45.0f;
    camera_init(&state->cam, (vec3){ 0.0f, 0.0f, 0.0f }, (vec3){ 0.0f, 0.0f, 0.0f }, fov, state->aspect, 0.1f, 100.0f);
    state->orbit = state->scene == TESTBED_SCENE_MATERIALS ? (orbit_camera){ .yaw = 20.0f, .pitch = 22.0f }
                 : state->scene == TESTBED_SCENE_GLASS     ? (orbit_camera){ .yaw = 0.0f, .pitch = 8.0f }
                 : state->scene == TESTBED_SCENE_COAT      ? (orbit_camera){ .yaw = 0.0f, .pitch = 18.0f }
                 : state->scene == TESTBED_SCENE_SHOWCASE  ? (orbit_camera){ .yaw = 3.0f, .pitch = 9.0f }
                                                           : (orbit_camera){ .yaw = 12.0f, .pitch = 6.0f };
    orbit_fit(&state->orbit, &state->cam, lo, hi, 1.0f);
    if (state->scene == TESTBED_SCENE_SHOWCASE)
        state->orbit.distance = 8.5f;   // inside the room, the walls filling the view
    orbit_apply(&state->orbit, &state->cam);

    // Depth of field: focused on the focus or showcase scene's subject, unless
    // told otherwise -- its depth along the view, which is what the lens measures.
    state->cam.aperture = state->aperture >= 0.0f ? state->aperture
                        : state->scene == TESTBED_SCENE_FOCUS    ? 2.0f
                        : state->scene == TESTBED_SCENE_SHOWCASE ? 2.8f : 0.0f;
    vec3 forward, to_subject;
    glm_vec3_sub(state->cam.target, state->cam.position, forward);
    glm_vec3_normalize(forward);
    glm_vec3_sub(state->scene == TESTBED_SCENE_SHOWCASE ? (f32*)SHOWCASE_SUBJECT : (f32*)FOCUS_SUBJECT,
                 state->cam.position, to_subject);
    state->cam.focus_distance = state->focus > 0.0f ? state->focus : glm_vec3_dot(forward, to_subject);
}

// The HDRI and the rest of the light the scene is seen in.
static void
apply_scene_environment(testbed_state* state)
{
    state->env = scene_environment_default();
    snprintf(state->env.hdri, sizeof(state->env.hdri), "%s",
             state->scene == TESTBED_SCENE_SHOWCASE || state->scene == TESTBED_SCENE_FEATURES ? SHOWCASE_HDRI : HDRI);

    if (state->scene == TESTBED_SCENE_FEATURES)   // the environment's options, all on show
    {
        state->env.auto_exposure    = true;
        state->env.tonemap          = SCENE_TONEMAP_AGX;
        state->env.bloom            = 0.06f;
        state->env.ssao             = true;
        state->env.ssao_radius      = 0.6f;
        state->env.ambient_strength = 0.6f;
    }
    ibl_cache_sync(&state->ibl, state->env.hdri);
}

// Replaces the world with `scene`, built, lit and framed.
static void
load_testbed_scene(testbed_state* state, testbed_scene scene)
{
    state->scene = scene;
    world_clear(&state->world);
    switch (scene)
    {
        case TESTBED_SCENE_GLASS:    build_glass_scene(&state->world, &state->assets);    break;
        case TESTBED_SCENE_FOCUS:    build_focus_scene(&state->world, &state->assets);    break;
        case TESTBED_SCENE_COAT:     build_coat_scene(&state->world, &state->assets);     break;
        case TESTBED_SCENE_SHOWCASE: build_showcase_scene(&state->world, &state->assets); break;
        case TESTBED_SCENE_FEATURES: build_features_scene(&state->world, &state->assets); break;
        default:                     build_scene(&state->world, &state->assets);          break;
    }
    world_update(&state->world);
    LOG_INFO("testbed: built '%s', %u entities\n", TESTBED_SCENE_NAMES[scene], world_count(&state->world));

    apply_scene_environment(state);
    frame_scene(state);

    state->player = world_next(&state->world, ENTITY_NULL,
                               COMPONENT_BIT(COMPONENT_CAMERA) | COMPONENT_BIT(COMPONENT_PLAYER_CONTROLLER));
}

// A SceneTrigger's scene, by its testbed name. False (logged) for a name
// that isn't one: the scene that was showing stays.
static b8
enter_scene_named(testbed_state* state, const char* name)
{
    for (int s = 0; s < TESTBED_SCENE_COUNT; ++s)
        if (strcmp(name, TESTBED_SCENE_NAMES[s]) == 0)
        {
            load_testbed_scene(state, (testbed_scene)s);
            return true;
        }
    LOG_ERROR("testbed: a SceneTrigger names '%s', which isn't a testbed scene\n", name);
    return false;
}

// --- App hooks -------------------------------------------------------------------

static void
on_init(App* app)
{
    testbed_state* state = (testbed_state*)app->user_data;
    window_size    size  = window_get_size(get_renderer()->drawing_window);
    state->aspect        = size.height ? (f32)size.width / (f32)size.height : 1.0f;

    if (!scene_renderer_create(&state->renderer, size.width, size.height, CASCADE_COUNT, SHADOW_MAP_SIZE))
        FATAL("testbed: failed to create the scene renderer");

    assets_init(&state->assets);
    world_init(&state->world, &state->assets);
    load_testbed_scene(state, state->scene);
    scene_transition_reset(&state->transition, &state->world);
}

static void
on_resize(App* app, uint16 width, uint16 height)
{
    testbed_state* state = (testbed_state*)app->user_data;
    if (width == 0 || height == 0)   // minimized
        return;

    state->aspect = (f32)width / (f32)height;
    camera_set_aspect(&state->cam, state->aspect);
    scene_renderer_resize(&state->renderer, width, height);
}

// --check: the frame as a PNG at check_path. False (logged) if it can't be
// rendered or written.
static b8
save_check_picture(testbed_state* state, const scene_render_settings* settings)
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
    testbed_state* state = (testbed_state*)app->user_data;

    window* wind   = get_renderer()->drawing_window;
    b8      locked = input_cursor_mode() == WINDOW_CURSOR_LOCKED;
    b8      walk   = state->player && !state->check_path && !state->timing_frames;

    // Esc gives the mouse back first, then quits; walking, a click takes it.
    if (input_key_pressed(INPUT_KEY_ESCAPE))
    {
        if (locked)
            input_set_cursor_mode(wind, WINDOW_CURSOR_NORMAL);
        else
            app->should_close = true;
    }
    else if (walk && !locked && input_mouse_pressed(INPUT_MOUSE_LEFT))
        input_set_cursor_mode(wind, WINDOW_CURSOR_LOCKED);
    else if (!walk && locked)
        input_set_cursor_mode(wind, WINDOW_CURSOR_NORMAL);   // a trigger led to a scene that's orbited

    // A check's picture is the scene as built, whatever the frame times.
    if (!state->check_path)
    {
        if (walk)
        {
            player_input input = { 0 };   // held still while the view fades between scenes
            if (!scene_transition_busy(&state->transition))
                input = player_input_from_actions();
            player_controller_tick(&state->world, fminf(dt, 0.1f), &input);
        }
        world_tick(&state->world, fminf(dt, 0.1f));
    }
    world_update(&state->world);

    // Not only while walking: a trigger may lead to an orbited scene, which
    // still has to fade in.
    if (!state->check_path && !state->timing_frames)
    {
        const char* next = scene_transition_update(&state->transition, &state->world, fminf(dt, 0.1f));
        if (next)
            scene_transition_loaded(&state->transition, &state->world, enter_scene_named(state, next));
    }

    // Walked: the view is the player's Camera. Orbited: drag and wheel.
    if (state->player && world_camera_view(&state->world, state->player, state->aspect, &state->cam))
    {
        if (state->aperture > 0.0f)
        {
            state->cam.aperture       = state->aperture;
            state->cam.focus_distance = state->focus > 0.0f ? state->focus : 5.0f;
        }
    }
    else
    {
        f32 dx = 0.0f, dy = 0.0f;
        if (input_mouse_down(INPUT_MOUSE_LEFT))
            input_mouse_delta(&dx, &dy);
        if (dx != 0.0f || dy != 0.0f)
            orbit_drag(&state->orbit, &state->cam, dx, dy);

        f32 wheel_x, wheel;
        input_scroll(&wheel_x, &wheel);
        if (wheel != 0.0f)
            orbit_zoom(&state->orbit, &state->cam, wheel);
    }

    scene_render_settings settings = scene_render_settings_default();
    scene_render_settings_apply_environment(&settings, &state->env, ibl_cache_maps(&state->ibl));
    settings.debug_view = state->debug_view;
    if (state->dof_taps)
        settings.dof_taps = state->dof_taps;
    if (state->antialiasing_set)
        settings.antialiasing = state->antialiasing;
    else if (state->scene == TESTBED_SCENE_SHOWCASE || state->scene == TESTBED_SCENE_FEATURES)
        settings.antialiasing = SCENE_AA_TAA;   // smooths the glass and the depth of field's edges

    if (state->turn_degrees != 0.0f)
    {
        state->orbit.yaw += state->turn_degrees;
        orbit_apply(&state->orbit, &state->cam);
    }
    settings.delta_time = state->check_path ? -1.0f : dt;
    settings.exposure  *= 1.0f - scene_transition_fade(&state->transition);

    scene_renderer_draw(&state->renderer, &state->world, &state->cam, &settings);

    if (state->check_path && ++state->check_frames >= CHECK_FRAMES && !state->timing_frames)
    {
        state->check_passed = save_check_picture(state, &settings);
        app->should_close   = true;
    }

    if (state->timing_frames && ++state->check_frames >= state->timing_frames)
    {
        const gpu_profiler* p = &state->renderer.profiler;
        printf("GPU timings at %u x %u after %u frames (ms, smoothed):\n", state->renderer.post.width,
               state->renderer.post.height, state->timing_frames);
        for (uint32 i = 0; i < p->timing_count; ++i)
            printf("  %-14s %7.3f\n", p->timings[i].name, p->timings[i].ms);
        printf("  %-14s %7.3f\n", "frame", p->frame_ms);
        if (state->check_path)
            state->check_passed = save_check_picture(state, &settings);
        app->should_close = true;
    }
}

static void
on_shutdown(App* app)
{
    testbed_state* state = (testbed_state*)app->user_data;

    world_shutdown(&state->world);    // releases the world's assets...
    assets_shutdown(&state->assets);  // ...then the models themselves
    ibl_cache_destroy(&state->ibl);
    scene_renderer_destroy(&state->renderer);
}

static void
print_usage(void)
{
    printf("usage: Testbed [--scene NAME] [--check PNG] [--view NAME] [--timings N] [--size WxH]\n"
           "               [--aa NAME] [--turn DEG] [--aperture F] [--focus M] [--dof-taps N]\n"
           "  --scene  materials (the default), glass, focus, coat, showcase or features (walk: click, WASD, mouse)\n"
           "  --aperture  depth of field's f-number (0 = off; focus scene: 2, showcase: 2.8)\n"
           "  --focus  meters to focus at (default: the focus scene's subject)\n"
           "  --dof-taps  depth of field's samples per pixel, 8..128 (default 32)\n"
           "  --timings  draw N frames, hidden, print each pass's GPU time and exit\n"
           "  --size   the window's (or picture's) size, e.g. 1920x1080\n"
           "  --aa     anti-aliasing: off, fxaa (the default) or taa\n"
           "  --turn   orbit the camera this many degrees every frame\n"
           "  --check  render the scene once, hidden, into PNG and exit (0 = it was written)\n"
           "  --view   a debug view: albedo, normals, roughness, metallic, ao, shadow, lighting, wireframe, lights,\n"
           "           transmission, behind_glass, focus\n");
}

int main(int argc, char** argv)
{
    testbed_state state = { .aperture = -1.0f };

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        if (strcmp(arg, "--check") == 0)
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
                fprintf(stderr, "--view needs one of: lit, albedo, normals, roughness, metallic, ao, shadow, lighting, wireframe, lights, transmission, behind_glass, focus\n");
                print_usage();
                return 1;
            }
        }
        else if (strcmp(arg, "--aa") == 0)
        {
            if (!scene_antialiasing_from_name(i + 1 < argc ? argv[++i] : NULL, &state.antialiasing))
            {
                fprintf(stderr, "--aa needs off, fxaa or taa\n");
                return 1;
            }
            state.antialiasing_set = true;
        }
        else if (strcmp(arg, "--turn") == 0)
        {
            state.turn_degrees = i + 1 < argc ? (f32)atof(argv[++i]) : 0.0f;
        }
        else if (strcmp(arg, "--timings") == 0)
        {
            int frames = i + 1 < argc ? atoi(argv[++i]) : 0;
            if (frames < GPU_PROFILER_LATENCY + 1)
            {
                fprintf(stderr, "--timings needs a number of frames, at least %d\n", GPU_PROFILER_LATENCY + 1);
                return 1;
            }
            state.timing_frames = (uint32)frames;
        }
        else if (strcmp(arg, "--size") == 0)
        {
            unsigned width = 0, height = 0;
            if (i + 1 >= argc || sscanf(argv[++i], "%ux%u", &width, &height) != 2 || !width || !height
                || width > 8192 || height > 8192)
            {
                fprintf(stderr, "--size needs WIDTHxHEIGHT, e.g. 1920x1080\n");
                return 1;
            }
            state.width  = (uint16)width;
            state.height = (uint16)height;
        }
        else if (strcmp(arg, "--scene") == 0)
        {
            const char* name  = i + 1 < argc ? argv[++i] : "";
            int         found = -1;
            for (int s = 0; s < (int)(sizeof(TESTBED_SCENE_NAMES) / sizeof(TESTBED_SCENE_NAMES[0])); ++s)
                if (strcmp(name, TESTBED_SCENE_NAMES[s]) == 0)
                    found = s;
            if (found < 0)
            {
                fprintf(stderr, "--scene needs materials, glass, focus, coat, showcase or features\n");
                print_usage();
                return 1;
            }
            state.scene = (testbed_scene)found;
        }
        else if (strcmp(arg, "--aperture") == 0)
        {
            state.aperture = i + 1 < argc ? (f32)atof(argv[++i]) : -1.0f;
            if (state.aperture < 0.0f)
            {
                fprintf(stderr, "--aperture needs an f-number (0 = off)\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--dof-taps") == 0)
        {
            state.dof_taps = i + 1 < argc ? atoi(argv[++i]) : 0;
            if (state.dof_taps < 8 || state.dof_taps > 128)
            {
                fprintf(stderr, "--dof-taps needs 8..128\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--focus") == 0)
        {
            state.focus = i + 1 < argc ? (f32)atof(argv[++i]) : 0.0f;
            if (state.focus <= 0.0f)
            {
                fprintf(stderr, "--focus needs a distance in meters\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            print_usage();
            return 0;
        }
        else
        {
            fprintf(stderr, "unknown argument %s\n", arg);
            print_usage();
            return 1;
        }
    }

    App app = {
        .name        = "PBR Testbed",
        .hidden      = state.check_path != NULL || state.timing_frames != 0,
        .width       = state.width ? state.width : state.check_path ? 960 : 1600,
        .height      = state.height ? state.height : state.check_path ? 540 : 900,
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
