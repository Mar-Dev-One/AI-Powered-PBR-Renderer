// demo.c
//
// Interactive feature demo: a rotating, perspective-projected cube. Exists
// alongside main.c (the automated RHI smoke test) to show the pieces that
// sit above raw RHI calls — Scene/Camera and the new RHI uniform-setting
// API — actually being used together: model/view/projection matrices built
// with cglm, uploaded to the shader every frame, driving real 3D geometry
// instead of a flat NDC triangle.

#include "App.h"
#include "Core/Defines.h"
#include "Core/Paths.h"
#include "RHI/RHI.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Camera.h"
#include "Scene/Model.h"
#include "Assets/AssetCache.h"
#include "World/World.h"
#include "Platform/Window.h"
#include "UI/EntityPanel.h"
#include "UI/ImGuiLayer.h"
#include "cimgui.h"

#include <math.h>
#include <GLFW/glfw3.h>

// Texture picker: every path/name pair the GUI combo box can switch
// between. Add an entry here (and drop the file in assets/textures/) to
// make a new texture selectable -- nothing else needs to change.
static const char* TEXTURE_PATHS[] = {
    "textures/checker.png",
    "textures/stripes.png",
    "textures/uv_grid.png",
};

static const char* TEXTURE_NAMES[] = {
    "Checkerboard",
    "Stripes",
    "UV Grid",
};

#define TEXTURE_COUNT (sizeof(TEXTURE_PATHS) / sizeof(TEXTURE_PATHS[0]))

// Model picker: every model the GUI combo box can switch between. Format is
// picked from the extension (.obj / .gltf / .glb) by model_load(). Add an
// entry here (and drop the file in assets/models/) to make a new model
// selectable -- nothing else needs to change. glTF models bring their own
// materials and textures; the .obj files here have none, so they're drawn
// with the tweakable default material.
static const char* MODEL_PATHS[] = {
    "models/Avocado.glb",
    "models/BoxTextured.glb",
    "models/suzanne.obj",
    "models/sphere.obj",
    "models/lord_krishna_statue_3d_model_free.glb",
    "models/bulky_knight.glb"
};

static const char* MODEL_NAMES[] = {
    "Avocado (glTF, PBR textures)",
    "Textured Box (glTF)",
    "Suzanne (OBJ)",
    "Sphere (OBJ)",
    "krishna (glTF)",
    "bulky_knight (glTF)"
};

#define MODEL_COUNT (sizeof(MODEL_PATHS) / sizeof(MODEL_PATHS[0]))

// HDRI picker: equirectangular (2:1 lat/long) Radiance .hdr panoramas the GUI
// can bake into image-based lighting. Add an entry here (and drop the file in
// assets/hdri/) to make a new one selectable. Any 1k-2k HDRI works; Poly
// Haven (polyhaven.com/hdris, CC0) has plenty. A missing file is not an
// error -- the shader falls back to its built-in hemisphere ambient.
static const char* HDRI_PATHS[] = {
    "hdri/citrus_orchard_road_puresky_4k.hdr",
    "hdri/table_mountain_2_puresky_4k.hdr"
};

static const char* HDRI_NAMES[] = {
    "citrus_orchard_road_puresky_4k.hdr",
    "table_mountain_2_puresky_4k.hdr"
};

#define HDRI_COUNT (sizeof(HDRI_PATHS) / sizeof(HDRI_PATHS[0]))

// Resolution of *each* cascade's shadow map (Renderer/Shadow.h). Depth-only
// framebuffers are cheap, so this and CASCADE_COUNT are both sized
// generously; CASCADE_COUNT tops out at CSM_MAX_CASCADES.
#define SHADOW_MAP_SIZE 2048
#define CASCADE_COUNT   4

// Material grid (RENDER_MODE_MATERIAL_GRID) layout.
#define GRID_COLUMNS        7       // roughness: 0.05 (left) .. 1.0 (right)
#define GRID_ROWS           5       // metallic:  1.0 (top)   .. 0.0 (bottom)
#define GRID_SPACING        0.62f   // centre to centre, in world units
#define GRID_SPHERE_RADIUS  0.27f

// The grid sits right of the origin because the "Scene" panel covers the left
// third of the default 800x600 window, and the leftmost column (the mirror-
// smooth spheres) is the one worth seeing.
#define GRID_CENTER_X       1.5f

// The three demo lights, created identically in both lit worlds so the
// same GUI sliders drive both (see sync_demo_lights()).
typedef struct demo_lights
{
    entity_id key;    // directional, shadowed: the first Light entity in its world
    entity_id fill;   // point
    entity_id spot;   // spot, aimed at the origin
} demo_lights;

// Which of the two demo scenes on_frame draws this frame -- both share the
// same camera/orbit controls, only the geometry/shader/GUI panel differ.
typedef enum render_mode
{
    RENDER_MODE_TEXTURED_CUBE,
    RENDER_MODE_LIT_MODEL,
    RENDER_MODE_MATERIAL_GRID   // IBL demo: a metallic x roughness sphere grid, see create_grid_world()
} render_mode;

