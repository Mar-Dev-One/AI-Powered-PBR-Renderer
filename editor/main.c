// editor/main.c
//
// The editor: the demo scenes (textured cube, lit model viewer, IBL material
// grid) with their ImGui panels, the entity inspector, the command console,
// the command server other processes (MCP server, AI agent, scripts) drive
// it through, and the in-app chat. The game runtime -- the same engine with
// none of this -- is testbed/main.c.
//
// Live link: the world on screen is published to the live scene file
// (World/LiveScene.h) whenever it changes, however it changed -- panels,
// console, agent. F5 (or "Play in Testbed") starts the game with --live, and
// it follows every edit while it keeps running.

#include "App.h"
#include "Chat/ChatSession.h"
#include "Core/Defines.h"
#include "Core/Encoding.h"
#include "Core/FileIO.h"
#include "Core/Image.h"
#include "Core/Paths.h"
#include "Core/Process.h"
#include "RHI/RHI.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Camera.h"
#include "Scene/Model.h"
#include "Assets/AssetCache.h"
#include "World/LiveScene.h"
#include "World/World.h"
#include "Platform/Window.h"
#include "Net/CommandServer.h"
#include "UI/ChatPanel.h"
#include "UI/CommandConsole.h"
#include "UI/EntityPanel.h"
#include "UI/ImGuiLayer.h"
#include "cimgui.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    // PBR shader. The World is the single source of truth: the GUI's light,
    // ground and material controls edit World components directly, so they
    // always show what the console (and, later, an AI agent) last did.
    int        selected_model;     // index into MODEL_PATHS/MODEL_NAMES, driven by the GUI combo box
    int        loaded_model;       // which entry was last requested (-1 = not yet); the load may have failed

    // --- Worlds (World/World.h) ----------------------------------------------
    // Both lit modes are Worlds sharing one asset cache:
    //   lit_world:  model_pivot (spins about Y) -> model (fit to ~2 units),
    //               ground (built-in plane), key/fill/spot lights
    //   grid_world: one sphere entity per grid cell, each with its own
    //               metallic/roughness override, plus the same three lights
    // The model/ground IDs may go stale if a command destroys them; the code
    // checks before use and recreates the model entities when a model is
    // picked.
    asset_cache assets;
    world       lit_world;
    world       grid_world;
    entity_id   model_pivot;
    entity_id   model_entity;
    entity_id   ground_entity;
    entity_id   grid_spheres[GRID_ROWS * GRID_COLUMNS];

    // --- Commands (Commands/Commands.h) ----------------------------------------
    // One context + undo history per world; the console runs against the
    // world on screen. App-level commands (camera, environment, exposure)
    // come from HOST_COMMANDS below.
    command_history lit_history;
    command_history grid_history;
    command_context lit_commands;
    command_context grid_commands;
    command_console console;
    b8              show_console;   // F1 toggles

    // Each world's on_change: the console log plus a "scene.changed" push to
    // every server client, tagged with which world it was.
    struct world_listener
    {
        struct demo_state* state;
        const char*        world_name;
    } lit_listener, grid_listener;

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

    // --- Live link to the game (World/LiveScene.h) ------------------------------
    live_publisher live;
    process*       game_process;   // the Testbed started by F5; NULL if not running
    char           game_log[1024];
    char           game_note[256];
    char         agent_note[1280];
    char         agent_log[1024];

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
    vec3 grid_albedo;   // sRGB, like default_albedo; applied when changed

    // --- Environment + image-based lighting -----------------------------------
    // `env` (World/Environment.h) is the single source of truth for the HDRI,
    // IBL / skybox switches, exposure and ambient strength: the GUI, the
    // environment.set / render.set commands and scene.load all write it, and
    // scene.save stores it with the scene. `ibl` is env.hdri baked
    // (Renderer/IBL.h), all zeroes when it couldn't be loaded; sync_hdri()
    // re-bakes it whenever env.hdri changes.
    scene_environment env;
    ibl_environment   ibl;
    char              baked_hdri[ENVIRONMENT_HDRI_MAX];   // what `ibl` was last baked from; the bake may have failed
    b8                hdri_baked;                         // false until the first bake

    // Material for models the file gave none (a bare .obj), applied to the
    // model entity's MeshRenderer override when a model loads or these change.
    // Kept here rather than on the model so they survive switching models.
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

