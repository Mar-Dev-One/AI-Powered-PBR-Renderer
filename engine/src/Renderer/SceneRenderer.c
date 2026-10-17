#include "SceneRenderer.h"

#include "../Core/Paths.h"
#include "Lighting.h"

#include <stdio.h>
#include <string.h>

#define PBR_VERTEX_PATH   "shaders/pbr.vert"
#define PBR_FRAGMENT_PATH "shaders/pbr.frag"

scene_render_settings
scene_render_settings_default(void)
{
    return (scene_render_settings){
        .ibl                  = NULL,
        .show_skybox          = true,
        .sky_blur             = 0.0f,
        .ambient_strength     = 0.5f,
        .exposure             = 1.0f,
        .bloom                = 0.04f,
        .fxaa                 = true,
        .shadows              = true,
        .cascade_split_lambda = 0.5f,
        .debug_show_cascades  = false,
        .debug_view           = SCENE_VIEW_LIT,
        .clear_color          = { 0.0f, 0.0f, 0.0f }
    };
}

static const char* const DEBUG_VIEW_NAMES[SCENE_VIEW_COUNT] = {
    "lit", "albedo", "normals", "roughness", "metallic", "ao", "shadow", "lighting", "wireframe"
};

const char*
scene_debug_view_name(scene_debug_view view)
{
    return (uint32)view < SCENE_VIEW_COUNT ? DEBUG_VIEW_NAMES[view] : NULL;
}

b8
scene_debug_view_from_name(const char* name, scene_debug_view* out_view)
{
    for (uint32 i = 0; name && i < SCENE_VIEW_COUNT; ++i)
    {
        if (strcmp(name, DEBUG_VIEW_NAMES[i]) == 0)
        {
            *out_view = (scene_debug_view)i;
            return true;
        }
    }
    return false;
}

// The views whose pixels are data, not light: shown without exposure, tone
// mapping or skybox (see scene_debug_view).
static b8
debug_view_is_data(scene_debug_view view)
{
    return view != SCENE_VIEW_LIT && view != SCENE_VIEW_LIGHTING;
}

void
scene_render_settings_apply_environment(scene_render_settings* settings, const scene_environment* env,
                                        const ibl_environment* ibl)
{
    settings->ibl              = env->use_ibl && ibl && ibl->irradiance ? ibl : NULL;
    settings->show_skybox      = env->show_skybox;
    settings->sky_blur         = env->sky_blur;
    settings->ambient_strength = env->ambient_strength;
    settings->exposure         = env->exposure;
    settings->bloom            = env->bloom;
}

// The constants pbr.vert/pbr.frag size their arrays with, generated from
// the C definitions they have to agree with -- see the #error guards at the
// top of both shaders.
static void
scene_renderer_shader_defines(char* out, size_t size)
{
    snprintf(out, size,
             "#define CSM_MAX_CASCADES %d\n"
             "#define MAX_LIGHTS %d\n"
             "#define LIGHT_DIRECTIONAL %d\n"
             "#define LIGHT_POINT %d\n"
             "#define LIGHT_SPOT %d\n"
             "#define DEBUG_VIEW_LIT %d\n"
             "#define DEBUG_VIEW_ALBEDO %d\n"
             "#define DEBUG_VIEW_NORMALS %d\n"
             "#define DEBUG_VIEW_ROUGHNESS %d\n"
             "#define DEBUG_VIEW_METALLIC %d\n"
             "#define DEBUG_VIEW_AO %d\n"
             "#define DEBUG_VIEW_SHADOW %d\n"
             "#define DEBUG_VIEW_LIGHTING %d\n"
             "#define DEBUG_VIEW_WIREFRAME %d\n",
             CSM_MAX_CASCADES, LIGHTING_MAX_LIGHTS,
             (int)LIGHT_DIRECTIONAL, (int)LIGHT_POINT, (int)LIGHT_SPOT,
             (int)SCENE_VIEW_LIT, (int)SCENE_VIEW_ALBEDO, (int)SCENE_VIEW_NORMALS, (int)SCENE_VIEW_ROUGHNESS,
             (int)SCENE_VIEW_METALLIC, (int)SCENE_VIEW_AO, (int)SCENE_VIEW_SHADOW, (int)SCENE_VIEW_LIGHTING,
             (int)SCENE_VIEW_WIREFRAME);
}

