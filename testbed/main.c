// testbed/main.c
//
// The testbed: builds a scene in code with the engine's World API -- no scene
// file -- and draws it with the scene renderer. What a program using the
// engine as a library looks like, and a quick way to try a renderer change.
//
//   Testbed [--scene NAME] [--check PNG] [--view NAME]
//     --scene NAME  materials (the default, below), glass (build_glass_scene()),
//                   focus (build_focus_scene()), coat (build_coat_scene())
//                   or showcase (build_showcase_scene(): all of it at once)
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
// Left-drag orbits, the wheel zooms, Esc quits.

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
    TESTBED_SCENE_SHOWCASE     // build_showcase_scene()
} testbed_scene;

static const char* const TESTBED_SCENE_NAMES[] = { "materials", "glass", "focus", "coat", "showcase" };

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
    switch (state->scene)
    {
        case TESTBED_SCENE_GLASS: build_glass_scene(&state->world, &state->assets); break;
        case TESTBED_SCENE_FOCUS: build_focus_scene(&state->world, &state->assets); break;
        case TESTBED_SCENE_COAT:  build_coat_scene(&state->world, &state->assets);  break;
        case TESTBED_SCENE_SHOWCASE: build_showcase_scene(&state->world, &state->assets); break;
        default:                  build_scene(&state->world, &state->assets);       break;
    }
    world_update(&state->world);
    LOG_INFO("testbed: built %u entities\n", world_count(&state->world));

    state->env = scene_environment_default();
    snprintf(state->env.hdri, sizeof(state->env.hdri), "%s",
             state->scene == TESTBED_SCENE_SHOWCASE ? SHOWCASE_HDRI : HDRI);
    ibl_cache_sync(&state->ibl, state->env.hdri);

    frame_scene(state);
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

    if (input_key_pressed(INPUT_KEY_ESCAPE))
        app->should_close = true;

    // A check's picture is the scene as built, whatever the frame times.
    if (!state->check_path)
        world_tick(&state->world, fminf(dt, 0.1f));
    world_update(&state->world);

    f32 dx = 0.0f, dy = 0.0f;
    if (input_mouse_down(INPUT_MOUSE_LEFT))
        input_mouse_delta(&dx, &dy);
    if (dx != 0.0f || dy != 0.0f)
        orbit_drag(&state->orbit, &state->cam, dx, dy);

    f32 wheel_x, wheel;
    input_scroll(&wheel_x, &wheel);
    if (wheel != 0.0f)
        orbit_zoom(&state->orbit, &state->cam, wheel);

    scene_render_settings settings = scene_render_settings_default();
    scene_render_settings_apply_environment(&settings, &state->env, ibl_cache_maps(&state->ibl));
    settings.debug_view = state->debug_view;
    if (state->dof_taps)
        settings.dof_taps = state->dof_taps;
    if (state->antialiasing_set)
        settings.antialiasing = state->antialiasing;
    else if (state->scene == TESTBED_SCENE_SHOWCASE)
        settings.antialiasing = SCENE_AA_TAA;   // smooths the glass and the depth of field's edges

    if (state->turn_degrees != 0.0f)
    {
        state->orbit.yaw += state->turn_degrees;
        orbit_apply(&state->orbit, &state->cam);
    }
    settings.delta_time = state->check_path ? -1.0f : dt;

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
           "  --scene  materials (the default), glass, focus, coat or showcase\n"
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
                fprintf(stderr, "--scene needs materials, glass, focus, coat or showcase\n");
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