typedef struct demo_state
{
    render_mode mode;
    rhi_buffer  vertex_buffer;
    rhi_buffer  index_buffer;
    rhi_shader  shader;
    rhi_texture textures[TEXTURE_COUNT];
    int         selected_texture;   // index into textures[]/TEXTURE_NAMES, driven by the GUI combo box
    camera      cam;

    // --- Model viewer (RENDER_MODE_LIT_MODEL) ----------------------------
    // A loaded model on a ground plane under three lights, drawn through the
    // PBR shader. Everything below is exposed through ImGui so the shading
    // math is easy to see change live; the GUI edits these plain fields and
    // sync_lit_world() / sync_grid_world() push them into the Worlds below
    // every frame.
    int        selected_model;     // index into MODEL_PATHS/MODEL_NAMES, driven by the GUI combo box
    int        loaded_model;       // which entry was last requested (-1 = not yet); the load may have failed
    b8         spin;
    vec3       light_dir;          // direction the light travels toward the surface; normalized before use
    vec3       light_color;
    f32        light_intensity;
    f32        ambient_strength;
    f32        exposure;

    // --- Worlds (World/World.h) ----------------------------------------------
    // Both lit modes are Worlds sharing one asset cache:
    //   lit_world:  model_pivot (spins about Y) -> model (fit to ~2 units),
    //               ground (built-in plane), key/fill/spot lights
    //   grid_world: one sphere entity per grid cell, each with its own
    //               metallic/roughness override, plus the same three lights
    asset_cache assets;
    world       lit_world;
    world       grid_world;
    entity_id   model_pivot;
    entity_id   model_entity;
    entity_id   ground_entity;
    demo_lights lit_lights;
    demo_lights grid_lights;
    entity_id   grid_spheres[GRID_ROWS * GRID_COLUMNS];

    // --- Fill light (point) -------------------------------------------------
    // Position/color/intensity/range are all live-editable via ImGui, same as
    // the key (directional) light above. "Disabled" zeroes its intensity, so
    // the entity (and the GUI's values) survive toggling. Casts no shadow.
    b8    fill_light_enabled;
    vec3  fill_light_pos;
    vec3  fill_light_color;
    f32   fill_light_intensity;
    f32   fill_light_range;

    // --- Spot light ------------------------------------------------------------
    // Off by default: a warm cone always aimed at the origin (where the model
    // is centred), so moving it never loses the target. Exercises pbr.frag's
    // LIGHT_SPOT path. Casts no shadow.
    b8    spot_light_enabled;
    vec3  spot_light_pos;
    vec3  spot_light_color;
    f32   spot_light_intensity;
    f32   spot_light_range;
    f32   spot_light_inner_degrees;
    f32   spot_light_outer_degrees;

    // --- Lit rendering ---------------------------------------------------------
    // Renderer/SceneRenderer.h runs the whole lit frame (cascaded shadows,
    // HDR target, skybox, PBR uniforms, every MeshRenderer, tone mapping) for
    // both lit modes; build_render_settings() turns the GUI state below into
    // its settings. The textured cube is plain LDR and draws straight to the
    // screen.
    scene_renderer renderer;
    b8             debug_show_cascades;   // tints shading by cascade index; see pbr.frag's u_debug_show_cascades

    // --- Material grid (RENDER_MODE_MATERIAL_GRID) ----------------------------
    // The classic IBL test pattern: the same sphere 7 x 5 times, roughness
    // sweeping left to right and metallic top to bottom.
    vec3 grid_albedo;   // sRGB, like default_albedo; converted to linear when applied

    // --- Image-based lighting -----------------------------------------------
    // Baked from an HDRI (Renderer/IBL.h) and read by pbr.frag for ambient
    // diffuse + specular. All zeroes when no HDRI could be loaded.
    ibl_environment ibl;
    int             selected_hdri;   // index into HDRI_PATHS/HDRI_NAMES, driven by the GUI combo box
    int             loaded_hdri;     // which entry `ibl` was last baked from (-1 = not yet); the load may have failed
    b8              ibl_enabled;     // use `ibl` for ambient light (needs a loaded HDRI)
    b8              show_skybox;     // draw the HDRI as the background
    f32             sky_blur;        // 0..1, how blurry the background is

    b8 show_ground;

    // Overrides for meshes the file gave no material (a bare .obj). Kept
    // here rather than on the model so they survive switching models.
    vec3       default_albedo;
    f32        default_metallic;
    f32        default_roughness;

    // Mouse-orbit camera control. The camera itself stays dumb (position +
    // target, per Camera.h) -- yaw/pitch/distance live here, in the layer
    // responsible for turning input into camera state, and position is
    // recomputed from them every frame.
    f32 orbit_yaw;
    f32 orbit_pitch;
    f32 orbit_distance;
    f64 last_cursor_x;
    f64 last_cursor_y;
    b8  dragging;
} demo_state;