// Adds the demo's key/fill/spot lights to `w`. After this the World owns
// them: the GUI edits them in place (draw_light_editors()) and commands can
// change, add or remove them like any other entity.
static void
create_demo_lights(world* w)
{
    light key, fill, spot;
    light_init_directional(&key, (vec3){ -0.4f, -0.6f, -0.5f }, (vec3){ 1.0f, 0.98f, 0.92f }, 2.5f);
    light_init_point(&fill, (vec3){ 0.55f, 0.7f, 1.0f }, 3.0f, 8.0f);

    // A warm cone aimed at the origin, where the model sits. Intensity 0 =
    // off until someone turns it up.
    light_init_spot(&spot, (vec3){ 1.5f, -2.5f, -1.0f }, (vec3){ 1.0f, 0.75f, 0.45f }, 0.0f, 10.0f, 12.0f, 22.0f);

    // Key first: the first directional light is the one shadowed.
    entity_id key_e  = world_create(w, "key_light");
    entity_id fill_e = world_create(w, "fill_light");
    entity_id spot_e = world_create(w, "spot_light");

    world_add_light(w, key_e, key);
    world_add_light(w, fill_e, fill);
    world_add_light(w, spot_e, spot);

    glm_vec3_copy((vec3){ 1.5f, 1.0f, 1.5f }, world_transform(w, fill_e)->position);
    glm_vec3_copy((vec3){ -1.5f, 2.5f, 1.0f }, world_transform(w, spot_e)->position);
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

    create_demo_lights(w);
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
            srgb_to_linear(state->grid_albedo, mr->material.base_color);
            mr->material.metallic  = 1.0f - (f32)row / (f32)(GRID_ROWS - 1);
            mr->material.roughness = 0.05f + 0.95f * (f32)col / (f32)(GRID_COLUMNS - 1);
        }
    }

    create_demo_lights(w);
}

// The GUI's default material, on the model entity -- but only for models
// that brought no materials of their own (a bare .obj); glTF models keep
// theirs. Runs when a model loads and when the GUI values change.
static void
apply_default_material(demo_state* state)
{
    c_mesh_renderer* mr = world_mesh_renderer(&state->lit_world, state->model_entity);
    if (!mr)
        return;

    const model* m = assets_model(&state->assets, mr->model);
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
}

// Points lit_world's model entity at MODEL_PATHS[index], fits it into a ~2
// unit box at the origin, and sits the ground under it. Recreates the
// pivot/model entities if a command destroyed them.
static void
load_selected_model(demo_state* state, int index)
{
    world* w = &state->lit_world;

    // Recorded even when the load fails, so on_frame doesn't retry (and
    // re-log the error) every single frame.
    state->loaded_model = index;

    if (!world_is_alive(w, state->model_pivot))
        state->model_pivot = world_create(w, "model_pivot");
    if (!world_is_alive(w, state->model_entity))
    {
        state->model_entity = world_create(w, "model");
        world_add_mesh_renderer(w, state->model_entity, ASSET_NONE);
    }
    world_set_parent(w, state->model_entity, state->model_pivot);
    if (!world_has(w, state->model_entity, COMPONENT_MESH_RENDERER))
        world_add_mesh_renderer(w, state->model_entity, ASSET_NONE);

    asset_id id = assets_load_model(&state->assets, MODEL_PATHS[index]);
    world_set_model(w, state->model_entity, id);
    apply_default_material(state);

    // Free the model this one replaced, rather than keeping every model
    // ever picked resident on the GPU. (Undo history keeps the ones it
    // still references.)
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
    c_transform* ground = world_transform(w, state->ground_entity);
    if (ground)
        ground->position[1] = fitted_min[1];
    world_update(w);
}

// Bakes env.hdri into state->ibl if it changed since the last bake -- picked
// in the GUI, set by environment.set or brought in by scene.load. A
// missing/unreadable file leaves image-based lighting off, not the app dead.
static void
sync_hdri(demo_state* state)
{
    if (state->hdri_baked && strcmp(state->baked_hdri, state->env.hdri) == 0)
        return;

    ibl_destroy(&state->ibl);
    memcpy(state->baked_hdri, state->env.hdri, sizeof(state->baked_hdri));
    state->hdri_baked = true;

    if (!state->env.hdri[0])
        return;   // no HDRI: the hemisphere ambient light

    char* path = asset_path(state->env.hdri);
    if (!ibl_create(&state->ibl, path))
        LOG_WARN("No environment loaded from '%s' -- using the hemisphere ambient light\n", path);

    free(path);
}