static rhi_shader
build_pbr_shader(void)
{
    char defines[1024];
    scene_renderer_shader_defines(defines, sizeof(defines));

    char* vertex_path   = engine_path(PBR_VERTEX_PATH);
    char* fragment_path = engine_path(PBR_FRAGMENT_PATH);

    rhi_shader shader = rhi_shader_create_from_files_with_defines(vertex_path, fragment_path, defines);

    free(vertex_path);
    free(fragment_path);

    return shader;
}

b8
scene_renderer_create(scene_renderer* r, uint16 width, uint16 height,
                      uint32 cascade_count, uint32 shadow_resolution)
{
    memset(r, 0, sizeof(*r));

    static const char* const pbr_files[]    = { PBR_VERTEX_PATH, PBR_FRAGMENT_PATH };
    static const char* const post_files[]   = { POST_VERTEX_PATH, POST_FRAGMENT_PATH };
    static const char* const bloom_files[]  = { POST_VERTEX_PATH, POST_BLOOM_DOWN_PATH, POST_BLOOM_UP_PATH };
    static const char* const fxaa_files[]   = { POST_VERTEX_PATH, POST_FXAA_PATH };
    static const char* const shadow_files[] = { CSM_VERTEX_PATH, CSM_FRAGMENT_PATH };
    file_watch_init(&r->pbr_files, pbr_files, 2);
    file_watch_init(&r->post_files, post_files, 2);
    file_watch_init(&r->bloom_files, bloom_files, 3);
    file_watch_init(&r->fxaa_files, fxaa_files, 2);
    file_watch_init(&r->shadow_files, shadow_files, 2);

    r->pbr_shader = build_pbr_shader();
    if (!r->pbr_shader)
    {
        LOG_ERROR("scene_renderer_create: failed to build the PBR shader\n");
        return false;
    }

    if (!csm_create(&r->csm, cascade_count, shadow_resolution))
        return false;

    if (!post_create(&r->post, width, height))
        return false;

    // Without timings the frame still renders: only the numbers are missing.
    if (!gpu_profiler_create(&r->profiler))
    {
        LOG_WARN("scene_renderer_create: no GPU timestamp queries, so no GPU timings\n");
        gpu_profiler_destroy(&r->profiler);
    }

    return true;
}

void
scene_renderer_resize(scene_renderer* r, uint16 width, uint16 height)
{
    post_resize(&r->post, width, height);
}

// World-space direction of the first LIGHT_DIRECTIONAL in world order --
// the light lighting_bind() marks as shadowed. False if there is none.
static b8
find_shadow_light(const world* w, vec3 out_direction)
{
    light  lights[LIGHTING_MAX_LIGHTS];
    uint32 count = world_gather_lights(w, lights, NULL, LIGHTING_MAX_LIGHTS);

    for (uint32 i = 0; i < count; ++i)
    {
        if (lights[i].type == LIGHT_DIRECTIONAL)
        {
            glm_vec3_copy(lights[i].direction, out_direction);
            return true;
        }
    }

    return false;
}