// Cube: position (vec3) + uv (vec2) per vertex. Each face still gets its
// own 4 vertices (rather than sharing the 8 cube corners) so every face
// can have its own 0..1 UV range — sharing corners would smear a single
// UV attribute across faces that should each tile the texture the same
// way, since uv is per-vertex, not per-face.
static const f32 CUBE_VERTICES[] = {
    // Front face (+z)
    -0.5f, -0.5f,  0.5f,  0.0f, 0.0f,
     0.5f, -0.5f,  0.5f,  1.0f, 0.0f,
     0.5f,  0.5f,  0.5f,  1.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,  0.0f, 1.0f,

    // Back face (-z)
    -0.5f, -0.5f, -0.5f,  0.0f, 0.0f,
     0.5f, -0.5f, -0.5f,  1.0f, 0.0f,
     0.5f,  0.5f, -0.5f,  1.0f, 1.0f,
    -0.5f,  0.5f, -0.5f,  0.0f, 1.0f,

    // Left face (-x)
    -0.5f, -0.5f, -0.5f,  0.0f, 0.0f,
    -0.5f, -0.5f,  0.5f,  1.0f, 0.0f,
    -0.5f,  0.5f,  0.5f,  1.0f, 1.0f,
    -0.5f,  0.5f, -0.5f,  0.0f, 1.0f,

    // Right face (+x)
     0.5f, -0.5f, -0.5f,  0.0f, 0.0f,
     0.5f, -0.5f,  0.5f,  1.0f, 0.0f,
     0.5f,  0.5f,  0.5f,  1.0f, 1.0f,
     0.5f,  0.5f, -0.5f,  0.0f, 1.0f,

    // Top face (+y)
    -0.5f,  0.5f,  0.5f,  0.0f, 0.0f,
     0.5f,  0.5f,  0.5f,  1.0f, 0.0f,
     0.5f,  0.5f, -0.5f,  1.0f, 1.0f,
    -0.5f,  0.5f, -0.5f,  0.0f, 1.0f,

    // Bottom face (-y)
    -0.5f, -0.5f,  0.5f,  0.0f, 0.0f,
     0.5f, -0.5f,  0.5f,  1.0f, 0.0f,
     0.5f, -0.5f, -0.5f,  1.0f, 1.0f,
    -0.5f, -0.5f, -0.5f,  0.0f, 1.0f,
};

static const uint32 CUBE_INDICES[] = {
     0,  1,  2,   2,  3,  0,   // front
     4,  6,  5,   6,  4,  7,   // back
     8,  9, 10,  10, 11,  8,   // left
    12, 14, 13,  14, 12, 15,   // right
    16, 17, 18,  18, 19, 16,   // top
    20, 22, 21,  22, 20, 23,   // bottom
};

