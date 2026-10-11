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
#include "Renderer/Lighting.h"
#include "Renderer/Renderer.h"
#include "Renderer/Shadow.h"
#include "Scene/Camera.h"
#include "Scene/Model.h"
#include "Scene/Scene.h"
#include "Platform/Window.h"
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

// Which of the two demo scenes on_frame draws this frame -- both share the
// same camera/orbit controls, only the geometry/shader/GUI panel differ.
typedef enum render_mode
{
    RENDER_MODE_TEXTURED_CUBE,
    RENDER_MODE_LIT_MODEL,
    RENDER_MODE_MATERIAL_GRID   // IBL demo: a metallic x roughness sphere grid, see draw_material_grid()
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
    // A loaded model (Scene/Model.h) drawn through the PBR shader with one
    // directional light. Everything below is exposed through ImGui so the
    // shading math is easy to see change live.
    model      scene_model;
    int        selected_model;     // index into MODEL_PATHS/MODEL_NAMES, driven by the GUI combo box
    int        loaded_model;       // which entry scene_model was last loaded from (-1 = not yet); the load may have failed, see submesh_count
    mat4       model_fit;          // centres + scales scene_model to a fixed on-screen size
    rhi_shader pbr_shader;
    b8         spin;
    vec3       light_dir;          // direction the light travels toward the surface; normalized before use
    vec3       light_color;
    f32        light_intensity;
    f32        ambient_strength;
    f32        exposure;

    // --- Scene ---------------------------------------------------------------
    // scene_model/light_dir above are still the values ImGui edits directly
    // (sliders bind straight to them, same as before); demo_scene just holds
    // an ENTITY_MODEL and an ENTITY_LIGHT that mirror them each frame, so
    // on_frame can draw/gather through Scene.h instead of the single model
    // and single light uniform set it used to reach for directly. as_model
    // points at scene_model above, which never moves (same demo_state
    // address for the app's lifetime), so it's wired up once in on_init().
    scene demo_scene;
    int32 model_entity;
    int32 light_entity;

    // --- Fill light (point) -------------------------------------------------
    // A second scene light, demonstrating multi-light support end to end:
    // position/color/intensity/range are all live-editable via ImGui, same
    // as the key (directional) light above. Disabled by zeroing intensity
    // (see sync_scene_lights()) rather than removed -- Scene.h has no
    // entity-removal call yet (see its ownership note). Casts no shadow.
    int32 fill_light_entity;
    b8    fill_light_enabled;
    vec3  fill_light_pos;
    vec3  fill_light_color;
    f32   fill_light_intensity;
    f32   fill_light_range;

    // --- Shadow mapping ----------------------------------------------------
    // Cascaded shadow maps (Renderer/Shadow.h) for the key directional
    // light, re-fit and re-rendered every frame -- both the camera (orbit
    // controls) and light_dir (live via the GUI) can change frame to
    // frame, so the cascades can't be baked once at load time.
    csm_state csm;
    b8        debug_show_cascades;   // tints shading by cascade index; see pbr.frag's u_debug_show_cascades

    // --- Image-based lighting -----------------------------------------------
    // Baked from an HDRI (Renderer/IBL.h) and read by pbr.frag for ambient
    // diffuse + specular. All zeroes when no HDRI could be loaded.
    // --- Material grid (RENDER_MODE_MATERIAL_GRID) ----------------------------
    // The classic IBL test pattern: the same sphere drawn 7 x 5 times, roughness
    // sweeping left to right and metallic top to bottom. Loaded on first use.
    model           grid_model;
    b8              grid_load_attempted;
    vec3            grid_albedo;         // sRGB, like default_albedo; converted to linear when drawn

    ibl_environment ibl;
    int             selected_hdri;   // index into HDRI_PATHS/HDRI_NAMES, driven by the GUI combo box
    int             loaded_hdri;     // which entry `ibl` was last baked from (-1 = not yet); the load may have failed
    b8              ibl_enabled;     // use `ibl` for ambient light (needs a loaded HDRI)
    b8              show_skybox;     // draw the HDRI as the background
    f32             sky_blur;        // 0..1, how blurry the background is

    // --- Ground plane --------------------------------------------------------
    // A big flat quad the shadow actually lands on -- built once in on_init()
    // like CUBE_VERTICES above rather than wrapped in a model, since
    // Scene/ModelInternal.h's model_add_*() helpers are private to the
    // Scene/Model*.c loaders and this is just one hand-built quad.
    mesh     ground_mesh;
    material ground_material;
    f32      ground_y;       // world-space height it sits at; set per loaded model, see load_selected_model()
    b8       show_ground;

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

// (Re)loads the model at MODEL_PATHS[index] into state->scene_model, freeing
// whatever was there. Runs on the render thread with the GL context current,
// which is all model_load() needs.
static void
load_selected_model(demo_state* state, int index)
{
    model_destroy(&state->scene_model);

    char* path = asset_path(MODEL_PATHS[index]);
    state->scene_model = model_load(path);
    free(path);

    // Recorded even when the load failed, so on_frame doesn't retry (and
    // re-log the error) every single frame; the draw path checks
    // submesh_count instead.
    state->loaded_model = index;

    if (state->scene_model.submesh_count == 0)
    {
        LOG_ERROR("demo: failed to load model '%s'\n", MODEL_PATHS[index]);
        return;
    }

    // Assets come in wildly different units; fit everything into a ~2 unit
    // box at the origin so the orbit camera framing works for all of them.
    model_get_fit_transform(&state->scene_model, 2.0f, state->model_fit);

    // Sit the ground plane at the fitted model's lowest point, so it reads
    // as something the model stands on instead of floating through it.
    // on_frame's world = Spin * Fit only ever rotates around Y through the
    // origin, which doesn't change Y, so this Y stays correct at every
    // spin angle, not just the one at load time.
    vec3 fitted_min;
    glm_mat4_mulv3(state->model_fit, state->scene_model.bounds_min, 1.0f, fitted_min);
    state->ground_y = fitted_min[1];
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
    char* pbr_vertex_path   = asset_path("shaders/pbr.vert");
    char* pbr_fragment_path = asset_path("shaders/pbr.frag");

    state->pbr_shader = rhi_shader_create_from_files(pbr_vertex_path, pbr_fragment_path);

    free(pbr_vertex_path);
    free(pbr_fragment_path);

    if (!state->pbr_shader)
        FATAL("demo: failed to load PBR shader");

    if (!csm_create(&state->csm, CASCADE_COUNT, SHADOW_MAP_SIZE))
        FATAL("demo: failed to create cascaded shadow maps");

    // Cascade texture units (CSM_SLOT_FIRST..+CSM_MAX_CASCADES-1) sit right
    // after Material.h's five material slots (MATERIAL_SLOT_COUNT) so
    // material_bind() never binds over them -- csm_bind() rebinds the
    // actual textures every frame (shadow map contents change), but the
    // sampler-to-unit assignment itself is also just set there each frame
    // since it's cheap and keeps all of csm_bind()'s uniform-setting in one
    // place rather than splitting setup between on_init() and on_frame().

    state->grid_albedo[0] = state->grid_albedo[1] = state->grid_albedo[2] = 0.9f;

    state->show_skybox   = true;
    state->sky_blur      = 0.0f;
    state->selected_hdri = 0;
    state->loaded_hdri   = -1;
    load_selected_hdri(state, state->selected_hdri);

    // --- Ground plane ------------------------------------------------------
    // A big flat quad on the XZ plane so the model's shadow has something
    // to land on. Only its Y offset changes later (per model, in
    // load_selected_model()); the geometry itself is built once here.
    {
        const f32 half_size = 4.0f;

        mesh_vertex ground_vertices[4] = {
            { .position = { -half_size, 0.0f, -half_size }, .normal = { 0.0f, 1.0f, 0.0f }, .uv = { 0.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f } },
            { .position = {  half_size, 0.0f, -half_size }, .normal = { 0.0f, 1.0f, 0.0f }, .uv = { 1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f } },
            { .position = {  half_size, 0.0f,  half_size }, .normal = { 0.0f, 1.0f, 0.0f }, .uv = { 1.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f } },
            { .position = { -half_size, 0.0f,  half_size }, .normal = { 0.0f, 1.0f, 0.0f }, .uv = { 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f } },
        };

        // Wound so the +Y-normal face is front-facing under the RHI's
        // default CCW-front convention (rhi_render_state_default(), RHI.c).
        uint32 ground_indices[6] = { 0, 2, 1,  0, 3, 2 };

        state->ground_mesh = mesh_create(ground_vertices, 4, ground_indices, 6);

        material_init_default(&state->ground_material);
        state->ground_material.base_color[0] = 0.18f;   // dim, non-metal concrete-ish grey --
        state->ground_material.base_color[1] = 0.18f;   // dark enough that the shadow reads clearly
        state->ground_material.base_color[2] = 0.18f;   // against it without crushing to black
        state->ground_material.roughness     = 0.9f;

        state->show_ground = true;
    }

    state->mode = RENDER_MODE_LIT_MODEL;

    glm_vec3_copy((vec3){ 0.9f, 0.3f, 0.25f }, state->default_albedo);
    state->default_metallic  = 0.0f;
    state->default_roughness = 0.45f;

    glm_vec3_copy((vec3){ -0.4f, -0.6f, -0.5f }, state->light_dir);
    glm_vec3_copy((vec3){ 1.0f, 0.98f, 0.92f }, state->light_color);
    state->light_intensity  = 2.5f;
    state->ambient_strength = 0.5f;
    state->exposure         = 1.0f;

    state->selected_model = 0;
    state->loaded_model   = -1;
    load_selected_model(state, state->selected_model);

    // --- Scene -------------------------------------------------------------
    // &state->scene_model is stable for the app's lifetime (load_selected_model()
    // reloads *into* it, never reallocates it), so the entity can borrow it
    // once here; on_frame only ever touches local_transform/as_light after
    // this, never re-adds anything.
    scene_init(&state->demo_scene);

    state->model_entity = scene_add_model(&state->demo_scene, &state->scene_model, NULL, ENTITY_NO_PARENT);

    light initial_light;
    light_init_directional(&initial_light, state->light_dir, state->light_color, state->light_intensity);
    state->light_entity = scene_add_light(&state->demo_scene, initial_light, NULL, ENTITY_NO_PARENT);

    // Added after the key light, so lighting_bind() keeps shadowing the key
    // light (first directional in scene order).
    state->fill_light_enabled   = true;
    glm_vec3_copy((vec3){ 1.5f, 1.0f, 1.5f }, state->fill_light_pos);
    glm_vec3_copy((vec3){ 0.55f, 0.7f, 1.0f }, state->fill_light_color);
    state->fill_light_intensity = 3.0f;
    state->fill_light_range     = 8.0f;

    light fill_light;
    light_init_point(&fill_light, state->fill_light_color, state->fill_light_intensity, state->fill_light_range);
    state->fill_light_entity = scene_add_light(&state->demo_scene, fill_light, NULL, ENTITY_NO_PARENT);

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

// Pushes this frame's GUI-driven light values into demo_scene's light
// entities and refreshes world transforms, so lighting_bind() and
// scene_gather_lights() see them. Both entities are unparented, so local ==
// world transform. The fill light is disabled by zeroing its intensity --
// Scene.h has no entity removal.
static void
sync_scene_lights(demo_state* state)
{
    light* key = &state->demo_scene.entities[state->light_entity].as_light;
    glm_vec3_normalize_to(state->light_dir, key->direction);
    glm_vec3_copy(state->light_color, key->color);
    key->intensity = state->light_intensity;

    entity* fill_entity = &state->demo_scene.entities[state->fill_light_entity];
    glm_translate_make(fill_entity->local_transform, state->fill_light_pos);
    glm_vec3_copy(state->fill_light_color, fill_entity->as_light.color);
    fill_entity->as_light.intensity = state->fill_light_enabled ? state->fill_light_intensity : 0.0f;
    fill_entity->as_light.range     = state->fill_light_range;

    scene_update_world_transforms(&state->demo_scene);
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

#define GRID_COLUMNS        7       // roughness: 0.05 (left) .. 1.0 (right)
#define GRID_ROWS           5       // metallic:  1.0 (top)   .. 0.0 (bottom)
#define GRID_SPACING        0.62f   // centre to centre, in world units
#define GRID_SPHERE_RADIUS  0.27f

// The grid sits right of the origin because the "Scene" panel covers the left
// third of the default 800x600 window, and the leftmost column (the mirror-
// smooth spheres) is the one worth seeing.
#define GRID_CENTER_X       1.5f

// Draws the IBL demo: one sphere per (roughness, metallic) pair, lit by the
// baked environment. Shows what image-based lighting does to each kind of
// surface: mirror-sharp reflections on the smooth metals (top left) that blur
// out to a soft glow as roughness rises, and diffuse-only dielectrics (bottom
// row) that pick up the sky and ground colors from the irradiance map.
static void
draw_material_grid(demo_state* state, const mat4 view_projection)
{
    if (!state->grid_load_attempted)
    {
        state->grid_load_attempted = true;   // a failed load is logged once, not every frame

        char* path = asset_path("models/sphere.obj");
        state->grid_model = model_load(path);
        free(path);
    }

    model* sphere = &state->grid_model;
    if (sphere->submesh_count == 0)
        return;

    if (state->ibl_enabled && state->show_skybox)
    {
        mat4 view, projection;
        camera_get_view(&state->cam, view);
        camera_get_projection(&state->cam, projection);

        ibl_draw_skybox(&state->ibl, view, projection, state->exposure, state->sky_blur);
    }

    sync_scene_lights(state);

    rhi_shader shader = state->pbr_shader;

    rhi_shader_bind(shader);
    rhi_shader_set_mat4(shader, "u_view_projection", (const f32*)view_projection);
    lighting_bind(shader, &state->demo_scene);
    rhi_shader_set_vec3(shader, "u_view_pos",
                         state->cam.position[0], state->cam.position[1], state->cam.position[2]);
    rhi_shader_set_float(shader, "u_ambient_strength", state->ambient_strength);
    rhi_shader_set_float(shader, "u_exposure", state->exposure);

    // The demo is about ambient light: rather than fit and render cascades
    // for it, just tell the shader nothing casts a shadow (see pbr.frag's
    // pick_cascade()) -- cheaper than the single-shadow-map version's
    // old trick of rendering an empty shadow map with an identity light
    // matrix, and needs no framebuffer bound at all.
    rhi_shader_set_int(shader, "u_cascade_count", 0);

    ibl_bind(state->ibl_enabled ? &state->ibl : NULL, shader);

    // sphere.obj has no material of its own, so every submesh uses
    // default_material: rewrite it between draws. The picker shows sRGB,
    // material colors are linear.
    material* dm = &sphere->default_material;
    dm->base_color[0] = powf(state->grid_albedo[0], 2.2f);
    dm->base_color[1] = powf(state->grid_albedo[1], 2.2f);
    dm->base_color[2] = powf(state->grid_albedo[2], 2.2f);

    mat4 fit;
    model_get_fit_transform(sphere, 2.0f, fit);   // radius 1, centred on the origin

    for (int row = 0; row < GRID_ROWS; ++row)
    {
        for (int col = 0; col < GRID_COLUMNS; ++col)
        {
            dm->metallic  = 1.0f - (f32)row / (f32)(GRID_ROWS - 1);
            dm->roughness = 0.05f + 0.95f * (f32)col / (f32)(GRID_COLUMNS - 1);

            vec3 position = {
                GRID_CENTER_X + ((f32)col - 0.5f * (GRID_COLUMNS - 1)) * GRID_SPACING,
                (0.5f * (GRID_ROWS - 1) - (f32)row)    * GRID_SPACING,
                0.0f
            };

            mat4 world;
            glm_mat4_identity(world);
            glm_translate(world, position);
            glm_scale_uni(world, GRID_SPHERE_RADIUS);
            glm_mat4_mul(world, fit, world);

            model_draw(sphere, shader, world);
        }
    }
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

    f32 time = (f32)window_get_time();

    mat4 view_projection;
    camera_get_view_projection(&state->cam, view_projection);

    if (state->mode == RENDER_MODE_TEXTURED_CUBE)
    {
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

        draw_material_grid(state, view_projection);
    }
    else // RENDER_MODE_LIT_MODEL
    {
        // A different model was picked in the GUI last frame.
        if (state->selected_model != state->loaded_model)
            load_selected_model(state, state->selected_model);

        if (state->selected_hdri != state->loaded_hdri)
            load_selected_hdri(state, state->selected_hdri);

        if (state->scene_model.submesh_count > 0)
        {
            // Push the GUI's default-material tweaks into the model; only
            // meshes without their own material (bare .obj files) use it.
            //
            // The color picker shows (and the artist thinks in) sRGB, while
            // material colors are linear -- convert, or the color comes out
            // washed out.
            material* dm = &state->scene_model.default_material;
            dm->base_color[0] = powf(state->default_albedo[0], 2.2f);
            dm->base_color[1] = powf(state->default_albedo[1], 2.2f);
            dm->base_color[2] = powf(state->default_albedo[2], 2.2f);
            dm->metallic      = state->default_metallic;
            dm->roughness     = state->default_roughness;

            // world = Spin * Fit: centre and scale the model first, then rotate
            // it about its own middle.
            mat4 world;
            glm_mat4_identity(world);
            if (state->spin)
                glm_rotate(world, time * 0.5f, (vec3){ 0.0f, 1.0f, 0.0f });
            glm_mat4_mul(world, state->model_fit, world);

            // Push this frame's GUI-driven values into the scene: the model
            // entity's placement (unparented, so local == world), then the
            // lights, which also refreshes world transforms.
            glm_mat4_copy(world, state->demo_scene.entities[state->model_entity].local_transform);
            sync_scene_lights(state);

            // The key light is the scene's first light and its only
            // directional one, which is the light lighting_bind() marks as
            // shadowed -- so fit the cascades to its world-space direction.
            light gathered_lights[1];
            scene_gather_lights(&state->demo_scene, gathered_lights, NULL, 1);
            f32* light_dir_normalized = gathered_lights[0].direction;   // normalized by scene_gather_lights()

            // --- Shadow pass -----------------------------------------------
            // Re-fit every cascade to this frame's camera + light_dir, then
            // render each one's depth pass. See Renderer/Shadow.h for how
            // the fit itself works; unlike the old single fixed-radius-2
            // ortho box, this tracks the camera instead of the model, so it
            // scales to whatever the camera can see rather than whatever
            // model_get_fit_transform() happened to fit into a 2-unit cube.
            csm_update(&state->csm, &state->cam, light_dir_normalized, 0.5f);
            csm_render(&state->csm, &state->demo_scene);

            // Background first: it writes no depth, and blended materials drawn
            // below need the sky already behind them.
            if (state->ibl_enabled && state->show_skybox)
            {
                mat4 view, projection;
                camera_get_view(&state->cam, view);
                camera_get_projection(&state->cam, projection);

                ibl_draw_skybox(&state->ibl, view, projection, state->exposure, state->sky_blur);
            }

            rhi_shader shader = state->pbr_shader;

            mat4 view_only;
            camera_get_view(&state->cam, view_only);

            rhi_shader_bind(shader);
            rhi_shader_set_mat4(shader, "u_view_projection", (const f32*)view_projection);
            rhi_shader_set_mat4(shader, "u_view", (const f32*)view_only);   // pbr.vert: v_view_depth, for cascade selection
            rhi_shader_set_int(shader, "u_debug_show_cascades", state->debug_show_cascades);
            lighting_bind(shader, &state->demo_scene);
            rhi_shader_set_vec3(shader, "u_view_pos",
                                 state->cam.position[0], state->cam.position[1], state->cam.position[2]);
            rhi_shader_set_float(shader, "u_ambient_strength", state->ambient_strength);
            rhi_shader_set_float(shader, "u_exposure", state->exposure);

            // Ambient light: baked IBL when enabled, else the shader's
            // hemisphere fallback. Uses IBL_SLOT_* texture units, clear of
            // the material slots and the shadow cascades.
            ibl_bind(state->ibl_enabled ? &state->ibl : NULL, shader);

            // Binds every cascade's depth texture to its fixed unit
            // (CSM_SLOT_FIRST..+CSM_MAX_CASCADES-1, clear of the material
            // slots -- material_bind() inside model_draw() below never
            // touches them) and sets the matching u_shadow_maps[]/
            // u_light_view_projection[]/u_cascade_splits[]/u_cascade_count
            // uniforms.
            csm_bind(&state->csm, shader);

            // u_model / u_normal_matrix and every material uniform + texture
            // binding are set per submesh inside model_draw() (called once
            // per ENTITY_MODEL by scene_draw_models() -- just the one
            // model_entity today, but N from here on costs nothing extra).
            scene_draw_models(&state->demo_scene, shader);

            if (state->show_ground)
            {
                // Not part of `world` (Spin * Fit) -- the ground shouldn't
                // spin with the model, just sit under it at ground_y.
                mat4 ground_world;
                glm_mat4_identity(ground_world);
                glm_translate(ground_world, (vec3){ 0.0f, state->ground_y, 0.0f });

                mat4 ground_normal_matrix;
                glm_mat4_inv(ground_world, ground_normal_matrix);
                glm_mat4_transpose(ground_normal_matrix);

                rhi_shader_set_mat4(shader, "u_model", (const f32*)ground_world);
                rhi_shader_set_mat4(shader, "u_normal_matrix", (const f32*)ground_normal_matrix);

                // The cascades are already bound to their fixed units from
                // csm_bind() above and material_bind() never touches them,
                // so they're still in place here -- no rebind needed.
                material_bind(&state->ground_material, shader, false);
                mesh_draw(&state->ground_mesh);

                rhi_set_render_state(rhi_render_state_default());
            }
        }
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

        if (state->scene_model.submesh_count > 0)
        {
            const model* m = &state->scene_model;

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
}

// Runs from terminate(), before the GL context goes away.
static void on_shutdown(App* app)
{
    demo_state* state = (demo_state*)app->user_data;

    for (uint32 i = 0; i < TEXTURE_COUNT; ++i)
        rhi_texture_destroy(state->textures[i]);

    scene_destroy(&state->demo_scene);   // borrows scene_model, frees nothing but its own entity array
    model_destroy(&state->scene_model);
    mesh_destroy(&state->ground_mesh);
    material_destroy(&state->ground_material);
    model_destroy(&state->grid_model);
    ibl_destroy(&state->ibl);
    csm_destroy(&state->csm);
    rhi_shader_destroy(state->pbr_shader);

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