// env.hdri's index in HDRI_PATHS, or -1 for one the picker doesn't list (a
// scene file can name any .hdr under assets/) or none.
static int
hdri_index(const demo_state* state)
{
    for (uint32 i = 0; i < HDRI_COUNT; ++i)
    {
        if (strcmp(HDRI_PATHS[i], state->env.hdri) == 0)
            return (int)i;
    }
    return -1;
}

// --- Application commands ------------------------------------------------------
// Commands for state the World doesn't hold -- the camera, the HDRI and the
// renderer settings -- registered with every command context, so the
// console (and later an agent) can drive them like any built-in command.

static scene_render_settings build_render_settings(const demo_state* state);

// on_change for both worlds: show the change in the console, and push it to
// connected clients with the world's name, since each world keeps its own
// revision count.
static void
on_world_change(void* user_data, const cJSON* event)
{
    struct world_listener* listener = user_data;
    demo_state*            state    = listener->state;

    console_on_change(&state->console, event);

    if (state->server)
    {
        cJSON* params = cJSON_Duplicate(event, true);
        cJSON_AddStringToObject(params, "world", listener->world_name);
        server_notify(state->server, SERVER_CHANGE_NOTIFICATION, params);
        cJSON_Delete(params);
    }
}

static command_context*
active_commands(demo_state* state)
{
    return state->mode == RENDER_MODE_MATERIAL_GRID ? &state->grid_commands : &state->lit_commands;
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

// Re-derives the orbit controls from the camera, so mouse orbiting carries
// on from wherever a command put it.
static void
orbit_from_camera(demo_state* state)
{
    vec3 offset;
    glm_vec3_sub(state->cam.position, state->cam.target, offset);

    state->orbit_distance = glm_max(glm_vec3_norm(offset), 0.01f);
    state->orbit_yaw      = glm_deg(atan2f(offset[0], offset[2]));
    state->orbit_pitch    = glm_deg(asinf(glm_clamp(offset[1] / state->orbit_distance, -1.0f, 1.0f)));
}

static b8
host_environment_list(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    demo_state* state = user_data;

    *out_result = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(*out_result, "hdris");
    for (uint32 i = 0; i < HDRI_COUNT; ++i)
        cJSON_AddItemToArray(list, cJSON_CreateString(HDRI_PATHS[i]));

    if (state->env.hdri[0])
        cJSON_AddStringToObject(*out_result, "current", state->env.hdri);
    else
        cJSON_AddNullToObject(*out_result, "current");
    cJSON_AddBoolToObject(*out_result, "hdri_loaded", state->ibl.irradiance != 0);
    cJSON_AddBoolToObject(*out_result, "use_ibl", state->env.use_ibl);
    cJSON_AddBoolToObject(*out_result, "show_skybox", state->env.show_skybox);
    cJSON_AddItemToObject(*out_result, "sky_blur", number_json(state->env.sky_blur));
    return true;
}

static b8
host_environment_set(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    demo_state* state = user_data;

    const cJSON* hdri = cJSON_GetObjectItem(args, "hdri");
    int          index = -1;
    if (cJSON_IsString(hdri))
    {
        for (uint32 i = 0; i < HDRI_COUNT; ++i)
        {
            if (strcmp(HDRI_PATHS[i], hdri->valuestring) == 0 || strcmp(HDRI_NAMES[i], hdri->valuestring) == 0)
                index = (int)i;
        }
        if (index < 0)
        {
            snprintf(error, error_size, "unknown HDRI \"%s\"; environment.list shows the available ones", hdri->valuestring);
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

    if (index >= 0)
    {
        snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", HDRI_PATHS[index]);
        sync_hdri(state);
    }

    const cJSON* use_ibl = cJSON_GetObjectItem(args, "use_ibl");
    const cJSON* skybox  = cJSON_GetObjectItem(args, "show_skybox");
    if (cJSON_IsBool(use_ibl)) state->env.use_ibl     = cJSON_IsTrue(use_ibl);
    if (cJSON_IsBool(skybox))  state->env.show_skybox = cJSON_IsTrue(skybox);
    if (blur)                  state->env.sky_blur    = (f32)blur->valuedouble;

    return host_environment_list(user_data, ctx, NULL, out_result, error, error_size);
}

static b8
host_view_get(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    demo_state* state = user_data;

    *out_result = cJSON_CreateObject();
    cJSON_AddItemToObject(*out_result, "position", vec3_json(state->cam.position));
    cJSON_AddItemToObject(*out_result, "target", vec3_json(state->cam.target));
    cJSON_AddItemToObject(*out_result, "fov_degrees", number_json(state->cam.fov_degrees));
    cJSON_AddItemToObject(*out_result, "exposure", number_json(state->env.exposure));
    cJSON_AddItemToObject(*out_result, "ambient_strength", number_json(state->env.ambient_strength));
    return true;
}

static b8
host_view_set_camera(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    demo_state* state = user_data;

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
    orbit_from_camera(state);
    orbit_camera_apply(state);
    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

// Points the camera at an entity (or the whole scene), keeping the current
// viewing angle, and backs off until it fits.
static b8
host_view_frame(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    demo_state*  state = user_data;
    const world* w     = ctx->w;

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
        const uint32 mask = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
        for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
        {
            vec3 emin, emax;
            if (!world_mesh_renderer(w, e)->visible || !world_bounds(w, e, emin, emax))
                continue;
            glm_vec3_minv(bmin, emin, bmin);
            glm_vec3_maxv(bmax, emax, bmax);
            any = true;
        }
    }

    if (!any)
    {
        snprintf(error, error_size, "nothing to frame");
        return false;
    }

    vec3 center;
    glm_vec3_center(bmin, bmax, center);
    f32 radius = glm_max(glm_vec3_distance(bmin, bmax) * 0.5f, 0.05f);

    glm_vec3_copy(center, state->cam.target);
    state->orbit_distance = radius / sinf(glm_rad(state->cam.fov_degrees) * 0.5f) * 1.1f;
    orbit_camera_apply(state);
    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

static b8
host_render_set(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    demo_state* state = user_data;

    const cJSON* exposure = cJSON_GetObjectItem(args, "exposure");
    const cJSON* ambient  = cJSON_GetObjectItem(args, "ambient_strength");
    const cJSON* cascades = cJSON_GetObjectItem(args, "debug_show_cascades");

    if ((exposure && (!cJSON_IsNumber(exposure) || exposure->valuedouble <= 0.0 || exposure->valuedouble > 16.0))
        || (ambient && (!cJSON_IsNumber(ambient) || ambient->valuedouble < 0.0 || ambient->valuedouble > 4.0))
        || (cascades && !cJSON_IsBool(cascades)))
    {
        snprintf(error, error_size, "exposure must be in (0, 16], ambient_strength in [0, 4], debug_show_cascades true/false");
        return false;
    }

    if (exposure) state->env.exposure         = (f32)exposure->valuedouble;
    if (ambient)  state->env.ambient_strength = (f32)ambient->valuedouble;
    if (cascades) state->debug_show_cascades = cJSON_IsTrue(cascades);

    return host_view_get(user_data, ctx, NULL, out_result, error, error_size);
}

// What the scene looks like right now, as a PNG -- rendered fresh,
// without the editor's panels, so a model can see its own work.
static b8
host_view_screenshot(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    demo_state* state = user_data;

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
    if (state->mode == RENDER_MODE_MATERIAL_GRID)
        settings.shadows = false;   // as on screen

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
};

#define HOST_ARGS(array) array, (uint32)(sizeof(array) / sizeof((array)[0]))

static const cmd_host_command HOST_COMMANDS[] = {
    { "environment.list", "The HDRIs that can light the scene, and the current settings", NULL, 0, false, host_environment_list },
    { "environment.set",  "Changes the HDRI and how it lights and backs the scene",         HOST_ARGS(ARGS_ENVIRONMENT_SET), false, host_environment_set },
    { "view.get",         "Where the camera is and what it looks at",                       NULL, 0, false, host_view_get },
    { "view.set_camera",  "Moves the camera and/or what it looks at",                       HOST_ARGS(ARGS_SET_CAMERA),      false, host_view_set_camera },
    { "view.frame",       "Points the camera at an entity (or everything) so it fills the view", HOST_ARGS(ARGS_FRAME),     false, host_view_frame },
    { "render.set",       "Exposure, ambient strength and debug views",                      HOST_ARGS(ARGS_RENDER_SET),      false, host_render_set },
    { "view.screenshot",  "The scene as a PNG (base64), rendered now without the editor's panels -- to check how it looks",
                                                                                               HOST_ARGS(ARGS_SCREENSHOT),      false, host_view_screenshot },
};

#define HOST_COMMAND_COUNT ((uint32)(sizeof(HOST_COMMANDS) / sizeof(HOST_COMMANDS[0])))

// --- Chat agent sidecar -----------------------------------------------------------
// The agent runs as "python -m pbr_agent.agent --exit-with-app" with the
// Python from the agent's virtual environment (agent/README.md), or the one
// PBR_AGENT_PYTHON names. It reads its API key from the environment it
// inherits from the app, and logs to ~/.pbr-renderer/agent.log.

static void
start_agent(demo_state* state)
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
            FATAL("demo: out of memory");
        memcpy(python, configured, length);
    }
    else
    {
#ifdef _WIN32
        python = asset_path("../agent/.venv/Scripts/python.exe");
#else
        python = asset_path("../agent/.venv/bin/python");
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

// F5: starts the game (TESTBED_EXE, set by CMake) following this editor's
// scene. Publishes first, so it finds the current world straight away.
static void
start_game(demo_state* state)
{
    if (state->game_process && process_running(state->game_process, NULL))
    {
        snprintf(state->game_note, sizeof(state->game_note), "The game is already running.");
        return;
    }
    process_destroy(state->game_process);
    state->game_process = NULL;

    if (!file_exists(TESTBED_EXE))
    {
        snprintf(state->game_note, sizeof(state->game_note), "No game at %s; build the Testbed target.", TESTBED_EXE);
        return;
    }

    const char* argv[] = { TESTBED_EXE, "--live", NULL };
    state->game_process = process_spawn(argv, state->game_log);
    snprintf(state->game_note, sizeof(state->game_note), state->game_process
             ? "Game running, following your edits." : "Could not start the game; see %s.", state->game_log);
}

// Notices the game being closed.
static void
watch_game(demo_state* state)
{
    int exit_code;
    if (!state->game_process || process_running(state->game_process, &exit_code))
        return;

    snprintf(state->game_note, sizeof(state->game_note), exit_code == 0
             ? "Game closed." : "The game exited (code %d); its log is %s.", exit_code, state->game_log);
    process_destroy(state->game_process);
    state->game_process = NULL;
}

// Notices the sidecar exiting, and says where to look.
static void
watch_agent(demo_state* state)
{
    int exit_code;
    if (!state->agent_process || process_running(state->agent_process, &exit_code))
        return;

    snprintf(state->agent_note, sizeof(state->agent_note), "The agent exited (code %d); its log is %s.",
             exit_code, state->agent_log);
    process_destroy(state->agent_process);
    state->agent_process = NULL;
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

    state->env = scene_environment_default();
    snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", HDRI_PATHS[0]);
    sync_hdri(state);

    state->mode = RENDER_MODE_LIT_MODEL;

    glm_vec3_copy((vec3){ 0.9f, 0.3f, 0.25f }, state->default_albedo);
    state->default_metallic  = 0.0f;
    state->default_roughness = 0.45f;

    // --- Worlds -------------------------------------------------------------
    assets_init(&state->assets);
    world_init(&state->lit_world, &state->assets);
    world_init(&state->grid_world, &state->assets);

    create_lit_world(state);
    create_grid_world(state);

    state->selected_model = 0;
    state->loaded_model   = -1;
    load_selected_model(state, state->selected_model);
    world_update(&state->grid_world);

    // --- Commands + console ---------------------------------------------------
    console_init(&state->console);
    state->show_console = true;

    history_init(&state->lit_history);
    history_init(&state->grid_history);

    state->lit_listener  = (struct world_listener){ state, "lit" };
    state->grid_listener = (struct world_listener){ state, "grid" };

    state->server = server_start(NULL);
    chat_init(&state->chat, state->server);
    chat_panel_init(&state->chat_panel);

    uint32                  chat_command_count;
    const cmd_host_command* chat_command_table = chat_commands(&chat_command_count);

    command_context*       contexts[2]  = { &state->lit_commands, &state->grid_commands };
    world*                 worlds[2]    = { &state->lit_world, &state->grid_world };
    command_history*       histories[2] = { &state->lit_history, &state->grid_history };
    struct world_listener* listeners[2] = { &state->lit_listener, &state->grid_listener };
    for (int i = 0; i < 2; ++i)
    {
        *contexts[i] = (command_context){
            .w                    = worlds[i],
            .history              = histories[i],
            .on_change            = on_world_change,
            .on_change_user       = listeners[i],
            .host_commands        = HOST_COMMANDS,
            .host_command_count   = HOST_COMMAND_COUNT,
            .host_user            = state,
            .module_commands      = chat_command_table,
            .module_command_count = chat_command_count,
            .module_user          = &state->chat,
            .environment          = &state->env,
        };
    }

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
    snprintf(state->game_log, sizeof(state->game_log), "%s/.pbr-renderer/game.log", home ? home : ".");
    live_publisher_init(&state->live, NULL);

    // The chat agent connects through the server, so without one there's no agent.
    const char* autostart = getenv("PBR_AGENT_AUTOSTART");
    if (!state->server)
        snprintf(state->agent_note, sizeof(state->agent_note), "The command server didn't start, so no agent can connect.");
    else if (autostart && strcmp(autostart, "0") == 0)
        snprintf(state->agent_note, sizeof(state->agent_note), "Not started automatically (PBR_AGENT_AUTOSTART=0).");
    else
        start_agent(state);

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

    demo_state* state = (demo_state*)app->user_data;
    if (key == GLFW_KEY_F1 && action == GLFW_PRESS)
        state->show_console = !state->show_console;
    if (key == GLFW_KEY_F5 && action == GLFW_PRESS)
        start_game(state);
}

// One editor per Light entity in `w` -- whatever lights exist, including
// ones added or renamed from the console. Edits go straight into the
// components (no undo; the console's history covers commands).
static void
draw_light_editors(world* w)
{
    static const char* const TYPE_NAMES[] = { "directional", "point", "spot" };

    igText("Lights");
    const uint32 mask  = COMPONENT_BIT(COMPONENT_LIGHT);
    uint32       count = 0;

    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        c_light*     l = world_light(w, e);
        c_transform* t = world_transform(w, e);
        ++count;

        igPushID_Int((int)e);
        char header[ENTITY_NAME_MAX + 32];
        snprintf(header, sizeof(header), "%s (%s)", world_name(w, e), TYPE_NAMES[l->type]);

        if (igCollapsingHeader_TreeNodeFlags(header, l->type == LIGHT_DIRECTIONAL ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            igColorEdit3("Color", l->color, 0);
            igSliderFloat("Intensity", &l->intensity, 0.0f, l->type == LIGHT_DIRECTIONAL ? 10.0f : 50.0f,
                          l->intensity > 0.0f ? "%.2f" : "off (0)", 0);

            if (l->type == LIGHT_DIRECTIONAL)
            {
                igSliderFloat3("Direction", l->direction, -1.0f, 1.0f, "%.2f", 0);
            }
            else
            {
                b8 moved = igSliderFloat3("Position", t->position, -5.0f, 5.0f, "%.2f", 0);
                igSliderFloat("Range", &l->range, 0.0f, 20.0f, l->range > 0.0f ? "%.2f" : "unlimited", 0);

                if (l->type == LIGHT_SPOT)
                {
                    igSliderFloat("Inner angle", &l->inner_cone_degrees, 0.0f, 89.0f, "%.1f deg", 0);
                    igSliderFloat("Outer angle", &l->outer_cone_degrees, 0.0f, 89.0f, "%.1f deg", 0);
                    if (l->outer_cone_degrees < l->inner_cone_degrees)
                        l->outer_cone_degrees = l->inner_cone_degrees;

                    // Keep the cone on the model while dragging it around.
                    if ((moved || igButton("Aim at origin", (ImVec2_c){ 0, 0 })) && glm_vec3_norm2(t->position) > 1e-8f)
                        glm_vec3_negate_to(t->position, l->direction);
                }
            }
        }
        igPopID();
    }

    if (count == 0)
        igTextDisabled("(no lights -- add one from the console)");
    igSeparator();
}

// Ambient, exposure and image-based lighting: the scene's environment, not
// World entities (saved with the scene all the same).
static void
draw_environment_controls(demo_state* state)
{
    igSliderFloat("Ambient", &state->env.ambient_strength, 0.0f, 2.0f, "%.2f", 0);
    igSliderFloat("Exposure", &state->env.exposure, 0.1f, 4.0f, "%.2f", 0);
    igSeparator();

    igText("Image-based lighting");
    int index = hdri_index(state);
    if (igCombo_Str_arr("HDRI", &index, HDRI_NAMES, (int)HDRI_COUNT, -1) && index >= 0)
        snprintf(state->env.hdri, sizeof(state->env.hdri), "%s", HDRI_PATHS[index]);
    if (index < 0 && state->env.hdri[0])
        igTextDisabled("From the scene: %s", state->env.hdri);

    if (state->ibl.irradiance)
    {
        igCheckbox("Use IBL", &state->env.use_ibl);
        igCheckbox("Show skybox", &state->env.show_skybox);
        igSliderFloat("Sky blur", &state->env.sky_blur, 0.0f, 1.0f, "%.2f", 0);
    }
    else if (state->env.hdri[0])
    {
        igText("No HDRI loaded. Expected:");
        igText("assets/%s", state->env.hdri);
    }
    else
    {
        igText("No HDRI set.");
    }
    igSeparator();
}

// The model viewer's "Spin": a Spin component on model_pivot (the model is
// centred under it, so it turns about its own middle), the same rate the
// viewer always used: 0.5 rad/s about Y. A component rather than an
// animation in this file, so the game plays it too.
#define MODEL_SPIN_DEGREES_PER_SECOND 28.6479f

static void
set_model_spin(demo_state* state, b8 on)
{
    world* w = &state->lit_world;
    if (on)
        world_add_spin(w, state->model_pivot, (vec3){ 0.0f, MODEL_SPIN_DEGREES_PER_SECOND, 0.0f });
    else
        world_remove(w, state->model_pivot, COMPONENT_SPIN);
}

// --- Spin preview -------------------------------------------------------------------
// The editor never runs world_tick(): what it holds -- and what panels,
// commands, scene.save and the live link see -- is the authored pose. To still
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
        FATAL("demo: out of memory");

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

// The grid's albedo picker, applied to every sphere still in the grid
// world when it changes.
static void
apply_grid_albedo(demo_state* state)
{
    f32 albedo[3];
    srgb_to_linear(state->grid_albedo, albedo);

    for (int i = 0; i < GRID_ROWS * GRID_COLUMNS; ++i)
    {
        c_mesh_renderer* mr = world_mesh_renderer(&state->grid_world, state->grid_spheres[i]);
        if (!mr)
            continue;   // removed from the console
        mr->material.fields |= MATERIAL_OVERRIDE_BASE_COLOR;
        mr->material.base_color[0] = albedo[0];
        mr->material.base_color[1] = albedo[1];
        mr->material.base_color[2] = albedo[2];
    }
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

    scene_render_settings_apply_environment(&settings, &state->env, &state->ibl);
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

    // Requests from other processes, run against the world on screen --
    // before rendering, so their changes show this same frame.
    server_poll(state->server, active_commands(state));
    chat_poll(&state->chat);
    watch_agent(state);
    watch_game(state);

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
        sync_hdri(state);

        spin_preview preview = spin_preview_begin(&state->grid_world, time);
        world_update(&state->grid_world);

        // The demo is about ambient light, so no shadow pass.
        scene_render_settings settings = build_render_settings(state);
        settings.shadows = false;

        scene_renderer_draw(&state->renderer, &state->grid_world, &state->cam, &settings);
        spin_preview_end(&state->grid_world, &preview);
    }
    else // RENDER_MODE_LIT_MODEL
    {
        // A different model was picked in the GUI last frame.
        if (state->selected_model != state->loaded_model)
            load_selected_model(state, state->selected_model);

        sync_hdri(state);

        spin_preview preview = spin_preview_begin(&state->lit_world, time);
        world_update(&state->lit_world);

        scene_render_settings settings = build_render_settings(state);
        scene_renderer_draw(&state->renderer, &state->lit_world, &state->cam, &settings);
        spin_preview_end(&state->lit_world, &preview);
    }

    // App/on_frame runs between imgui_layer_new_frame() and
    // imgui_layer_render() (see App.c), so any ig*() calls here just work.
    // Fullscreen dockspace so ImGui windows can actually be dragged and
    // docked against something. dockspace_id=0 auto-generates an ID; NULL
    // viewport = the main viewport; no flags, no window-class restriction.
    igDockSpaceOverViewport(0, NULL, ImGuiDockNodeFlags_PassthruCentralNode, NULL);

    igBegin("Scene", NULL, 0);

    if (state->server)
        igTextDisabled("Command server: port %u, %u client(s)", (unsigned)server_port(state->server),
                       server_client_count(state->server));

    if (igButton(state->game_process ? "Game running (F5)" : "Play in Testbed (F5)", (ImVec2_c){ 0, 0 }))
        start_game(state);
    if (state->game_note[0])
        igTextDisabled("%s", state->game_note);
    igSeparator();

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
        if (igColorEdit3("Albedo", state->grid_albedo, 0))
            apply_grid_albedo(state);
        igSeparator();

        draw_light_editors(&state->grid_world);
        draw_environment_controls(state);
    }
    else // RENDER_MODE_LIT_MODEL
    {
        igCombo_Str_arr("Model", &state->selected_model, MODEL_NAMES, (int)MODEL_COUNT, -1);

        const c_mesh_renderer* model_mr = world_mesh_renderer(&state->lit_world, state->model_entity);
        const model*           m        = model_mr ? assets_model(&state->assets, model_mr->model) : NULL;
        if (!model_mr)
        {
            igTextWrapped("The \"model\" entity was removed; pick a model to bring it back.");
        }
        else if (m)
        {
            igText("%u submeshes, %u materials, %u textures", m->submesh_count, m->material_count, m->texture_count);
            igText("%u vertices, %u triangles", m->vertex_count, m->triangle_count);
        }
        else
        {
            igText("Model failed to load (see console)");
        }

        b8 spin = world_has(&state->lit_world, state->model_pivot, COMPONENT_SPIN);
        if (igCheckbox("Spin", &spin))
            set_model_spin(state, spin);

        c_mesh_renderer* ground = world_mesh_renderer(&state->lit_world, state->ground_entity);
        if (ground)
            igCheckbox("Ground plane", &ground->visible);

        igCheckbox("Debug: show shadow cascades", &state->debug_show_cascades);
        igSeparator();

        draw_light_editors(&state->lit_world);
        draw_environment_controls(state);

        igText("Default material (models without one)");
        b8 changed = igColorEdit3("Albedo", state->default_albedo, 0);
        changed |= igSliderFloat("Metallic", &state->default_metallic, 0.0f, 1.0f, "%.2f", 0);
        changed |= igSliderFloat("Roughness", &state->default_roughness, 0.0f, 1.0f, "%.2f", 0);
        if (changed)
            apply_default_material(state);
    }

    igEnd();

    // Every entity and component of the world on screen (read-only), and
    // the command console for it. The textured cube has no world; the
    // console then works on the lit-model world.
    command_context* active = active_commands(state);
    if (state->mode != RENDER_MODE_TEXTURED_CUBE)
        entity_panel_draw(active->w, "Entities");
    if (state->show_console)
        console_draw(&state->console, active, "Console (F1)");

    if (chat_panel_draw(&state->chat_panel, &state->chat, active, "Chat", state->agent_note) == CHAT_PANEL_START_AGENT
        && state->server)
        start_agent(state);

    // Whatever changed this frame -- panels, console, agent -- goes to the
    // live scene file, for a game running with --live.
    // TODO: re-serializes the whole world to detect a change; to be replaced
    // by an in-editor Play mode / incremental sync (World/LiveScene.h). That
    // needs the panels to edit through commands, so every change is an event.
    live_publisher_publish(&state->live, active->w, &state->env, window_get_time(), 0.1);
}

// Runs from terminate(), before the GL context goes away.
static void on_shutdown(App* app)
{
    demo_state* state = (demo_state*)app->user_data;

    for (uint32 i = 0; i < TEXTURE_COUNT; ++i)
        rhi_texture_destroy(state->textures[i]);

    chat_shutdown(&state->chat);             // closes a running turn's transaction
    server_stop(state->server);              // the agent (--exit-with-app) leaves when its connection closes...
    process_destroy(state->agent_process);   // ...or is ended here
    process_destroy(state->game_process);    // the game started by F5 closes with the editor
    live_publisher_shutdown(&state->live);
    console_shutdown(&state->console);
    history_shutdown(&state->lit_history, &state->lit_world);     // histories hold asset references too
    history_shutdown(&state->grid_history, &state->grid_world);
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
        .name = "PBR Renderer - Editor",
        .on_init = on_init,
        .on_frame = on_frame,
        .on_shutdown = on_shutdown,
        .on_key = on_key,
        .on_scroll = on_scroll,
        .on_resize = on_resize,
        .user_data = &state,
        .should_close = false,
        .use_imgui = true
    };

    run(&app);
    terminate(&app);

    return 0;
}