static inline f32
clampf(f32 v, f32 lo, f32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Recomputes cam->position from state's yaw/pitch/distance, orbiting around
// whatever cam->target currently is. Call after any yaw/pitch/distance
// change; cam->target itself is untouched so this only ever orbits, never
// pans.
static void
orbit_camera_apply(demo_state* state)
{
    f32 yaw_rad   = glm_rad(state->orbit_yaw);
    f32 pitch_rad = glm_rad(state->orbit_pitch);

    f32 x = state->orbit_distance * cosf(pitch_rad) * sinf(yaw_rad);
    f32 y = state->orbit_distance * sinf(pitch_rad);
    f32 z = state->orbit_distance * cosf(pitch_rad) * cosf(yaw_rad);

    state->cam.position[0] = state->cam.target[0] + x;
    state->cam.position[1] = state->cam.target[1] + y;
    state->cam.position[2] = state->cam.target[2] + z;
}

// Polls the left mouse button + cursor delta and turns it into orbit_yaw /
// orbit_pitch. Polling (rather than a glfwSetCursorPosCallback) keeps this
// self-contained in the demo layer. Goes through the window_* input
// wrappers rather than GLFW directly.
static void
orbit_camera_update(demo_state* state, window* wind)
{
    f64 cursor_x, cursor_y;
    window_get_cursor_pos(wind, &cursor_x, &cursor_y);

    b8 lmb_down        = window_is_mouse_button_down(wind, GLFW_MOUSE_BUTTON_LEFT);
    b8 imgui_has_mouse = imgui_layer_wants_mouse();

    if (lmb_down && !imgui_has_mouse)
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

// Mouse-wheel zoom: shrinks/grows orbit_distance, clamped so you can't
// zoom through the cube or scroll off into the distance forever. App.c only
// calls this for scrolls ImGui didn't claim.
static void
on_scroll(App* app, f64 dx, f64 dy)
{
    (void)dx;

    demo_state* state = (demo_state*)app->user_data;

    const f32 zoom_speed = 0.5f;

    state->orbit_distance -= (f32)dy * zoom_speed;
    state->orbit_distance  = clampf(state->orbit_distance, 1.0f, 50.0f);

    orbit_camera_apply(state);
}

// sRGB color-picker value -> linear, as material colors are stored.
static void
srgb_to_linear(const vec3 srgb, f32 out[3])
{
    out[0] = powf(srgb[0], 2.2f);
    out[1] = powf(srgb[1], 2.2f);
    out[2] = powf(srgb[2], 2.2f);
}

// Adds the key/fill/spot light entities to `w` from the GUI's current
// values. Called once per world; sync_demo_lights() keeps them current.
static void
create_demo_lights(demo_state* state, world* w, demo_lights* out)
{
    light key, fill, spot;
    light_init_directional(&key, state->light_dir, state->light_color, state->light_intensity);
    light_init_point(&fill, state->fill_light_color, state->fill_light_intensity, state->fill_light_range);

    // Direction is re-aimed every frame by sync_demo_lights(); any non-zero
    // vector does here.
    light_init_spot(&spot, (vec3){ 0.0f, -1.0f, 0.0f }, state->spot_light_color, state->spot_light_intensity,
                    state->spot_light_range, state->spot_light_inner_degrees, state->spot_light_outer_degrees);

    // Key first: it's the first directional light, so it's the one shadowed.
    out->key  = world_create(w, "key_light");
    out->fill = world_create(w, "fill_light");
    out->spot = world_create(w, "spot_light");

    world_add_light(w, out->key, key);
    world_add_light(w, out->fill, fill);
    world_add_light(w, out->spot, spot);
}

// Pushes this frame's GUI light values into one world's light entities.
// They're unparented, so their local transform/direction is world space.
static void
sync_demo_lights(demo_state* state, world* w, const demo_lights* lights)
{
    c_light* key = world_light(w, lights->key);
    glm_vec3_normalize_to(state->light_dir, key->direction);
    glm_vec3_copy(state->light_color, key->color);
    key->intensity = state->light_intensity;

    glm_vec3_copy(state->fill_light_pos, world_transform(w, lights->fill)->position);
    c_light* fill = world_light(w, lights->fill);
    glm_vec3_copy(state->fill_light_color, fill->color);
    fill->intensity = state->fill_light_enabled ? state->fill_light_intensity : 0.0f;
    fill->range     = state->fill_light_range;

    glm_vec3_copy(state->spot_light_pos, world_transform(w, lights->spot)->position);
    c_light* spot = world_light(w, lights->spot);

    // Aim at the origin.
    vec3 to_origin;
    glm_vec3_negate_to(state->spot_light_pos, to_origin);
    if (glm_vec3_norm2(to_origin) > 1e-8f)
        glm_vec3_normalize_to(to_origin, spot->direction);

    // The outer cone can't be narrower than the inner one.
    if (state->spot_light_outer_degrees < state->spot_light_inner_degrees)
        state->spot_light_outer_degrees = state->spot_light_inner_degrees;

    glm_vec3_copy(state->spot_light_color, spot->color);
    spot->intensity          = state->spot_light_enabled ? state->spot_light_intensity : 0.0f;
    spot->range              = state->spot_light_range;
    spot->inner_cone_degrees = state->spot_light_inner_degrees;
    spot->outer_cone_degrees = state->spot_light_outer_degrees;
}

// Builds lit_world: the (still empty) model under a spinning pivot, the
// ground plane and the lights. load_selected_model() fills the model in.
static void
create_lit_world(demo_state* state)
{
    world* w = &state->lit_world;

    state->model_pivot  = world_create(w, "model_pivot");
    state->model_entity = world_create(w, "model");
    world_set_parent(w, state->model_entity, state->model_pivot);
    world_add_mesh_renderer(w, state->model_entity, ASSET_NONE);

    // An 8 x 8 plane the model's shadow lands on; its height follows the
    // loaded model (load_selected_model()).
    state->ground_entity = world_create(w, "ground");
    glm_vec3_copy((vec3){ 8.0f, 1.0f, 8.0f }, world_transform(w, state->ground_entity)->scale);

    c_mesh_renderer* ground = world_add_mesh_renderer(w, state->ground_entity,
                                                      assets_builtin(&state->assets, BUILTIN_PLANE));
    ground->cast_shadows = false;   // flat and facing up: there's nothing above it for it to shadow

    // Dim, non-metal concrete-ish grey: dark enough that the shadow reads
    // clearly against it without crushing to black.
    ground->material.fields = MATERIAL_OVERRIDE_BASE_COLOR | MATERIAL_OVERRIDE_METALLIC | MATERIAL_OVERRIDE_ROUGHNESS;
    ground->material.base_color[0] = ground->material.base_color[1] = ground->material.base_color[2] = 0.18f;
    ground->material.metallic  = 0.0f;
    ground->material.roughness = 0.9f;

    create_demo_lights(state, w, &state->lit_lights);
}

// Builds grid_world: one built-in sphere per (roughness, metallic) cell.
static void
create_grid_world(demo_state* state)
{
    world*   w      = &state->grid_world;
    asset_id sphere = assets_builtin(&state->assets, BUILTIN_SPHERE);

    for (int row = 0; row < GRID_ROWS; ++row)
    {
        for (int col = 0; col < GRID_COLUMNS; ++col)
        {
            char name[32];
            snprintf(name, sizeof(name), "sphere_r%d_c%d", row, col);

            entity_id e = world_create(w, name);
            state->grid_spheres[row * GRID_COLUMNS + col] = e;

            c_transform* t = world_transform(w, e);
            t->position[0] = GRID_CENTER_X + ((f32)col - 0.5f * (GRID_COLUMNS - 1)) * GRID_SPACING;
            t->position[1] = (0.5f * (GRID_ROWS - 1) - (f32)row) * GRID_SPACING;
            t->position[2] = 0.0f;
            glm_vec3_fill(t->scale, 2.0f * GRID_SPHERE_RADIUS);   // the built-in sphere is 1 unit across

            c_mesh_renderer* mr = world_add_mesh_renderer(w, e, sphere);
            mr->material.fields    = MATERIAL_OVERRIDE_BASE_COLOR | MATERIAL_OVERRIDE_METALLIC | MATERIAL_OVERRIDE_ROUGHNESS;
            mr->material.metallic  = 1.0f - (f32)row / (f32)(GRID_ROWS - 1);
            mr->material.roughness = 0.05f + 0.95f * (f32)col / (f32)(GRID_COLUMNS - 1);
        }
    }

    create_demo_lights(state, w, &state->grid_lights);
}

// Points lit_world's model entity at MODEL_PATHS[index], fits it into a ~2
// unit box at the origin, and sits the ground under it.
static void
load_selected_model(demo_state* state, int index)
{
    world* w = &state->lit_world;

    // Recorded even when the load fails, so on_frame doesn't retry (and
    // re-log the error) every single frame.
    state->loaded_model = index;

    asset_id id = assets_load_model(&state->assets, MODEL_PATHS[index]);
    world_set_model(w, state->model_entity, id);

    // Free the model this one replaced, rather than keeping every model
    // ever picked resident on the GPU.
    assets_collect_unused(&state->assets);

    const model* m = assets_model(&state->assets, id);
    if (!m)
        return;   // assets_load_model() logged why; the model entity just draws nothing

    // Assets come in wildly different units; fit everything into a ~2 unit
    // box at the origin so the orbit camera framing works for all of them.
    // The fit is a uniform scale plus a translation, so it maps straight
    // onto the entity's transform.
    mat4 fit;
    model_get_fit_transform(m, 2.0f, fit);

    c_transform* t = world_transform(w, state->model_entity);
    glm_vec3_fill(t->scale, fit[0][0]);
    glm_vec3_copy(fit[3], t->position);

    // Sit the ground at the fitted model's lowest point. The pivot only
    // spins about Y, which doesn't change Y, so this holds at every angle.
    vec3 fitted_min;
    glm_mat4_mulv3(fit, (f32*)m->bounds_min, 1.0f, fitted_min);
    world_transform(w, state->ground_entity)->position[1] = fitted_min[1];
}

// Bakes HDRI_PATHS[index] into state->ibl, replacing whatever was there. A
// missing/unreadable file leaves image-based lighting off, not the app dead.
static void
load_selected_hdri(demo_state* state, int index)
{
    ibl_destroy(&state->ibl);
    state->loaded_hdri = index;

    char* path = asset_path(HDRI_PATHS[index]);
    state->ibl_enabled = ibl_create(&state->ibl, path);

    if (!state->ibl_enabled)
        LOG_WARN("No environment loaded from '%s' -- using the hemisphere ambient light\n", path);

    free(path);
}

// Runs once from App.c's run(), after the GL context / RHI / ImGui exist.
static void
on_init(App* app)
{
    demo_state* state = (demo_state*)app->user_data;
    window_size framebuffer_size = window_get_size(get_renderer()->drawing_window);

    rhi_vertex_attribute attrs[] = {
        { .location = 0, .component_count = 3, .offset = 0 },
        { .location = 1, .component_count = 2, .offset = 3 * sizeof(f32) },
    };

    rhi_vertex_layout layout = {
        .attributes = attrs,
        .attribute_count = 2,
        .stride = 5 * sizeof(f32)
    };

    state->vertex_buffer = rhi_vertex_buffer_create(CUBE_VERTICES, sizeof(CUBE_VERTICES), &layout, RHI_USAGE_STATIC);
    state->index_buffer  = rhi_index_buffer_create(CUBE_INDICES, sizeof(CUBE_INDICES), RHI_USAGE_STATIC);

    char* vertex_path   = asset_path("shaders/textured.vert");
    char* fragment_path = asset_path("shaders/textured.frag");

    state->shader = rhi_shader_create_from_files(vertex_path, fragment_path);

    free(vertex_path);
    free(fragment_path);

    if (!state->shader)
        FATAL("demo: failed to load textured shader");

    for (uint32 i = 0; i < TEXTURE_COUNT; ++i)
    {
        char* texture_path = asset_path(TEXTURE_PATHS[i]);

        state->textures[i] = rhi_texture_create_from_file(texture_path,
                                                            RHI_FILTER_LINEAR,
                                                            RHI_WRAP_REPEAT,
                                                            /* generate_mipmaps */ true);

        free(texture_path);

        if (!state->textures[i])
            FATAL("demo: failed to load texture '%s'", TEXTURE_PATHS[i]);
    }

    state->selected_texture = 0;

    // Sampler binding is fixed at texture unit 0 for the lifetime of the
    // shader, so this only needs setting once here rather than every
    // frame — only the actual texture bound to unit 0 changes per-draw.
    rhi_shader_set_int(state->shader, "u_texture", 0);

    // --- Model viewer setup ---------------------------------------------
    if (!scene_renderer_create(&state->renderer, framebuffer_size.width, framebuffer_size.height,
                               CASCADE_COUNT, SHADOW_MAP_SIZE))
        FATAL("demo: failed to create the scene renderer");

    state->grid_albedo[0] = state->grid_albedo[1] = state->grid_albedo[2] = 0.9f;

    state->show_skybox   = true;
    state->sky_blur      = 0.0f;
    state->selected_hdri = 0;
    state->loaded_hdri   = -1;
    load_selected_hdri(state, state->selected_hdri);

    state->show_ground = true;

    state->mode = RENDER_MODE_LIT_MODEL;

    glm_vec3_copy((vec3){ 0.9f, 0.3f, 0.25f }, state->default_albedo);
    state->default_metallic  = 0.0f;
    state->default_roughness = 0.45f;

    glm_vec3_copy((vec3){ -0.4f, -0.6f, -0.5f }, state->light_dir);
    glm_vec3_copy((vec3){ 1.0f, 0.98f, 0.92f }, state->light_color);
    state->light_intensity  = 2.5f;
    state->ambient_strength = 0.5f;
    state->exposure         = 1.0f;

    state->fill_light_enabled   = true;
    glm_vec3_copy((vec3){ 1.5f, 1.0f, 1.5f }, state->fill_light_pos);
    glm_vec3_copy((vec3){ 0.55f, 0.7f, 1.0f }, state->fill_light_color);
    state->fill_light_intensity = 3.0f;
    state->fill_light_range     = 8.0f;

    state->spot_light_enabled       = false;
    glm_vec3_copy((vec3){ -1.5f, 2.5f, 1.0f }, state->spot_light_pos);
    glm_vec3_copy((vec3){ 1.0f, 0.75f, 0.45f }, state->spot_light_color);
    state->spot_light_intensity     = 15.0f;
    state->spot_light_range         = 10.0f;
    state->spot_light_inner_degrees = 12.0f;
    state->spot_light_outer_degrees = 22.0f;

    // --- Worlds -------------------------------------------------------------
    assets_init(&state->assets);
    world_init(&state->lit_world, &state->assets);
    world_init(&state->grid_world, &state->assets);

    create_lit_world(state);
    create_grid_world(state);

    state->selected_model = 0;
    state->loaded_model   = -1;
    load_selected_model(state, state->selected_model);

    f32 aspect = (f32)framebuffer_size.width / (f32)framebuffer_size.height;

    vec3 initial_position = { 2.5f, 2.0f, 3.5f };
    vec3 initial_target   = { 0.0f, 0.0f, 0.0f };

    camera_init(&state->cam,
                initial_position,
                initial_target,               // target: look at the origin
                45.0f,                        // vertical FOV
                aspect,
                0.1f, 100.0f);

    // Derive yaw/pitch/distance from the position above so mouse-orbit
    // continues smoothly from wherever the camera already was, instead of
    // snapping to some default angle on the first drag.
    vec3 offset;
    glm_vec3_sub(initial_position, initial_target, offset);

    state->orbit_distance = glm_vec3_norm(offset);
    state->orbit_yaw       = glm_deg(atan2f(offset[0], offset[2]));
    state->orbit_pitch     = glm_deg(asinf(offset[1] / state->orbit_distance));
    state->dragging        = false;
}

// The GL viewport is already updated by App.c before this runs; the
// projection matrix needs the same treatment or the cube will stretch.
static void on_resize(App* app, uint16 width, uint16 height)
{
    demo_state* state = (demo_state*)app->user_data;

    // 0x0 while minimized -- would make the aspect NaN/inf.
    if (width == 0 || height == 0)
        return;

    camera_set_aspect(&state->cam, (f32)width / (f32)height);
    scene_renderer_resize(&state->renderer, width, height);
}

static void on_key(App* app, int key, int action, int mods)
{
    (void)mods;

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        app->should_close = true;
}

// Light + image-based-lighting controls, shared by the lit-model and material
// grid panels.
static void
draw_lighting_controls(demo_state* state)
{
    igText("Light");
    igColorEdit3("Light Color", state->light_color, 0);
    igSliderFloat3("Light Direction", state->light_dir, -1.0f, 1.0f, "%.2f", 0);
    igSliderFloat("Light Intensity", &state->light_intensity, 0.0f, 10.0f, "%.2f", 0);
    igSeparator();

    igText("Fill light (point)");
    igCheckbox("Enabled##fill", &state->fill_light_enabled);
    igColorEdit3("Color##fill", state->fill_light_color, 0);
    igSliderFloat3("Position##fill", state->fill_light_pos, -5.0f, 5.0f, "%.2f", 0);
    igSliderFloat("Intensity##fill", &state->fill_light_intensity, 0.0f, 20.0f, "%.2f", 0);
    igSliderFloat("Range##fill", &state->fill_light_range, 0.0f, 20.0f, "%.2f", 0);
    igSeparator();

    igText("Spot light (aimed at the origin)");
    igCheckbox("Enabled##spot", &state->spot_light_enabled);
    igColorEdit3("Color##spot", state->spot_light_color, 0);
    igSliderFloat3("Position##spot", state->spot_light_pos, -5.0f, 5.0f, "%.2f", 0);
    igSliderFloat("Intensity##spot", &state->spot_light_intensity, 0.0f, 50.0f, "%.2f", 0);
    igSliderFloat("Range##spot", &state->spot_light_range, 0.0f, 20.0f, "%.2f", 0);
    igSliderFloat("Inner angle##spot", &state->spot_light_inner_degrees, 0.0f, 89.0f, "%.1f deg", 0);
    igSliderFloat("Outer angle##spot", &state->spot_light_outer_degrees, 0.0f, 89.0f, "%.1f deg", 0);
    igSeparator();

    igSliderFloat("Ambient", &state->ambient_strength, 0.0f, 2.0f, "%.2f", 0);
    igSliderFloat("Exposure", &state->exposure, 0.1f, 4.0f, "%.2f", 0);
    igSeparator();

    igText("Image-based lighting");
    igCombo_Str_arr("HDRI", &state->selected_hdri, HDRI_NAMES, (int)HDRI_COUNT, -1);

    if (state->ibl.irradiance)
    {
        igCheckbox("Use IBL", &state->ibl_enabled);
        igCheckbox("Show skybox", &state->show_skybox);
        igSliderFloat("Sky blur", &state->sky_blur, 0.0f, 1.0f, "%.2f", 0);
    }
    else
    {
        igText("No HDRI loaded. Expected:");
        igText("assets/%s", HDRI_PATHS[state->selected_hdri]);
    }
    igSeparator();
}

// Pushes this frame's GUI state into lit_world and updates its transforms.
static void
sync_lit_world(demo_state* state, f32 time)
{
    world* w = &state->lit_world;

    // Spin about the model's own middle: the model is centred under the pivot.
    world_transform(w, state->model_pivot)->rotation_deg[1] = state->spin ? glm_deg(time * 0.5f) : 0.0f;

    // The GUI's default material only applies to models that brought no
    // materials of their own (a bare .obj); glTF models keep theirs.
    c_mesh_renderer* mr = world_mesh_renderer(w, state->model_entity);
    const model*     m  = assets_model(&state->assets, mr->model);
    if (m && m->material_count == 0)
    {
        mr->material.fields = MATERIAL_OVERRIDE_BASE_COLOR | MATERIAL_OVERRIDE_METALLIC | MATERIAL_OVERRIDE_ROUGHNESS;
        srgb_to_linear(state->default_albedo, mr->material.base_color);
        mr->material.metallic  = state->default_metallic;
        mr->material.roughness = state->default_roughness;
    }
    else
    {
        mr->material.fields = 0;
    }

    world_mesh_renderer(w, state->ground_entity)->visible = state->show_ground;

    sync_demo_lights(state, w, &state->lit_lights);
    world_update(w);
}

// Pushes this frame's GUI state into grid_world and updates its transforms.
static void
sync_grid_world(demo_state* state)
{
    world* w = &state->grid_world;

    f32 albedo[3];
    srgb_to_linear(state->grid_albedo, albedo);

    for (int i = 0; i < GRID_ROWS * GRID_COLUMNS; ++i)
    {
        c_mesh_renderer* mr = world_mesh_renderer(w, state->grid_spheres[i]);
        mr->material.base_color[0] = albedo[0];
        mr->material.base_color[1] = albedo[1];
        mr->material.base_color[2] = albedo[2];
    }

    sync_demo_lights(state, w, &state->grid_lights);
    world_update(w);
}

// Square the camera up to the grid -- the default viewpoint is a corner view
// meant for a single model, which foreshortens a flat 7 x 5 layout badly.
static void
frame_material_grid(demo_state* state)
{
    state->orbit_yaw      = 0.0f;
    state->orbit_pitch    = 0.0f;
    state->orbit_distance = 7.6f;
    orbit_camera_apply(state);
}


// This frame's SceneRenderer settings from the GUI state.
static scene_render_settings
build_render_settings(const demo_state* state)
{
    scene_render_settings settings = scene_render_settings_default();

    settings.ibl                 = state->ibl_enabled ? &state->ibl : NULL;
    settings.show_skybox         = state->show_skybox;
    settings.sky_blur            = state->sky_blur;
    settings.ambient_strength    = state->ambient_strength;
    settings.exposure            = state->exposure;
    settings.debug_show_cascades = state->debug_show_cascades;

    // Where nothing (not even the skybox) is drawn: App.c's screen clear
    // color, (0.4, 0.1, 0.12) in sRGB, converted to linear so it comes out
    // of tone mapping looking about the same.
    glm_vec3_copy((vec3){ 0.133f, 0.0063f, 0.0094f }, settings.clear_color);

    return settings;
}

static void on_frame(App* app, f32 dt)
{
    (void)dt;

    demo_state* state = (demo_state*)app->user_data;
    window*     wind  = get_renderer()->drawing_window;

    // Left-drag to orbit the camera around the cube. Must run after
    // imgui_layer_new_frame() (WantCaptureMouse needs this frame's ImGui
    // state) which App.c already guarantees by calling it before on_frame.
    orbit_camera_update(state, wind);

    // Picks up edits to pbr.vert/pbr.frag saved while the app runs.
    scene_renderer_poll_hot_reload(&state->renderer);

    f32 time = (f32)window_get_time();

    if (state->mode == RENDER_MODE_TEXTURED_CUBE)
    {
        mat4 view_projection;
        camera_get_view_projection(&state->cam, view_projection);

        mat4 model;
        glm_mat4_identity(model);
        glm_rotate(model, time, (vec3){ 0.3f, 1.0f, 0.2f });

        mat4 mvp;
        glm_mat4_mul(view_projection, model, mvp);

        rhi_shader_set_mat4(state->shader, "u_mvp", (const f32*)mvp);

        rhi_shader_bind(state->shader);
        rhi_texture_bind(state->textures[state->selected_texture], 0);
        rhi_draw_indexed(state->vertex_buffer, state->index_buffer,
                          sizeof(CUBE_INDICES) / sizeof(CUBE_INDICES[0]));
    }
    else if (state->mode == RENDER_MODE_MATERIAL_GRID)
    {
        if (state->selected_hdri != state->loaded_hdri)
            load_selected_hdri(state, state->selected_hdri);

        sync_grid_world(state);

        // The demo is about ambient light, so no shadow pass.
        scene_render_settings settings = build_render_settings(state);
        settings.shadows = false;

        scene_renderer_draw(&state->renderer, &state->grid_world, &state->cam, &settings);
    }
    else // RENDER_MODE_LIT_MODEL
    {
        // A different model was picked in the GUI last frame.
        if (state->selected_model != state->loaded_model)
            load_selected_model(state, state->selected_model);

        if (state->selected_hdri != state->loaded_hdri)
            load_selected_hdri(state, state->selected_hdri);

        sync_lit_world(state, time);

        scene_render_settings settings = build_render_settings(state);
        scene_renderer_draw(&state->renderer, &state->lit_world, &state->cam, &settings);
    }

    // App/on_frame runs between imgui_layer_new_frame() and
    // imgui_layer_render() (see App.c), so any ig*() calls here just work.
    // Fullscreen dockspace so ImGui windows can actually be dragged and
    // docked against something. dockspace_id=0 auto-generates an ID; NULL
    // viewport = the main viewport; no flags, no window-class restriction.
    igDockSpaceOverViewport(0, NULL, ImGuiDockNodeFlags_PassthruCentralNode, NULL);

    igBegin("Scene", NULL, 0);

    igText("Render Mode");
    igRadioButton_IntPtr("Textured Cube", (int*)&state->mode, RENDER_MODE_TEXTURED_CUBE);
    igSameLine(0.0f, -1.0f);
    igRadioButton_IntPtr("Lit Model", (int*)&state->mode, RENDER_MODE_LIT_MODEL);
    igSameLine(0.0f, -1.0f);
    if (igRadioButton_IntPtr("Grid", (int*)&state->mode, RENDER_MODE_MATERIAL_GRID))
        frame_material_grid(state);
    igSeparator();

    if (state->mode == RENDER_MODE_TEXTURED_CUBE)
    {
        // Texture picker: a combo box bound directly to selected_texture,
        // which is what the draw call above reads every frame -- no extra
        // plumbing needed between "user picks an entry" and "the cube
        // shows it".
        igCombo_Str_arr("Texture", &state->selected_texture, TEXTURE_NAMES, (int)TEXTURE_COUNT, -1);
    }
    else if (state->mode == RENDER_MODE_MATERIAL_GRID)
    {
        igTextWrapped("Columns: roughness 0.05 to 1.0, left to right.");
        igTextWrapped("Rows: metallic 1.0 to 0.0, top to bottom.");
        igColorEdit3("Albedo", state->grid_albedo, 0);
        igSeparator();

        draw_lighting_controls(state);
    }
    else // RENDER_MODE_LIT_MODEL
    {
        igCombo_Str_arr("Model", &state->selected_model, MODEL_NAMES, (int)MODEL_COUNT, -1);

        const model* m = assets_model(&state->assets,
                                      world_mesh_renderer(&state->lit_world, state->model_entity)->model);
        if (m)
        {

            igText("%u submeshes, %u materials, %u textures", m->submesh_count, m->material_count, m->texture_count);
            igText("%u vertices, %u triangles", m->vertex_count, m->triangle_count);
        }
        else
        {
            igText("Model failed to load (see console)");
        }

        igCheckbox("Spin", &state->spin);
        igCheckbox("Ground plane", &state->show_ground);
        igCheckbox("Debug: show shadow cascades", &state->debug_show_cascades);
        igSeparator();

        draw_lighting_controls(state);

        igText("Default material (models without one)");
        igColorEdit3("Albedo", state->default_albedo, 0);
        igSliderFloat("Metallic", &state->default_metallic, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Roughness", &state->default_roughness, 0.0f, 1.0f, "%.2f", 0);
    }

    igEnd();

    // Every entity and component of the world on screen (read-only).
    if (state->mode == RENDER_MODE_LIT_MODEL)
        entity_panel_draw(&state->lit_world, "Entities");
    else if (state->mode == RENDER_MODE_MATERIAL_GRID)
        entity_panel_draw(&state->grid_world, "Entities");
}

// Runs from terminate(), before the GL context goes away.
static void on_shutdown(App* app)
{
    demo_state* state = (demo_state*)app->user_data;

    for (uint32 i = 0; i < TEXTURE_COUNT; ++i)
        rhi_texture_destroy(state->textures[i]);

    world_shutdown(&state->lit_world);    // releases the worlds' assets...
    world_shutdown(&state->grid_world);
    assets_shutdown(&state->assets);      // ...then the models themselves
    ibl_destroy(&state->ibl);
    scene_renderer_destroy(&state->renderer);

    rhi_shader_destroy(state->shader);
    rhi_buffer_destroy(state->index_buffer);
    rhi_buffer_destroy(state->vertex_buffer);
}

int main(void)
{
    // GPU resources (vertex/index buffers, shader) can only be created
    // after RHI is initialized, which happens inside run() -- on_init
    // creates them, on_shutdown destroys them.
    demo_state state = { 0 };

    App app = {
        .name = "PBR Renderer - Cube Demo",
        .on_init = on_init,
        .on_frame = on_frame,
        .on_shutdown = on_shutdown,
        .on_key = on_key,
        .on_scroll = on_scroll,
        .on_resize = on_resize,
        .user_data = &state,
        .should_close = false
    };

    run(&app);
    terminate(&app);

    return 0;
}