rhi_shader
scene_renderer_begin(scene_renderer* r, const world* w, const camera* cam,
                     const scene_render_settings* settings)
{
    // --- Shadow pass ---------------------------------------------------------
    // Before the HDR target is bound: csm_render() binds its own
    // framebuffers and leaves the default one bound when it's done.
    gpu_profiler_begin_frame(&r->profiler);

    vec3 shadow_dir;
    b8   shadowed = settings->shadows && find_shadow_light(w, shadow_dir);

    if (shadowed)
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "Shadows");
        csm_update(&r->csm, cam, shadow_dir, settings->cascade_split_lambda);
        csm_render(&r->csm, w);
        gpu_profiler_end(&r->profiler, scope);
    }

    // --- Scene target + background ----------------------------------------------
    post_begin_scene(&r->post, settings->clear_color[0], settings->clear_color[1], settings->clear_color[2]);

    mat4 view, projection, view_projection;
    camera_get_view(cam, view);
    camera_get_projection(cam, projection);
    glm_mat4_mul(projection, view, view_projection);

    if (settings->ibl && settings->show_skybox && !debug_view_is_data(settings->debug_view))
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "Sky");
        ibl_draw_skybox(settings->ibl, view, projection, settings->sky_blur);
        gpu_profiler_end(&r->profiler, scope);
    }

    // Until finish_frame(): the world's geometry only, not the passes around it.
    if (settings->debug_view == SCENE_VIEW_WIREFRAME)
        rhi_set_wireframe(true);

    // --- PBR shader -------------------------------------------------------------
    rhi_shader shader = r->pbr_shader;

    rhi_shader_bind(shader);
    rhi_shader_set_mat4(shader, "u_view_projection", (const f32*)view_projection);
    rhi_shader_set_mat4(shader, "u_view", (const f32*)view);   // pbr.vert: v_view_depth, for cascade selection
    rhi_shader_set_vec3(shader, "u_view_pos", cam->position[0], cam->position[1], cam->position[2]);
    rhi_shader_set_float(shader, "u_ambient_strength", settings->ambient_strength);
    rhi_shader_set_int(shader, "u_debug_show_cascades", settings->debug_show_cascades);
    rhi_shader_set_int(shader, "u_debug_view", (int)settings->debug_view);

    lighting_bind(shader, w);
    ibl_bind(settings->ibl, shader);

    // Always bound, even unshadowed: it points u_shadow_maps[] at their own
    // units. Left at the default unit 0, those sampler2DShadow uniforms would
    // share a unit with the material's sampler2D base color, which GL
    // rejects at draw time. u_cascade_count = 0 then turns shadowing off.
    csm_bind(&r->csm, shader);
    if (!shadowed)
        rhi_shader_set_int(shader, "u_cascade_count", 0);

    r->scene_scope = gpu_profiler_begin(&r->profiler, "Scene");
    return shader;
}

// Resolves the HDR target into `target` (NULL = the screen), the way
// settings->debug_view wants it.
static void
finish_frame(scene_renderer* r, const scene_render_settings* settings, rhi_framebuffer target)
{
    gpu_profiler_end(&r->profiler, r->scene_scope);
    rhi_set_wireframe(false);

    post_settings post = {
        .exposure = settings->exposure,
        .raw      = debug_view_is_data(settings->debug_view),
        .bloom    = settings->bloom,
        .fxaa     = settings->fxaa,
    };

    if (post.bloom > 0.0f && !post.raw)
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "Bloom");
        post_bloom(&r->post);
        gpu_profiler_end(&r->profiler, scope);
    }

    uint32 scope = gpu_profiler_begin(&r->profiler, "Post");
    post_end_scene_to(&r->post, &post, target);
    gpu_profiler_end(&r->profiler, scope);
}

void
scene_renderer_end(scene_renderer* r, const scene_render_settings* settings)
{
    finish_frame(r, settings, NULL);
}

void
scene_renderer_draw_world(const world* w, rhi_shader shader)
{
    const uint32          mask      = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    const model_draw_pass passes[2] = { MODEL_PASS_OPAQUE, MODEL_PASS_BLEND };

    for (int p = 0; p < 2; ++p)
    {
        for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
        {
            const c_mesh_renderer* mr = world_mesh_renderer(w, e);
            const model*           m  = assets_model(w->assets, mr->model);
            if (!m || !mr->visible)
                continue;

            model_draw_ex(m, shader, world_matrix(w, e), &mr->material, passes[p]);
        }
    }
}

void
scene_renderer_draw(scene_renderer* r, const world* w, const camera* cam,
                    const scene_render_settings* settings)
{
    rhi_shader shader = scene_renderer_begin(r, w, cam, settings);
    scene_renderer_draw_world(w, shader);
    scene_renderer_end(r, settings);
}

void
scene_renderer_draw_to(scene_renderer* r, const world* w, const camera* cam,
                       const scene_render_settings* settings, rhi_framebuffer target)
{
    rhi_shader shader = scene_renderer_begin(r, w, cam, settings);
    scene_renderer_draw_world(w, shader);
    finish_frame(r, settings, target);
    rhi_framebuffer_bind_default();
}

uint8*
scene_renderer_capture(scene_renderer* r, const world* w, const camera* cam,
                       const scene_render_settings* settings, uint16* out_width, uint16* out_height)
{
    uint16 width  = r->post.width;
    uint16 height = r->post.height;
    if (!width || !height)
        return NULL;

    // Screenshots are rare: a target per capture keeps nothing resident.
    rhi_framebuffer_attachment color  = { .format = RHI_FORMAT_RGBA8 };
    rhi_framebuffer            target = rhi_framebuffer_create((rhi_framebuffer_desc){
        .width                  = width,
        .height                 = height,
        .color_attachments      = &color,
        .color_attachment_count = 1,
    });

    rhi_shader shader = scene_renderer_begin(r, w, cam, settings);
    scene_renderer_draw_world(w, shader);
    finish_frame(r, settings, target);   // leaves `target` bound, to read

    uint8* pixels = malloc((size_t)width * height * 4);
    if (!pixels)
        FATAL("scene_renderer_capture: out of memory");
    rhi_read_pixels(0, 0, width, height, pixels);

    rhi_framebuffer_bind_default();
    rhi_framebuffer_destroy(target);

    *out_width  = width;
    *out_height = height;
    return pixels;
}

static b8
reload_bloom(scene_renderer* r)
{
    b8 down = shader_rebuild(&r->post.bloom_down_shader, POST_VERTEX_PATH, POST_BLOOM_DOWN_PATH, NULL);
    b8 up   = shader_rebuild(&r->post.bloom_up_shader, POST_VERTEX_PATH, POST_BLOOM_UP_PATH, NULL);
    return down && up;
}

static b8
reload_pbr(scene_renderer* r)
{
    char defines[1024];
    scene_renderer_shader_defines(defines, sizeof(defines));
    return shader_rebuild(&r->pbr_shader, PBR_VERTEX_PATH, PBR_FRAGMENT_PATH, defines);
}

// Every shader keeps its uniforms' values per program, and each pass sets
// all of its own every time it runs, so a swapped-in program needs nothing
// carried over.
b8
scene_renderer_reload_shaders(scene_renderer* r)
{
    b8 pbr    = reload_pbr(r);
    b8 post   = shader_rebuild(&r->post.tonemap_shader, POST_VERTEX_PATH, POST_FRAGMENT_PATH, NULL);
    b8 shadow = shader_rebuild(&r->csm.depth_shader, CSM_VERTEX_PATH, CSM_FRAGMENT_PATH, NULL);
    b8 bloom  = reload_bloom(r);
    b8 fxaa   = shader_rebuild(&r->post.fxaa_shader, POST_VERTEX_PATH, POST_FXAA_PATH, NULL);
    return pbr && post && shadow && bloom && fxaa;
}

void
scene_renderer_poll_hot_reload(scene_renderer* r)
{
    if (file_watch_changed(&r->pbr_files))
        reload_pbr(r);
    if (file_watch_changed(&r->post_files))
        shader_rebuild(&r->post.tonemap_shader, POST_VERTEX_PATH, POST_FRAGMENT_PATH, NULL);
    if (file_watch_changed(&r->bloom_files))
        reload_bloom(r);
    if (file_watch_changed(&r->fxaa_files))
        shader_rebuild(&r->post.fxaa_shader, POST_VERTEX_PATH, POST_FXAA_PATH, NULL);
    if (file_watch_changed(&r->shadow_files))
        shader_rebuild(&r->csm.depth_shader, CSM_VERTEX_PATH, CSM_FRAGMENT_PATH, NULL);
}

void
scene_renderer_destroy(scene_renderer* r)
{
    gpu_profiler_destroy(&r->profiler);
    post_destroy(&r->post);
    csm_destroy(&r->csm);

    if (r->pbr_shader)
        rhi_shader_destroy(r->pbr_shader);

    memset(r, 0, sizeof(*r));
}
